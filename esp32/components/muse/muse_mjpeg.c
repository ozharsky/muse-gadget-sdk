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

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_assets.h"
#include "muse_mjpeg.h"
#include "muse_ui.h"

static const char *TAG = "muse_mjpeg";

#define BLOB_MAX  (96 * 1024)     /* one 300..466 px JPEG comfortably fits */

static TaskHandle_t s_task;
static volatile bool s_run;
static char s_name[16];
static int s_loops;

bool muse_mjpeg_active(void)
{
    return s_task != NULL;
}

void muse_mjpeg_stop(void)
{
    s_run = false;   /* the task hides the image and deletes itself */
}

static void play_task(void *arg)
{
    (void)arg;
    muse_asset_t a;
    uint8_t *in = NULL, *out = NULL;
    jpeg_dec_handle_t dec = NULL;
    int frames_done = 0, loops_done = 0;
    int64_t t_first = 0;
    int out_w = 0, out_h = 0;

    if (!muse_assets_open(s_name, &a) || a.fmt != 2) {
        ESP_LOGW(TAG, "%s: not an MJPEG clip", s_name);
        goto done;
    }
    in = heap_caps_aligned_alloc(16, BLOB_MAX, MALLOC_CAP_SPIRAM);
    if (!in) {
        ESP_LOGW(TAG, "no input buffer");
        goto done;
    }
    /* BE is in the enum but this decoder answers INVALID_PARAM for it; decode LE
     * and swap, because the image path takes big-endian bytes. */
    jpeg_dec_config_t cfg = { .output_type = JPEG_PIXEL_FORMAT_RGB565_LE };
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "no decoder");
        goto done;
    }
    ESP_LOGI(TAG, "%s: %u frames at %u fps", s_name, a.frames, a.fps);
    t_first = esp_timer_get_time();
    TickType_t next = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(a.fps ? 1000 / a.fps : 40);

    while (s_run) {
        for (int i = 0; i < a.frames && s_run; i++) {
            size_t len = 0;
            if (!muse_assets_blob(&a, i, in, BLOB_MAX, &len)) {
                ESP_LOGW(TAG, "frame %d unreadable", i);
                goto done;
            }
            int64_t t0 = esp_timer_get_time();
            jpeg_dec_io_t io = { .inbuf = in, .inbuf_len = (int)len };
            jpeg_dec_header_info_t info = { 0 };
            if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
                ESP_LOGW(TAG, "frame %d: bad header", i);
                goto done;
            }
            if (!out) {
                out_w = info.width;
                out_h = info.height;
                /* esp_new_jpeg wants a 16-byte aligned output buffer. */
                out = jpeg_calloc_align((size_t)out_w * out_h * 2, 16);
                if (!out) {
                    ESP_LOGW(TAG, "no %dx%d output buffer", out_w, out_h);
                    goto done;
                }
            }
            /* Parse AND process on the same io: the decoder keeps the post-header
             * remainder in it, so a fresh io for process answers NO_MORE_DATA. */
            jpeg_dec_io_t run = { .inbuf = in, .inbuf_len = (int)len, .outbuf = out };
            jpeg_dec_header_info_t again = { 0 };
            jpeg_error_t derr = jpeg_dec_parse_header(dec, &run, &again);
            if (derr == JPEG_ERR_OK) {
                derr = jpeg_dec_process(dec, &run);
            }
            if (derr != JPEG_ERR_OK) {
                ESP_LOGW(TAG, "frame %d: decode failed (%d), blob %u B, head %02X %02X %02X %02X",
                         i, (int)derr, (unsigned)len, in[0], in[1], in[2], in[3]);
                goto done;
            }
            uint16_t *px = (uint16_t *)out;
            for (int k = 0; k < out_w * out_h; k++) {
                px[k] = __builtin_bswap16(px[k]);
            }
            const int x = (466 - out_w) / 2, y = (466 - out_h) / 2;
            muse_ui_image_draw(x < 0 ? 0 : x, y < 0 ? 0 : y, out_w, out_h, (const uint16_t *)out);
            frames_done++;
            if (frames_done == 1) {
                ESP_LOGI(TAG, "first frame in %lld ms (%dx%d)", (long long)((esp_timer_get_time() - t0) / 1000), out_w, out_h);
            }
            vTaskDelayUntil(&next, period);
        }
        loops_done++;
        if (s_loops >= 0 && loops_done >= s_loops) {
            break;
        }
    }
    if (frames_done) {
        int64_t dt = esp_timer_get_time() - t_first;
        ESP_LOGI(TAG, "%s done: %d frames in %lld ms (%lld fps avg), stack min %u B",
                 s_name, frames_done, (long long)(dt / 1000),
                 (long long)(frames_done * 1000000LL / (dt ? dt : 1)),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
done:
    if (dec) {
        jpeg_dec_close(dec);
    }
    heap_caps_free(in);
    if (out) {
        jpeg_free_align(out);
    }
    muse_ui_image_hide();   /* the face comes back */
    s_task = NULL;
    vTaskDelete(NULL);
}

bool muse_mjpeg_play(const char *name, int loops)
{
    if (!name || !name[0] || loops == 0) {
        return false;
    }
    if (!muse_assets_is_mjpeg(name)) {
        return false;
    }
    muse_mjpeg_stop();
    for (int i = 0; i < 80 && s_task; i++) {   /* let the old task finish */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    strncpy(s_name, name, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
    s_loops = loops;
    s_run = true;
    /* 4 KB overflowed inside esp_new_jpeg (panicked the device); 8 KB holds it. */
    if (xTaskCreate(play_task, "muse_mjpeg", 8192, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        s_run = false;
        return false;
    }
    return true;
}
