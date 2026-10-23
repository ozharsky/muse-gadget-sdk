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

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*
 * Buttons, as the board reports them. The talk button is push-to-talk. On touch
 * boards the aux button puts the screen to sleep; holding it 1.5 s powers off,
 * and two quick presses toggle BLE phone setup. Without touch it opens the
 * two-button menu instead (muse_menu.h), where talk selects. Either button
 * wakes from sleep; the talk button's press is also posted, marked `wake`, so
 * holding it on through waking records a note. Waking also retries Wi-Fi at
 * once if it's down. Also runs auto-sleep and refreshes battery status into
 * muse_state.
 */

typedef enum {
    MUSE_PTT_DOWN,
    MUSE_PTT_UP,
} muse_ptt_t;

typedef struct {
    muse_ptt_t type;
    bool wake;   /* a press that woke the screen: a tap only wakes */
} muse_input_event_t;

/* PTT events are posted to `queue` (items are muse_input_event_t). */
esp_err_t muse_input_start(QueueHandle_t queue);

/*
 * Console '>' lines go to hook first, without the '>'; it returns false for a
 * line that isn't its own. The app sets the SDK token this way
 * (main/sdk_token_console.h). Set it before muse_input_start().
 */
void muse_input_set_console_hook(bool (*hook)(const char *line));

/* LVGL microphone press/release edges; handled by the existing input task. */
void muse_input_touch_talk(bool pressed);

/* Plays the goodbye animation and powers off (from the input task). */
/* Wakes the input task out of a long wait_buttons()/delay so update_power() re-evaluates now.
 * Needed whenever another task changes sleep state (e.g. a notification waking the screen):
 * without it the input task can sit in a 10 s wait while the display stays paused, and the UI
 * stall watchdog then aborts. */
void muse_input_poke(void);

void muse_input_request_power_off(void);

/* Reboots the board (from the input task): the menu's Restart, the settings page,
 * and the console's ">reboot". */
void muse_input_request_restart(void);

/* Face-down "do not disturb": true while the gadget lies screen-down. */
bool muse_input_dnd(void);
