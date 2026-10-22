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
 * Clips from the flash asset partition (packed by tools/muse/assets_pack.py).
 *
 * The pack holds 64x64, 16-colour, 4-bit frames; frame 0 is raw and later
 * frames are runs of changed bytes, so a mostly-still animation costs a few
 * hundred bytes a frame instead of 2 KB. Everything is read from the partition
 * on demand -- no RAM copy of the clip, no reflash to swap content in.
 *
 * The player (avatar/muse_pixel_video.c) asks by name: a name that is not in
 * the compiled registry is looked up here, and played through the same canvas.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MUSE_ASSET_MAX_CLIPS 8

typedef struct {
    char name[16];
    uint16_t frames;
    uint8_t fps;
    uint8_t fmt;          /* 0 = u8 indices, 1 = 4-bit packed */
    uint8_t npal;
    uint16_t pal[16];
    uint32_t data_off;
    uint32_t data_len;
    uint32_t offs[251];   /* frames+1 offsets into the data block */
} muse_asset_t;

/* Finds and validates the "assets" partition. False when there is no pack. */
bool muse_assets_init(void);

int muse_assets_count(void);
const muse_asset_t *muse_assets_clip(int i);
/* Opens a clip by name into `out` (which must outlive playback). */
bool muse_assets_open(const char *name, muse_asset_t *out);

/* Decodes frame `fi` into out[W*H] palette indices (64*64 = 4096 bytes).
 * Sequential frames reuse the previous decode; a jump re-decodes from 0. */
bool muse_assets_frame(muse_asset_t *a, int fi, uint8_t *out, int out_len);

/* Bench (>assets): pack size, clip list, and a full-clip decode timing. */
void muse_assets_report(void);
