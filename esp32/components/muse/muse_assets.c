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

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"

#include "muse_assets.h"
#include "muse_mem.h"

static const char *TAG = "muse_assets";

#define MAGIC "MGA1"
#define PACK_HDR 16
#define ENTRY_HDR 64
#define FRAME_BYTES (64 * 64 / 2)      /* 4-bit packed */
#define DELTA_MAX (FRAME_BYTES * 5)    /* worst case: every byte its own run */

static const esp_partition_t *s_part;
static uint32_t s_pack_len;
static int s_count;
static EXT_RAM_BSS_ATTR muse_asset_t s_clips[MUSE_ASSET_MAX_CLIPS];
static EXT_RAM_BSS_ATTR uint8_t s_prev[FRAME_BYTES];
static EXT_RAM_BSS_ATTR uint8_t s_scratch[DELTA_MAX];
static muse_asset_t *s_prev_clip;
static int s_prev_index = -1;

bool muse_assets_init(void)
{
    if (s_part) {
        return true;
    }
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x42, "assets");
    if (!s_part) {
        return false;
    }
    uint8_t hdr[PACK_HDR];
    if (esp_partition_read(s_part, 0, hdr, sizeof(hdr)) != ESP_OK || memcmp(hdr, MAGIC, 4) != 0) {
        ESP_LOGW(TAG, "no valid asset pack in the assets partition");
        s_part = NULL;
        return false;
    }
    const uint8_t version = hdr[4];
    s_count = hdr[5];
    memcpy(&s_pack_len, hdr + 8, 4);
    if (version != 1 || s_count <= 0 || s_count > MUSE_ASSET_MAX_CLIPS) {
        ESP_LOGW(TAG, "asset pack version %u / %d clip(s): not supported", version, s_count);
        s_part = NULL;
        return false;
    }

    uint32_t off = PACK_HDR;
    for (int i = 0; i < s_count; i++) {
        muse_asset_t *c = &s_clips[i];
        uint8_t e[ENTRY_HDR];
        if (esp_partition_read(s_part, off, e, sizeof(e)) != ESP_OK) {
            s_count = i;
            break;
        }
        memcpy(c->name, e, 16);
        c->name[15] = '\0';
        memcpy(&c->frames, e + 16, 2);
        c->fps = e[18];
        c->fmt = e[19];
        c->npal = e[20];
        memcpy(c->pal, e + 24, sizeof(c->pal));
        memcpy(&c->data_off, e + 56, 4);
        memcpy(&c->data_len, e + 60, 4);
        if (c->frames == 0 || c->frames > 250 || c->fps == 0
            || (c->fmt != 2 && (c->npal == 0 || c->npal > 16))
            || c->data_off < off + ENTRY_HDR || c->data_off + c->data_len > s_pack_len) {
            ESP_LOGW(TAG, "clip %d is malformed; ignoring", i);
            s_count = i;
            break;
        }
        const size_t table = (size_t)(c->frames + 1) * 4;
        if (esp_partition_read(s_part, off + ENTRY_HDR, c->offs, table) != ESP_OK) {
            s_count = i;
            break;
        }
        off = c->data_off + c->data_len;
    }
    ESP_LOGI(TAG, "asset pack: %u bytes, %d clip(s)", (unsigned)s_pack_len, s_count);
    return s_count > 0;
}

int muse_assets_count(void)
{
    return s_count;
}

const muse_asset_t *muse_assets_clip(int i)
{
    return i >= 0 && i < s_count ? &s_clips[i] : NULL;
}

bool muse_assets_is_mjpeg(const char *name)
{
    if (!s_part && !muse_assets_init()) {
        return false;
    }
    for (int i = 0; i < s_count; i++) {
        if (strncmp(s_clips[i].name, name, sizeof(s_clips[i].name)) == 0) {
            return s_clips[i].fmt == 2;
        }
    }
    return false;
}

bool muse_assets_open(const char *name, muse_asset_t *out)
{
    if (!s_part && !muse_assets_init()) {
        return false;
    }
    for (int i = 0; i < s_count; i++) {
        if (strncmp(s_clips[i].name, name, sizeof(s_clips[i].name)) == 0) {
            *out = s_clips[i];
            return true;
        }
    }
    return false;
}

/* Reads one frame payload (raw for 0, delta runs after) into `dst`. */
static bool read_frame(const muse_asset_t *a, int fi, uint8_t *dst, size_t cap, size_t *got)
{
    if (fi < 0 || fi >= a->frames) {
        return false;
    }
    const uint32_t start = a->offs[fi];
    const uint32_t end = a->offs[fi + 1];
    if (end < start || end - start > cap) {
        return false;
    }
    *got = end - start;
    return esp_partition_read(s_part, a->data_off + start, dst, *got) == ESP_OK;
}

static void unpack(const muse_asset_t *a, const uint8_t *packed, uint8_t *out, int out_len)
{
    const int n = out_len < 64 * 64 ? out_len : 64 * 64;
    if (a->fmt == 1) {
        for (int y = 0; y < 64; y++) {
            const uint8_t *row = packed + (size_t)y * 32;
            uint8_t *o = out + (size_t)y * 64;
            for (int x = 0; x < 32; x++) {
                uint8_t b = row[x];
                uint8_t lo = b & 0x0F, hi = (uint8_t)((b >> 4) & 0x0F);
                o[x * 2] = lo < a->npal ? lo : 0;
                o[x * 2 + 1] = hi < a->npal ? hi : 0;
            }
        }
    } else {
        for (int i = 0; i < n; i++) {
            out[i] = packed[i] < a->npal ? packed[i] : 0;
        }
    }
}

bool muse_assets_frame(muse_asset_t *a, int fi, uint8_t *out, int out_len)
{
    if (!s_part || !a || fi < 0 || fi >= a->frames) {
        return false;
    }
    size_t got = 0;
    /* Sequential playback: one delta on top of the previous decode. A jump
     * (loops restarting at 0, a different clip) decodes from frame 0. */
    bool sequential = (a == s_prev_clip) && (fi == s_prev_index + 1);
    if (!sequential) {
        if (!read_frame(a, 0, s_prev, sizeof(s_prev), &got) || got != FRAME_BYTES) {
            return false;
        }
        s_prev_clip = a;
        s_prev_index = 0;
        if (fi == 0) {
            unpack(a, s_prev, out, out_len);
            return true;
        }
    }
    for (int i = s_prev_index + 1; i <= fi; i++) {
        if (!read_frame(a, i, s_scratch, sizeof(s_scratch), &got)) {
            return false;
        }
        size_t pos = 0, p = 0;
        while (p + 4 <= got) {
            uint16_t skip, len;
            memcpy(&skip, s_scratch + p, 2);
            memcpy(&len, s_scratch + p + 2, 2);
            p += 4;
            pos += skip;
            if (p + len > got || pos + len > FRAME_BYTES) {
                return false;
            }
            memcpy(s_prev + pos, s_scratch + p, len);
            p += len;
            pos += len;
        }
        s_prev_index = i;
    }
    unpack(a, s_prev, out, out_len);
    return true;
}

bool muse_assets_blob(muse_asset_t *a, int fi, uint8_t *dst, unsigned cap, size_t *got)
{
    if (!s_part || !a || fi < 0 || fi >= a->frames) {
        return false;
    }
    const uint32_t start = a->offs[fi];
    const uint32_t end = a->offs[fi + 1];
    if (end < start || end - start > cap) {
        return false;
    }
    *got = end - start;
    return esp_partition_read(s_part, a->data_off + start, dst, *got) == ESP_OK;
}

void muse_assets_report(void)
{
    if (!muse_assets_init()) {
        printf("@assets none (no pack in the assets partition)\n");
        return;
    }
    printf("@assets pack=%u B clips=%d\n", (unsigned)s_pack_len, s_count);
    for (int i = 0; i < s_count; i++) {
        const muse_asset_t *c = &s_clips[i];
        printf("@clip %-15s frames=%3u fps=%2u fmt=%s data=%5u B (%u B/frame)\n",
               c->name, c->frames, c->fps, c->fmt == 2 ? "mjpeg" : (c->fmt == 1 ? "i4" : "u8"),
               (unsigned)c->data_len, (unsigned)(c->data_len / c->frames));
    }
    /* Time a full pass of the first clip: read + delta + unpack. */
    const muse_asset_t *c = &s_clips[0];
    static uint8_t out[64 * 64];
    int64_t t0 = esp_timer_get_time();
    int ok = 0;
    for (int i = 0; i < c->frames; i++) {
        ok += muse_assets_frame((muse_asset_t *)c, i, out, sizeof(out)) ? 1 : 0;
    }
    int64_t dt = esp_timer_get_time() - t0;
    printf("@asset.bench %s %d/%u frames %lld us total, %lld us/frame (~%lld fps)\n",
           c->name, ok, c->frames, (long long)dt,
           (long long)(c->frames ? dt / c->frames : 0),
           (long long)(dt && c->frames ? (int64_t)c->frames * 1000000 / dt : 0));
}
