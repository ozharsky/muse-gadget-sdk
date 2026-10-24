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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Bring up the display and build the UI: the avatar on the first tile,
 * settings one swipe to the left. Also owns screen sleep and brightness.
 */
esp_err_t muse_ui_start(void);

/* From any task: the screen has gone dark for sleep (and not yet woken). */
bool muse_ui_dark(void);

/* The functions below run in the LVGL task (or with the display lock held). */

/* Slide back to the face (e.g. when a talk starts). */
void muse_ui_show_face(void);
/* Settings sub-pages turn off the tile swipe so they can use horizontal gestures. */
void muse_ui_set_swipe_enabled(bool enabled);
/* Temporarily applies a brightness while a slider is dragged. */
void muse_ui_preview_brightness(int pct);

/*
 * display.draw_url, from any task. An image covers the face until a tap, a
 * talk, the menu or muse_ui_image_hide(). Pixels are RGB565, high byte first.
 * The size is false without PSRAM for the image or before the UI is up.
 */
bool muse_ui_image_size(int *w, int *h);
bool muse_ui_image_draw(int x, int y, int w, int h, const uint16_t *pixels);
void muse_ui_image_hide(void);
/* Watcher camera mode: shows an on-screen shutter hint over the live image. */
void muse_ui_camera_hint(bool visible);

/*
 * Muse's own moves, from any task. muse_ui_react acts out a short mood on the avatar (smirk, scan,
 * alert, glitch, squint, angry, surprised, pleased); false if the avatar has none by that name.
 * muse_ui_choose asks a question with 2..4 tap choices and calls done once: with the index tapped,
 * or -1 timed out, -2 you spoke instead, -3 replaced by a newer question.
 */
bool muse_ui_react(const char *name);

/*
 * Face animation period, in ms (25 = 40 fps). Safe from any task: it only sets
 * a request that the LVGL task applies on its next frame. The input task sets
 * it from the power source (40 fps on USB, 25 on battery).
 */
void muse_ui_set_frame_ms(int ms);

/* A short full-screen animation when the agent is switched: rings sweep round, the new agent's name lights up. */
void muse_ui_agent_switch(bool hermes);
typedef void (*muse_ui_choice_cb)(int index, void *user);
bool muse_ui_choose(const char *prompt, const char *const *options, int n, int timeout_ms, muse_ui_choice_cb done,
                    void *user);
/* Answer the open choose dialog programmatically (bench / approvals). */
bool muse_ui_choice_answer(int idx);
bool muse_ui_choice_open(void);
/* The open card's choice count, and an answer that slides the card off like a swipe does
 * (right = the first choice, left = the others). */
int muse_ui_choice_count(void);
bool muse_ui_choice_answer_animated(int idx);

/* Bench testing, from any task: streams the screen over USB serial. */
void muse_ui_request_snapshot(void);
