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
 * MJPEG clips from the asset pack: the "cinematic" tier. A pack made with
 * `assets_pack.py --fmt mjpeg` holds full 300x300 (or any size) JPEG frames;
 * this player decodes one per frame period into RGB565 and draws it centred
 * through the image path (muse_ui_image_draw), which owns the screen buffer.
 *
 * Measured decode on this chip: 300 px = ~18 ms, so 20-25 fps is comfortable.
 * 466 px (40.8 ms) is deliberately not offered: it cannot hold 30 fps.
 *
 * Ownership: the player runs its own task; the image buffer and LVGL stay in
 * the UI task's hands (image_draw takes the mutex, image_hide takes the display
 * lock). Stopping always hides the image, so the face comes back.
 */

#pragma once

#include <stdbool.h>

/* Starts a clip (loops < 0 = until stopped). False if the pack has no such clip. */
bool muse_mjpeg_play(const char *name, int loops);

/* Stops the player and gives the face back. Safe from any task. */
void muse_mjpeg_stop(void);

bool muse_mjpeg_active(void);
