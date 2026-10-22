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
 * How fast can this chip decode a JPEG, really? The firmware links
 * esp_new_jpeg (pulled in with the BSP) but has never used it. The answer
 * decides whether a video tier (MJPEG) is worth designing: the panel is 466 px
 * and 30 fps needs <= 33 ms per frame including everything else.
 *
 * >jpeg.bench decodes three embedded test images (232, 300 and 466 px) eight
 * times each into RGB565 and prints avg/min, plus the implied fps. The images
 * are testsrc2 on purpose: flat colour would flatter the decoder.
 *
 * Decode loop: a FRESH io + header parse per run, like an MJPEG stream (every
 * frame carries its own header). The decoder keeps the post-header remainder in
 * inbuf_remain; zeroing it (a natural mistake) makes every decode answer
 * NO_MORE_DATA.
 */
#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "esp_jpeg_dec.h"

#define RUNS 8

typedef struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
} jpg_t;

extern const uint8_t bench232_jpg_start[] asm("_binary_bench232_jpg_start");
extern const uint8_t bench232_jpg_end[] asm("_binary_bench232_jpg_end");
extern const uint8_t bench300_jpg_start[] asm("_binary_bench300_jpg_start");
extern const uint8_t bench300_jpg_end[] asm("_binary_bench300_jpg_end");
extern const uint8_t bench466_jpg_start[] asm("_binary_bench466_jpg_start");
extern const uint8_t bench466_jpg_end[] asm("_binary_bench466_jpg_end");

static void one(const jpg_t *f)
{
    jpeg_dec_config_t cfg = {
        .output_type = JPEG_PIXEL_FORMAT_RGB565_LE,
    };
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        printf("@jpeg %s: decoder open failed\n", f->name);
        return;
    }
    const int in_len = (int)(f->end - f->start);

    jpeg_dec_io_t io = { .inbuf = (uint8_t *)f->start, .inbuf_len = in_len };
    jpeg_dec_header_info_t info = { 0 };
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        printf("@jpeg %s: header parse failed (%d bytes)\n", f->name, in_len);
        jpeg_dec_close(dec);
        return;
    }
    int out_len = (int)info.width * info.height * 2;   /* RGB565 */
    if (out_len <= 0) {
        printf("@jpeg %s: no output size\n", f->name);
        jpeg_dec_close(dec);
        return;
    }
    uint8_t *out = jpeg_calloc_align(out_len, 16);
    if (!out) {
        printf("@jpeg %s: %d-byte output buffer alloc failed\n", f->name, out_len);
        jpeg_dec_close(dec);
        return;
    }

    int64_t sum = 0, best = 0;
    int ok = 0;
    for (int i = 0; i < RUNS; i++) {
        jpeg_dec_io_t run = { .inbuf = (uint8_t *)f->start, .inbuf_len = in_len, .outbuf = out };
        jpeg_dec_header_info_t again = { 0 };
        int64_t t0 = esp_timer_get_time();
        jpeg_error_t err = jpeg_dec_parse_header(dec, &run, &again);
        if (err == JPEG_ERR_OK) {
            err = jpeg_dec_process(dec, &run);
        }
        int64_t dt = esp_timer_get_time() - t0;
        if (err != JPEG_ERR_OK) {
            printf("@jpeg %s: decode failed (%d) on run %d\n", f->name, (int)err, i);
            break;
        }
        sum += dt;
        if (!best || dt < best) {
            best = dt;
        }
        ok++;
    }
    if (ok) {
        int64_t avg = sum / ok;
        printf("@jpeg %s %ux%u jpeg=%d B rgb565=%d B avg=%lld us min=%lld us (~%lld fps)\n",
               f->name, info.width, info.height, in_len, out_len,
               (long long)avg, (long long)best, (long long)(avg ? 1000000 / avg : 0));
    }
    jpeg_free_align(out);
    jpeg_dec_close(dec);
}

void muse_jpeg_bench(void)
{
    const jpg_t files[] = {
        { "232", bench232_jpg_start, bench232_jpg_end },
        { "300", bench300_jpg_start, bench300_jpg_end },
        { "466", bench466_jpg_start, bench466_jpg_end },
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        one(&files[i]);
    }
}
