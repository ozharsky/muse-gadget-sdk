/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sdkconfig.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_log.h"
#include "muse_mem.h"

static const char *TAG = "muse_log";

#define PART_LABEL "logs"
#define PART_SUB   0x43
#define BLOCK      8192
#define BLOCKS     63                 /* data blocks; block 0 is the header */
#define RAM_RING   32768
#define QSIZE      8192               /* W/E bytes waiting for flash */
#define REC_MAX    4096

static const esp_partition_t *s_part;
static uint32_t s_off;                /* 4-byte aligned write offset in the block */
static int s_blk = 1;                 /* current data block number */

static EXT_RAM_BSS_ATTR uint8_t s_ram[RAM_RING];
static size_t s_ram_head, s_ram_tail;
static EXT_RAM_BSS_ATTR uint8_t s_queue[QSIZE];
static size_t s_q_head, s_q_tail;
static EXT_RAM_BSS_ATTR uint8_t s_rec[REC_MAX + 8];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_wake;
static SemaphoreHandle_t s_flash_mutex;
static uint32_t s_dropped;

static vprintf_like_t s_prev_vprintf;

/* ---- RAM ring (everything) ---- */

static void ram_put(const char *buf, size_t len)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    for (size_t i = 0; i < len; i++) {
        s_ram[s_ram_head] = (uint8_t)buf[i];
        s_ram_head = (s_ram_head + 1) % RAM_RING;
        if (s_ram_head == s_ram_tail) {
            s_ram_tail = (s_ram_tail + 1) % RAM_RING;   /* overwrite the oldest */
        }
    }
    portEXIT_CRITICAL_SAFE(&s_lock);
}

static void q_put(const char *buf, size_t len)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    size_t free_bytes = (s_q_tail + QSIZE - s_q_head - 1) % QSIZE;
    if (len > free_bytes) {
        s_dropped += (uint32_t)(len - free_bytes);
        len = free_bytes;
    }
    for (size_t i = 0; i < len; i++) {
        s_queue[s_q_head] = (uint8_t)buf[i];
        s_q_head = (s_q_head + 1) % QSIZE;
    }
    portEXIT_CRITICAL_SAFE(&s_lock);
    if (s_wake) {
        xSemaphoreGive(s_wake);
    }
}

static size_t ring_take(uint8_t *dst, size_t max)
{
    size_t n = 0;
    portENTER_CRITICAL_SAFE(&s_lock);
    while (n < max && s_q_tail != s_q_head) {
        dst[n++] = s_queue[s_q_tail];
        s_q_tail = (s_q_tail + 1) % QSIZE;
    }
    portEXIT_CRITICAL_SAFE(&s_lock);
    return n;
}

/* ---- flash ring ---- */

static void header_write(void)
{
    uint8_t h[16] = { 'M', 'U', 'L', 'G', '1', 0, 0, 0 };
    memcpy(h + 8, &s_blk, 4);
    memcpy(h + 12, &s_off, 4);
    esp_partition_erase_range(s_part, 0, BLOCK);
    esp_partition_write(s_part, 0, h, sizeof(h));
}

static void advance_block(void)
{
    s_blk = s_blk >= BLOCKS ? 1 : s_blk + 1;
    s_off = 0;
    esp_partition_erase_range(s_part, (size_t)s_blk * BLOCK, BLOCK);
    header_write();
}

/* Appends as [u16 len][bytes][pad to 4]: one aligned write per record. */
static void flash_append(const uint8_t *data, size_t len)
{
    if (s_flash_mutex) {
        xSemaphoreTake(s_flash_mutex, portMAX_DELAY);
    }
    while (len) {
        size_t n = len > REC_MAX ? REC_MAX : len;
        size_t rec = (n + 2 + 3) & ~(size_t)3;
        if (rec > BLOCK - s_off) {
            /* Pad the block's tail (4 bytes of 0xFFFF marks the end) and move on. */
            static const uint8_t pad[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
            esp_partition_write(s_part, (size_t)s_blk * BLOCK + s_off, pad, 4);
            advance_block();
            continue;
        }
        s_rec[0] = (uint8_t)(n & 0xFF);
        s_rec[1] = (uint8_t)(n >> 8);
        memcpy(s_rec + 2, data, n);
        if (rec > n + 2) {
            memset(s_rec + 2 + n, 0xFF, rec - n - 2);
        }
        esp_partition_write(s_part, (size_t)s_blk * BLOCK + s_off, s_rec, rec);
        s_off += (uint32_t)rec;
        len -= n;
        data += n;
    }
    if (s_flash_mutex) {
        xSemaphoreGive(s_flash_mutex);
    }
}

static void flush_task(void *arg)
{
    (void)arg;
    static EXT_RAM_BSS_ATTR uint8_t buf[REC_MAX];
    for (;;) {
        xSemaphoreTake(s_wake, pdMS_TO_TICKS(2000));
        if (!s_part) {
            continue;
        }
        size_t n;
        while ((n = ring_take(buf, sizeof(buf))) > 0) {
            flash_append(buf, n);
        }
    }
}

/* ---- the vprintf hook ---- */

static int log_hook(const char *fmt, va_list args)
{
    char line[320];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    int out = s_prev_vprintf ? s_prev_vprintf(fmt, args) : 0;
    if (n <= 0) {
        return out;
    }
    size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
    ram_put(line, len);
    /* W/E only: Wi-Fi's INFO chatter would wear the flash out for nothing. */
    if ((line[0] == 'W' || line[0] == 'E') && line[1] == ' ') {
        q_put(line, len);
    }
    return out;
}

void muse_log_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, PART_SUB, PART_LABEL);
    if (!s_part) {
        ESP_LOGW(TAG, "no logs partition: history stays in RAM");
        s_prev_vprintf = esp_log_set_vprintf(log_hook);
        s_wake = xSemaphoreCreateBinary();
        if (s_wake) {
            xTaskCreate(flush_task, "muse_log", 3072, NULL, 2, NULL);
        }
        return;
    }
    uint8_t h[16];
    bool ok = esp_partition_read(s_part, 0, h, sizeof(h)) == ESP_OK
              && memcmp(h, "MULG1", 5) == 0;
    if (ok) {
        memcpy(&s_blk, h + 8, 4);
        memcpy(&s_off, h + 12, 4);
        if (s_blk < 1 || s_blk > BLOCKS || s_off > BLOCK) {
            ok = false;
        }
    }
    if (!ok) {
        s_blk = 1;
        s_off = 0;
        for (int i = 1; i <= BLOCKS; i++) {
            esp_partition_erase_range(s_part, (size_t)i * BLOCK, BLOCK);
        }
        header_write();
    }
    s_flash_mutex = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    if (s_wake) {
        xTaskCreate(flush_task, "muse_log", 3072, NULL, 2, NULL);
    }
    s_prev_vprintf = esp_log_set_vprintf(log_hook);
    ESP_LOGI(TAG, "log ring: %d KB RAM + %d x %d B flash (block %d off %u)",
             RAM_RING / 1024, BLOCKS, BLOCK, s_blk, (unsigned)s_off);
}

/* ---- dump ---- */

static void print_span(size_t from, size_t to)
{
    for (size_t i = from; i < to; i++) {
        putchar(s_ram[i]);
    }
}

void muse_log_ram_dump(int kb)
{
    size_t total = (s_ram_head + RAM_RING - s_ram_tail) % RAM_RING;
    size_t want = kb > 0 ? (size_t)kb * 1024 : total;
    size_t from = s_ram_tail;
    if (want < total) {
        from = (from + (total - want)) % RAM_RING;
        total = want;
    }
    printf("@log.ram %u bytes\n", (unsigned)total);
    if (from < s_ram_head && s_ram_head - from == total) {
        print_span(from, s_ram_head);
    } else {
        print_span(from, RAM_RING);
        print_span(0, s_ram_head);
    }
    printf("\n@end.log.ram\n");
}

void muse_log_save(void)
{
    if (!s_part) {
        printf("@log.save no partition\n");
        return;
    }
    size_t total = (s_ram_head + RAM_RING - s_ram_tail) % RAM_RING;
    printf("@log.save %u bytes\n", (unsigned)total);
    size_t done = 0;
    while (done < total) {
        size_t n = 0;
        size_t take = total - done < sizeof(s_rec) ? total - done : sizeof(s_rec);
        while (n < take) {
            s_rec[n++] = s_ram[(s_ram_tail + done) % RAM_RING];
            done++;
        }
        flash_append(s_rec, n);
    }
}

void muse_log_dump(int kb)
{
    if (!s_part) {
        printf("@log.dump no partition\n");
        return;
    }
    size_t budget = kb > 0 ? (size_t)kb * 1024 : (size_t)BLOCKS * BLOCK;
    static EXT_RAM_BSS_ATTR uint8_t blk[BLOCK];
    printf("@log.dump flash ring\n");
    size_t emitted = 0;
    for (int k = 1; k <= BLOCKS && emitted < budget; k++) {
        int b = s_blk + k;
        b = b > BLOCKS ? b - BLOCKS : b;
        if (esp_partition_read(s_part, (size_t)b * BLOCK, blk, BLOCK) != ESP_OK) {
            continue;
        }
        size_t limit = b == s_blk ? s_off : BLOCK;
        size_t p = 0;
        while (p + 2 <= limit) {
            uint16_t len;
            memcpy(&len, blk + p, 2);
            if (len == 0xFFFF || len == 0 || len > REC_MAX || p + 2 + len > limit) {
                break;
            }
            for (size_t i = 0; i < len; i++) {
                putchar(blk[p + 2 + i]);
            }
            emitted += len;
            p += (2 + (size_t)len + 3) & ~(size_t)3;
        }
    }
    printf("\n@end.log.dump (%u bytes, %u dropped)\n", (unsigned)emitted, (unsigned)s_dropped);
}

void muse_log_clear(void)
{
    if (!s_part) {
        printf("@log.clear no partition\n");
        return;
    }
    if (s_flash_mutex) {
        xSemaphoreTake(s_flash_mutex, portMAX_DELAY);
    }
    s_blk = 1;
    s_off = 0;
    for (int i = 1; i <= BLOCKS; i++) {
        esp_partition_erase_range(s_part, (size_t)i * BLOCK, BLOCK);
    }
    header_write();
    if (s_flash_mutex) {
        xSemaphoreGive(s_flash_mutex);
    }
    printf("@log.clear ok\n");
}
