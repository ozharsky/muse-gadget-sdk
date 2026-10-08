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

/*
 * The gadget's black box: WHO restarted the board and what the display looked
 * like just before. The screen freezes happen OFF-USB, and a reboot tears the
 * USB serial link down with it -- console logs from that window are lost, and
 * the flash coredump only exists when a panic aborted (a clean esp_restart
 * leaves none). These records are written to NVS, which survives both:
 *
 *   restart_by    written by whoever is about to restart.
 *   restart_last  restart_by, moved here at the next boot and logged -- so
 *                 @status can answer "why did it restart?" at ANY later time,
 *                 even when the boot itself happened off-USB.
 *   lcd_fault     a small snapshot of the display's health at the moment of
 *                 the last restart (counters + RAM + a detail string).
 *
 * All best-effort: a failure here must never take the device down.
 */

#include "muse_diag.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_mem.h"

static const char *TAG = "muse_diag";
#define NS "muse"

#define KEY_RESTART      "restart_by"
#define KEY_RESTART_LAST "restart_last"
#define KEY_FAULT        "lcd_fault"

/* The packed display snapshot. Kept tiny: it lives in one NVS value. */
typedef struct {
    uint32_t magic;          /* DIAG_FAULT_MAGIC when valid */
    uint32_t uptime_s;       /* device uptime when the snapshot was taken */
    uint16_t chunks_out;     /* pieces the send task still owed */
    uint16_t recover_count;  /* graceful recoveries this boot */
    uint16_t driver_ms;      /* age of the stuck driver call, 0 = none */
    uint16_t free_int_kb;    /* internal RAM free at the time */
    uint16_t min_int_kb;     /* internal RAM low-water mark */
    char     detail[40];     /* free text: why it was recorded */
} diag_fault_t;

static bool s_ready;

esp_err_t muse_diag_init(void)
{
    s_ready = true;
    ESP_LOGI(TAG, "black box ready (restart reason persists across reboots)");
    return ESP_OK;
}
/* Move restart_by -> restart_last, so the reason outlives the boot that reads it. */
void muse_diag_boot_report(void)
{
    /* The hardware's own verdict on how the last session ended: this catches the
     * panics no guard wrote a reason for (a crash at t=1s in the Wi-Fi bring-up
     * window cannot record anything before it dies -- but the NEXT boot sees
     * reset=panic here, with restart_by empty). */
    const char *reset = "other";
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   reset = "poweron"; break;
    case ESP_RST_SW:        reset = "sw"; break;
    case ESP_RST_PANIC:     reset = "panic"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       reset = "watchdog"; break;
    case ESP_RST_DEEPSLEEP: reset = "deepsleep"; break;
    case ESP_RST_BROWNOUT:  reset = "brownout"; break;
    case ESP_RST_USB:       reset = "usb"; break;
    case ESP_RST_JTAG:      reset = "jtag"; break;
    default: break;
    }

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char why[24] = "";
    size_t n = sizeof(why);
    if (nvs_get_str(h, KEY_RESTART, why, &n) == ESP_OK && why[0]) {
        nvs_set_str(h, KEY_RESTART_LAST, why);
        nvs_erase_key(h, KEY_RESTART);
        nvs_commit(h);
        ESP_LOGW(TAG, "previous boot ended by: %s (reset: %s)", why, reset);
    } else if (esp_reset_reason() == ESP_RST_PANIC || esp_reset_reason() == ESP_RST_INT_WDT ||
               esp_reset_reason() == ESP_RST_TASK_WDT || esp_reset_reason() == ESP_RST_WDT) {
        /* No guard wrote a reason and the hardware says it crashed: this is the
         * un-instrumented class (boot-window Wi-Fi race, LVGL assert, ...). */
        char line[32];
        snprintf(line, sizeof(line), "unexplained_%s", reset);
        nvs_set_str(h, KEY_RESTART_LAST, line);
        nvs_commit(h);
        ESP_LOGE(TAG, "previous boot CRASHED (reset: %s) with no guard reason -- unexplained", reset);
    } else {
        ESP_LOGI(TAG, "boot: reset=%s, no guard restart recorded", reset);
    }
    nvs_close(h);

    char detail[40];
    uint32_t up = 0;
    int counts[5] = { 0 };
    if (muse_diag_read_lcd_fault(detail, sizeof(detail), &up, counts)) {
        ESP_LOGW(TAG, "last display fault at t=%us: %s (chunks=%d rec=%d drv=%dms int=%dK min=%dK)",
                 (unsigned)up, detail, counts[0], counts[1], counts[2], counts[3], counts[4]);
    }
}

void muse_diag_note_restart(const char *why)
{
    if (!s_ready || !why || !*why) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    esp_err_t err = nvs_set_str(h, KEY_RESTART, why);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not record restart reason '%s': %s", why, esp_err_to_name(err));
    }
    nvs_close(h);
}

bool muse_diag_read_restart(char *out, size_t cap)
{
    if (!out || cap == 0) {
        return false;
    }
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = cap;
    /* The most recent answered question first: this boot followed restart_by if
     * one is pending, otherwise the last reported one. */
    esp_err_t err = nvs_get_str(h, KEY_RESTART, out, &n);
    if (err != ESP_OK) {
        n = cap;
        err = nvs_get_str(h, KEY_RESTART_LAST, out, &n);
    }
    nvs_close(h);
    return err == ESP_OK && out[0] != '\0';
}

static void pack_fault(diag_fault_t *f, const char *detail, const muse_diag_lcd_t *lcd)
{
    memset(f, 0, sizeof(*f));
    f->magic = DIAG_FAULT_MAGIC;
    f->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    if (lcd) {
        f->chunks_out = lcd->chunks_out > 0xFFFF ? 0xFFFF : (uint16_t)lcd->chunks_out;
        f->recover_count = lcd->recover_count > 0xFFFF ? 0xFFFF : (uint16_t)lcd->recover_count;
        f->driver_ms = lcd->driver_ms > 0xFFFF ? 0xFFFF : (uint16_t)lcd->driver_ms;
    }
    f->free_int_kb = (uint16_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    f->min_int_kb = (uint16_t)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
    if (detail) {
        strncpy(f->detail, detail, sizeof(f->detail) - 1);
    }
}

void muse_diag_note_lcd_fault(const char *detail, const muse_diag_lcd_t *lcd)
{
    if (!s_ready) {
        return;
    }
    diag_fault_t f;
    pack_fault(&f, detail, lcd);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, KEY_FAULT, &f, sizeof(f));   /* best effort */
    nvs_commit(h);
    nvs_close(h);
}

void muse_diag_note_fault_restart(const char *detail, const muse_diag_lcd_t *lcd, const char *why)
{
    if (!s_ready) {
        return;
    }
    diag_fault_t f;
    pack_fault(&f, detail, lcd);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    /* One NVS session for both records: this path ends in esp_restart(). */
    nvs_set_blob(h, KEY_FAULT, &f, sizeof(f));
    if (why && *why) {
        nvs_set_str(h, KEY_RESTART, why);
    }
    nvs_commit(h);
    nvs_close(h);
}

bool muse_diag_read_lcd_fault(char *detail_out, size_t cap, uint32_t *uptime_s, int *counts)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    diag_fault_t f = { 0 };
    size_t n = sizeof(f);
    esp_err_t err = nvs_get_blob(h, KEY_FAULT, &f, &n);
    nvs_close(h);
    if (err != ESP_OK || f.magic != DIAG_FAULT_MAGIC) {
        return false;
    }
    if (detail_out && cap) {
        strncpy(detail_out, f.detail, cap - 1);
        detail_out[cap - 1] = '\0';
    }
    if (uptime_s) {
        *uptime_s = f.uptime_s;
    }
    if (counts) {
        counts[0] = f.chunks_out;
        counts[1] = f.recover_count;
        counts[2] = f.driver_ms;
        counts[3] = f.free_int_kb;
        counts[4] = f.min_int_kb;
    }
    return true;
}
