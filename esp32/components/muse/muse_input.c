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

#include "muse_input.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"     /* esp_restart: the restart paths */
#include "esp_timer.h"      /* the talk-button hold timer */
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

#include "muse_battery.h"
#include "muse_ble.h"
#include "muse_board.h"
#include "muse_chat.h"
#include "muse_console.h"
#include "muse_link.h"
#include "muse_mem.h"
#include "muse_menu.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_WATCHER_CAMERA
#include "boards/watcher_camera.h"
#endif

/* Bench hook: "draw=URL" draws an image the way Muse's display.draw_url command does
 * (main/image_fetch.c; the layout below mirrors image_fetch_result_t). */
typedef struct {
    bool ok;
    const char *code, *message, *format;
    int width, height, scale;
    size_t bytes;
    int ms;
} draw_result_t;
extern bool image_fetch_start(const char *url, int row, void (*done)(const draw_result_t *, void *), void *user,
                              const char **code, const char **message);

static bool bench_ui_command(const char *line);

/* muse_chat_session.cpp's reverse-channel hook (defined there with C linkage): opens the agent
 * that raised a waiting notification. Used by the face tap and the bench ">notif.tap". */
bool muse_notif_open_pending(void);

static void draw_done(const draw_result_t *r, void *user)
{
    (void)user;
    printf("@draw %s %s %dx%d %u bytes %d ms\n", r->ok ? "ok" : "FAILED", r->ok ? r->format : r->message, r->width,
           r->height, (unsigned)r->bytes, r->ms);
    fflush(stdout);
}



static const char *TAG = "muse_input";

#define POLL_MS 10
#define REST_POLL_MS 50        /* screen off and paused: still quick to wake */
#define REST_WAIT_MS 1000      /* paused, buttons that interrupt: the rest waits this long */
#define NAP_WAIT_MS 10000      /* ... and Wi-Fi napping */
#define POWER_MS 2000          /* refresh battery */
#define REST_POWER_MS 10000    /* ... while paused */
#define WIFI_NAP_MS (2 * 60 * 1000)   /* low power this long: Wi-Fi off until it ends */
#define DOUBLE_TICKS 35        /* 350 ms: a second aux press within this toggles phone setup */

#define GOODBYE_MS 1500        /* let the goodbye animation play */
#define HOLD_RESTART_MS 20000  /* talk-button hold: in-hand restart when the screen freezes.
                                * A voice note already stops at 15 s (MAX_SECS), so a hold
                                * this long is unambiguous. This board has NO reset key --
                                * only BOOT and the (hardware-only) PWR switch. */
#define HINT_TICKS 60          /* 0.6 s: warn that holding powers off */
#define LONG_TICKS 150         /* 1.5 s: power off */
#define SLEEP_CHECK_MS 100

#define SERIAL_RX 1024         /* the driver drops what doesn't fit, so a console line must */
#define SERIAL_LINE 1024
#define CHAT_MAX (192 * 1024)  /* a typed message, assembled from "chat+=" lines */

static QueueHandle_t s_queue;
static TaskHandle_t s_input;
static bool s_talk_down;
static bool s_cpu_low;      /* display stopped and the CPU allowed to sleep */
static volatile bool s_power_off_requested;
static volatile bool s_restart_requested;   /* menu / console / hold: reboot the board */
static volatile bool s_nap_now;   /* ">nap": asleep, as if on battery, nap without waiting WIFI_NAP_MS */

static void post(muse_ptt_t type, bool wake)
{
    muse_input_event_t ev = { .type = type, .wake = wake };
    ESP_LOGI(TAG, "PTT %s%s", type == MUSE_PTT_DOWN ? "down" : "up", wake ? " (waking)" : "");
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        /* A dropped press would leave the task waiting for an UP that never comes (or
         * worse, an UP with its DOWN gone); flush the stale counterpart and log it. */
        ESP_LOGW(TAG, "PTT queue full: dropping %s", type == MUSE_PTT_DOWN ? "down" : "up");
        muse_input_event_t stale;
        while (xQueueReceive(s_queue, &stale, 0) == pdTRUE) {
        }
    }
}

static bool update_power(void);

static void power_off(void)
{
    ESP_LOGI(TAG, "shutting down");
    muse_state_set_asleep(false);
    update_power();
    muse_state_set_progress(0);
    muse_state_set_level(0);
    muse_state_set_mode(MUSE_MODE_OFF);
    muse_state_set_caption("GOODBYE!");
    vTaskDelay(pdMS_TO_TICKS(GOODBYE_MS));
    esp_err_t err = muse_board->power_off();
    vTaskDelay(pdMS_TO_TICKS(500));
    if (err == ESP_OK) {
        /* This board returns from power-off: the physical PWR button owns real power
         * (hardware-only, the firmware can't read it). The screen is asleep; say so. */
        muse_state_set_mode(MUSE_MODE_IDLE);
        muse_state_set_caption("USE THE POWER BUTTON");
        return;
    }
    ESP_LOGE(TAG, "power-off failed (%s)", esp_err_to_name(err));
    muse_state_set_mode(MUSE_MODE_IDLE);
    muse_state_set_caption("COULDN'T POWER OFF");
}

static void set_asleep(bool asleep, const char *why)
{
    if (asleep != muse_state_asleep()) {
        ESP_LOGI(TAG, "%s (%s)", asleep ? "sleeping" : "waking", why);
        muse_state_set_asleep(asleep);
        if (s_input) {
            xTaskNotifyGive(s_input);   /* out of wait_buttons() */
        }
        if (!asleep && !muse_wifi_connected()) {
            muse_wifi_apply();   /* the screen says reconnecting: don't sit out the backoff */
        }
    }
}

static void toggle_phone_setup(void)
{
    bool on = !muse_settings_ble_on();
    ESP_LOGI(TAG, "phone setup %s", on ? "on" : "off");
    muse_settings_set_ble_on(on);
    muse_ble_status_t b;
    muse_ble_status(&b);
    if (on && b.name[0]) {
        muse_state_set_caption("PHONE SETUP: %s", b.name);
    } else {
        muse_state_set_caption("PHONE SETUP %s", on ? "ON" : "OFF");
    }
}

/*
 * Aux button: short press sleeps, a 1.5 s hold powers off, any press wakes.
 * Two quick presses toggle BLE phone setup, so the sleep waits a moment to
 * see whether a second press follows.
 */
static void aux_button(bool pressed, bool edge)
{
    static int held;
    static bool swallow;
    static bool hinted;
    static int sleep_in;    /* ticks until a pending single press sleeps */
    static char saved_caption[64];

    if (sleep_in && --sleep_in == 0) {
        set_asleep(true, muse_board->aux_button);
    }
    if (edge && pressed) {
        held = 0;
        hinted = false;
        swallow = muse_state_asleep();
        if (swallow) {
            set_asleep(false, muse_board->aux_button);
        } else if (sleep_in) {
            sleep_in = 0;
            swallow = true;
            toggle_phone_setup();
        }
        return;
    }
    if (pressed && !swallow) {
        held++;
        if (held == HINT_TICKS) {
            uint32_t v = UINT32_MAX;
            muse_state_caption(saved_caption, sizeof(saved_caption), &v);
            muse_state_set_caption("HOLD TO POWER OFF");
            hinted = true;
        } else if (held == LONG_TICKS) {
            swallow = true;
            power_off();
        }
        return;
    }
    if (edge && !pressed && !swallow) {
        if (hinted) {
            muse_state_set_caption("%s", saved_caption);   /* let go early: cancel */
        } else {
            sleep_in = DOUBLE_TICKS;
        }
    }
}

/* No touch: the aux button opens the menu and steps down it; any press wakes. */
static void menu_button(bool pressed, bool edge)
{
    if (!edge || !pressed) {
        return;
    }
    if (muse_state_asleep()) {
        set_asleep(false, muse_board->aux_button);
    } else if (!s_talk_down) {
        muse_state_poke();
        muse_menu_key(MUSE_MENU_DOWN);
    }
}

static void aux_key(bool pressed, bool edge)
{
    if (muse_board->touch) {
        aux_button(pressed, edge);
    } else {
        menu_button(pressed, edge);
    }
}

/* Talk button: push-to-talk, or Select while the menu is open. Asleep, the
 * press wakes and is posted as a waking one: muse_voice records only if it's
 * still held once awake. */
/* Two quick taps of the talk button switch the agent (Muse / Hermes). */
static void count_tap(bool pressed)
{
#ifdef CONFIG_MUSE_HERMES_URL
    static TickType_t down, last_tap;
    static int taps;
    TickType_t now = xTaskGetTickCount();
    if (pressed) {
        down = now;
        return;
    }
    if (!CONFIG_MUSE_HERMES_URL[0] || now - down > pdMS_TO_TICKS(300)) {
        taps = 0;
        return;
    }
    taps = (taps && now - last_tap <= pdMS_TO_TICKS(1200)) ? taps + 1 : 1;
    last_tap = now;
    if (taps >= 2) {
        taps = 0;
        /* Cycle Muse -> Hermes -> Hermes ALT -> Muse. The ALT slot only joins the cycle
         * once a profile name exists (the relay cannot route an unnamed profile). */
        char alt[MUSE_AGENT_PROFILE_MAX + 1];
        muse_settings_agent_alt(alt);   /* stored name: the active-only getter reads "" here */
        int next = (muse_settings_agent() + 1) % MUSE_AGENT_COUNT;
        if (next == MUSE_AGENT_HERMES_ALT && !alt[0]) {
            next = MUSE_AGENT_MUSE;
        }
        ESP_LOGI(TAG, "double tap: agent %d -> %d", muse_settings_agent(), next);
        muse_settings_set_agent(next);
    }
#endif
}

static void talk_button(unsigned ev)
{
    bool talk_down = s_talk_down;
    static bool swallow;
#if CONFIG_MUSE_WATCHER_CAMERA
    static TickType_t last_release;
    if ((ev & MUSE_BTN_TALK_PRESS) && !muse_state_asleep()
        && !muse_menu_is_open() && last_release
        && xTaskGetTickCount() - last_release <= pdMS_TO_TICKS(350)) {
        ESP_LOGI(TAG, "wheel double-click: camera preview/shutter");
        last_release = 0;
        watcher_camera_preview_toggle();
        swallow = true;
        return;
    }
#endif

    /* A quick tap can latch press and release in the same poll, and a release
     * can land just before the next press; keep them ordered. */
    bool released = ev & MUSE_BTN_TALK_RELEASE;
#if CONFIG_MUSE_WATCHER_CAMERA
    bool saw_release = released;
#endif
    if ((talk_down || swallow) && released) {
        if (talk_down) {
            post(MUSE_PTT_UP, false);
            count_tap(false);
        }
        talk_down = swallow = false;
        released = false;
    }
    if (!talk_down && !swallow && (ev & MUSE_BTN_TALK_PRESS)) {
        if (muse_link_talk_press()) {
            /* Confirmed a Muse app pairing (Link's setup button). */
            muse_state_poke();
            swallow = true;
        } else if (muse_state_asleep()) {
            set_asleep(false, muse_board->talk_button);
            post(MUSE_PTT_DOWN, true);
            talk_down = true;
        } else if (muse_menu_is_open()) {
            muse_state_poke();
            if (!muse_board->keyboard) muse_menu_key(MUSE_MENU_SELECT);
            swallow = true;
        } else {
            post(MUSE_PTT_DOWN, false);
            talk_down = true;
            count_tap(true);
        }
    }
    if ((talk_down || swallow) && released) {
        if (talk_down) {
            post(MUSE_PTT_UP, false);
            count_tap(false);
        }
        talk_down = swallow = false;
    }
#if CONFIG_MUSE_WATCHER_CAMERA
    if (saw_release && !swallow) {
        last_release = xTaskGetTickCount();
    }
#endif
    s_talk_down = talk_down;
}

/* Keyboard menus don't repurpose Space/GO as Select. Navigation wakes the
 * screen without accidentally changing a setting; Enter can confirm pairing. */
static void keyboard_buttons(unsigned ev)
{
    const unsigned mask = MUSE_BTN_UP | MUSE_BTN_DOWN | MUSE_BTN_LEFT |
                          MUSE_BTN_RIGHT | MUSE_BTN_ENTER | MUSE_BTN_ESCAPE;
    if (!(ev & mask) || s_talk_down) return;
    if ((ev & MUSE_BTN_ENTER) && muse_link_talk_press()) {
        muse_state_poke();
        return;
    }
    if (muse_state_asleep()) {
        set_asleep(false, "keyboard");
        return;
    }
    muse_state_poke();
    if (ev & MUSE_BTN_ESCAPE) muse_menu_key(MUSE_MENU_BACK);
    else if (ev & MUSE_BTN_UP) muse_menu_key(MUSE_MENU_UP);
    else if (ev & MUSE_BTN_DOWN) muse_menu_key(MUSE_MENU_DOWN);
    else if (ev & MUSE_BTN_LEFT) muse_menu_key(MUSE_MENU_LEFT);
    else if (ev & MUSE_BTN_RIGHT) muse_menu_key(MUSE_MENU_RIGHT);
    else if (ev & MUSE_BTN_ENTER) muse_menu_key(MUSE_MENU_SELECT);
}

/* A pairing prompt wakes the screen and keeps it on; otherwise idle sleeps. */
static void check_sleep(void)
{
    muse_ble_status_t ble;
    muse_ble_status(&ble);
    bool prompt = ble.passkey || muse_link_state() == MUSE_LINK_CONFIRM;
    if (prompt) {
        set_asleep(false, "pairing");
        return;
    }
    int after = muse_settings_sleep_s();
    float mode_t;
    if (after && !muse_state_asleep() && muse_state_mode(&mode_t) == MUSE_MODE_IDLE
        && muse_state_idle_secs() > after) {
        set_asleep(true, "auto-sleep");
    }
}

static void set_cpu_low(bool low)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = low ? CONFIG_XTAL_FREQ : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = low,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power management: %s", esp_err_to_name(err));
    }
#else
    (void)low;
#endif
}

/*
 * On battery with the screen dark and voice resting: stop the display and let
 * the CPU drop to the crystal clock and light-sleep between polls, or until a
 * button interrupts (wait_buttons). Returns true while the display is stopped.
 * With a USB host attached (">nap" on the bench) the CPU stays at full speed:
 * at the crystal clock a long line of serial output can stall the USB console
 * until the host reopens the port.
 */
static bool update_power(void)
{
    static bool paused;
    bool pause = muse_board->display_pause && muse_state_on_battery() && muse_state_asleep()
                 && muse_ui_dark() && muse_voice_resting();
    bool want_low = pause && !muse_console_host();
    if (pause == paused && want_low == s_cpu_low) {
        return paused;
    }
    if (pause && !paused) {
        muse_board->display_pause(true);
    }
    if (want_low != s_cpu_low) {
        set_cpu_low(want_low);
    }
    if (!pause && paused) {
        muse_board->display_pause(false);
    }
    paused = pause;
    s_cpu_low = want_low;
    ESP_LOGI(TAG, "%s", s_cpu_low ? "low power: display paused"
                        : paused  ? "display paused (USB host: CPU at full speed)"
                                  : "full power");
    return paused;
}

/*
 * Low power for WIFI_NAP_MS: Wi-Fi off, since keeping it associated costs
 * more than the rest of the chip. It rejoins when the screen wakes or USB
 * power arrives; meanwhile nothing reaches the device over the network.
 * Not while a voice note recorded offline waits to go (for a while). Returns
 * true while napping.
 */
static bool update_wifi_nap(TickType_t now, bool paused)
{
    static TickType_t low_since;
    static bool napping;
    if (!s_cpu_low) {
        low_since = now;
    }
    if (!muse_state_asleep() && s_nap_now) {
        s_nap_now = false;
        muse_state_set_as_if_battery(false);
    }
    bool nap = paused && !muse_voice_notes_waiting()
               && (s_nap_now || (s_cpu_low && now - low_since >= pdMS_TO_TICKS(WIFI_NAP_MS)));
    if (nap != napping) {
        napping = nap;
        ESP_LOGI(TAG, "Wi-Fi %s", nap ? "napping" : "waking");
        muse_wifi_nap(nap);
    }
    return napping;
}

static void input_task(void *arg)
{
    (void)arg;
    bool aux_down = false;
    bool paused = false;
    TickType_t checked = xTaskGetTickCount() - pdMS_TO_TICKS(SLEEP_CHECK_MS);
    TickType_t powered = xTaskGetTickCount() - pdMS_TO_TICKS(POWER_MS);

    for (;;) {
        unsigned ev = muse_board->poll_buttons();
        if (ev & (MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_RELEASE)) {
            ESP_LOGI(TAG, "talk key:%s%s", ev & MUSE_BTN_TALK_PRESS ? " press" : "",
                     ev & MUSE_BTN_TALK_RELEASE ? " release" : "");
            talk_button(ev);
        }
        keyboard_buttons(ev);
        /* A latched key (the 1.75's PMU) can report press and release in the
         * same poll, and a release can land just before the next press; keep
         * them ordered, as talk_button does. */
        bool aux_press = ev & MUSE_BTN_AUX_PRESS;
        bool aux_release = ev & MUSE_BTN_AUX_RELEASE;
        bool aux_edge = false;
        if (aux_down && aux_release) {
            aux_down = false;
            aux_release = false;
            aux_edge = true;
            aux_key(false, true);
        }
        if (!aux_down && aux_press) {
            aux_down = aux_edge = true;
            aux_key(true, true);
            if (aux_release) {
                aux_down = false;
                aux_key(false, true);
            }
        }
        if (!aux_edge) {
            aux_key(aux_down, false);
        }

        if (s_power_off_requested) {
            s_power_off_requested = false;
            power_off();
        }

        /* Restart requests and the talk-button hold. The hold is the only in-hand
         * recovery on this board (no reset key, no readable PWR); a voice note stops
         * at 15 s, so 20 s of solid holding means "restart", not "talk". */
        static int64_t s_talk_down_us;
        if (s_talk_down && !s_talk_down_us) {
            s_talk_down_us = esp_timer_get_time();
        } else if (!s_talk_down) {
            s_talk_down_us = 0;
        } else if (esp_timer_get_time() - s_talk_down_us >= (int64_t)HOLD_RESTART_MS * 1000) {
            ESP_LOGW(TAG, "talk held %d s: forcing a restart", HOLD_RESTART_MS / 1000);
            s_restart_requested = true;
        }
        if (s_restart_requested) {
            s_restart_requested = false;
            ESP_LOGI(TAG, "restarting");
            muse_state_set_caption("RESTARTING...");
            vTaskDelay(pdMS_TO_TICKS(400));   /* let the caption land, then cut */
            esp_restart();
        }

        TickType_t now = xTaskGetTickCount();
        if (now - checked >= pdMS_TO_TICKS(SLEEP_CHECK_MS)) {
            checked = now;
            check_sleep();
        }

        if (now - powered >= pdMS_TO_TICKS(paused ? REST_POWER_MS : POWER_MS)) {
            powered = now;
            muse_power_t p = { .battery_pct = -1 };
            if (muse_board->read_power && muse_board->read_power(&p) == ESP_OK) {
                muse_state_set_power(&p);
                muse_battery_note_power(&p, muse_state_on_battery());
            }
        }

        paused = update_power();
        bool napping = update_wifi_nap(now, paused);
        muse_battery_note_state(muse_state_asleep(), s_cpu_low);
        /* Paused, anything but a button (a pairing prompt, an image) is
         * noticed within REST_WAIT_MS. Napping, nothing comes over the
         * network; USB power arriving is noticed within NAP_WAIT_MS. */
        if (paused && muse_board->wait_buttons) {
            muse_board->wait_buttons(napping ? NAP_WAIT_MS : REST_WAIT_MS);
        } else {
            vTaskDelay(pdMS_TO_TICKS(paused ? REST_POLL_MS : POLL_MS));
        }
    }
}

/* Reads the rest of a line into buf; false if it was too long (the rest is dropped). */
static bool read_line(char *buf, size_t cap)
{
    size_t len = 0;
    bool whole = true;
    uint8_t c;
    while (muse_console_getc(&c) && c != '\n') {
        if (c == '\r') {
            continue;
        }
        if (len < cap - 1) {
            buf[len++] = (char)c;
        } else {
            whole = false;
        }
    }
    buf[len] = '\0';
    return whole;
}

#if CONFIG_MUSE_HATCH
#define CHAT_OVER_SERIAL "true"

/*
 * "chat+=TEXT" adds a piece of a message and "chat=TEXT" adds the last piece
 * and sends it (TEXT escaped: \n \r \t \\). Every line is acknowledged, and the
 * host waits for that before the next: the driver drops what it has no room for.
 */
static char *s_chat;        /* the message so far */
static size_t s_chat_len;

static void chat_line(char *piece, bool last, bool whole)
{
    size_t n = muse_hatch_unescape(piece);
    if (!s_chat) {
        s_chat = heap_caps_malloc(CHAT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_chat_len = 0;
    }
    const char *err = !whole ? "LINE TOO LONG" : !s_chat ? "OUT OF MEMORY" : s_chat_len + n >= CHAT_MAX ? "TOO LONG" : NULL;
    if (err) {
        free(s_chat);
        s_chat = NULL;
        muse_hatch_console("error", err, NULL);
        return;
    }
    memcpy(s_chat + s_chat_len, piece, n);
    s_chat_len += n;
    s_chat[s_chat_len] = '\0';
    muse_hatch_console("ack", NULL, "\"bytes\":%u", (unsigned)s_chat_len);
    if (last) {
        muse_hatch_text_turn(s_chat);   /* frees it */
        s_chat = NULL;
    }
}

/* Drops a half-sent message and ends a typed turn. */
static void chat_cancel(void)
{
    free(s_chat);
    s_chat = NULL;
    muse_hatch_text_cancel();
}
#else
#define CHAT_OVER_SERIAL "false"   /* no PSRAM: replies come over Link, and only short ones */
#endif

/*
 * Bench: puts the face in a mode ("face=thinking") until the voice path or
 * another "face=" moves it on. "face=happy" goes back to idle with the happy
 * hop, as a finished turn does.
 */
static void set_face(const char *name)
{
    static const char *const modes[MUSE_MODE_COUNT] = {
        [MUSE_MODE_BOOT] = "boot",
        [MUSE_MODE_IDLE] = "idle",
        [MUSE_MODE_LISTENING] = "listening",
        [MUSE_MODE_THINKING] = "thinking",
        [MUSE_MODE_SPEAKING] = "speaking",
        [MUSE_MODE_ERROR] = "error",
        [MUSE_MODE_OFF] = "off",
    };
    muse_state_poke();
    bool happy = !strcmp(name, "happy");
    if (happy) {
        name = "idle";   /* where a finished turn hops to */
    }
    for (int m = 0; m < MUSE_MODE_COUNT; m++) {
        if (!strcmp(name, modes[m])) {
            muse_state_set_mode(MUSE_MODE_IDLE);   /* only idle leaves "off" */
            muse_state_set_mode((muse_mode_t)m);
            if (happy) {
                muse_state_make_happy();
            }
            return;
        }
    }
    printf("@face.error unknown face \"%s\": boot idle listening thinking speaking error off happy\n", name);
    fflush(stdout);
}

/*
 * Console-only commands; false for setup commands. Their buffers are taken
 * per command: without PSRAM, static ones would hold internal RAM for good.
 */
static bool console_command(char *line, bool whole)
{
    if (!strcmp(line, "status")) {
        size_t cap = 1024;   /* long SSID, host and VM names escaped: past 512 */
        char *json = heap_caps_malloc(cap, MUSE_BIG_CAPS);
        if (json) {
            muse_ble_status_json(json, cap);
            char alt[MUSE_AGENT_PROFILE_MAX + 1];
            muse_settings_agent_profile(alt);
            printf("@status {\"board\":\"%s\",\"chat\":%s,"
                   "\"agent\":{\"idx\":%d,\"name\":\"%s\",\"profile\":\"%s\"},"
                   "\"device\":%s}\n",
                   muse_board->name, CHAT_OVER_SERIAL,
                   muse_settings_agent(),
                   muse_settings_agent() == MUSE_AGENT_MUSE ? "muse" : "hermes", alt, json);
            fflush(stdout);
            free(json);
        }
        return true;
    }
    bool reset = !strcmp(line, "power.reset");
    if (reset || !strcmp(line, "power")) {
        if (reset) {
            muse_battery_reset();
        }
        const char *saved = muse_battery_saved_json();
        if (saved) {
            printf("@power.saved %s\n", saved);
        }
        size_t cap = 2048;   /* two dozen power locks */
        char *json = heap_caps_malloc(cap, MUSE_BIG_CAPS);
        if (json) {
            muse_battery_json(json, cap);
            printf("@power %s\n", json);
            free(json);
        }
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "nap")) {
        muse_state_set_as_if_battery(true);
        set_asleep(true, "serial");
        s_nap_now = true;
        return true;
    }
    if (!strcmp(line, "reboot") || !strcmp(line, "restart")) {
        printf("@reboot ok\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(150));   /* let the ack reach the host */
        esp_restart();
        return true;
    }
    if (!strncmp(line, "face=", 5)) {
        set_face(line + 5);
        return true;
    }
    if (bench_ui_command(line)) {
        return true;
    }
    if (!strncmp(line, "draw=", 5)) {
        const char *code = NULL, *message = NULL;
        if (!image_fetch_start(line + 5, -1, draw_done, NULL, &code, &message)) {
            printf("@draw FAILED %s: %s\n", code ? code : "?", message ? message : "?");
            fflush(stdout);
        }
        return true;
    }
    if (strncmp(line, "chat", 4) != 0) {
        return false;
    }
#if CONFIG_MUSE_HATCH
    if (!strcmp(line, "chat.cancel")) {
        chat_cancel();
        return true;
    }
    bool last = !strncmp(line, "chat=", 5);
    if (last || !strncmp(line, "chat+=", 6)) {
        chat_line(line + (last ? 5 : 6), last, whole);
        return true;
    }
    return false;
#else
    (void)whole;
    muse_hatch_console("error", "THIS BOARD CAN'T CHAT OVER SERIAL", NULL);
    return true;
#endif
}



/* Bench hooks: ">react=smirk" acts out a mood; ">choose=Question?|One|Two" asks a question and prints the answer. */
static void choose_done(int idx, void *user)
{
    (void)user;
    printf("@choose %d\n", idx);
    fflush(stdout);
}

static bool bench_ui_command(const char *line)
{
    if (!strcmp(line, "sound=transition")) {
        muse_voice_request_transition();
        printf("@sound transition\n");
        fflush(stdout);
        return true;
    }
    if (!strncmp(line, "agent=", 6)) {
        const char *v = line + 6;
        if (!strcmp(v, "muse")) {
            muse_settings_set_agent(MUSE_AGENT_MUSE);
        } else if (!strcmp(v, "hermes")) {
            muse_settings_set_agent(MUSE_AGENT_HERMES);
        } else if (!strcmp(v, "alt") || !strcmp(v, "2")) {
            muse_settings_set_agent(MUSE_AGENT_HERMES_ALT);
        } else if (!strcmp(v, "0")) {
            muse_settings_set_agent(MUSE_AGENT_MUSE);
        } else if (!strcmp(v, "1")) {
            muse_settings_set_agent(MUSE_AGENT_HERMES);
        } else {
            printf("@agent.error unknown \"%s\": muse|hermes|alt|0|1|2\n", v);
            fflush(stdout);
            return true;
        }
        char alt[MUSE_AGENT_PROFILE_MAX + 1];
        muse_settings_agent_profile(alt);
        printf("@agent %d %s%s%s\n", muse_settings_agent(),
               muse_settings_agent() == MUSE_AGENT_MUSE ? "muse" : "hermes",
               alt[0] ? " " : "", alt);
        fflush(stdout);
        return true;
    }
    if (!strncmp(line, "agent.profile=", 14)) {
        muse_settings_set_agent_profile(line + 14);
        char alt[MUSE_AGENT_PROFILE_MAX + 1];
        muse_settings_agent_alt(alt);   /* stored name, so this echo tells the truth */
        printf("@agent.profile '%s'%s\n", alt, alt[0] ? "" : " (cleared; the cycle skips ALT)");
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "notif.tap")) {
        /* Bench: act as a face tap on a waiting notification -- opens its agent. */
        printf("@notif.tap %s\n", muse_notif_open_pending() ? "opened" : "none");
        fflush(stdout);
        return true;
    }
    if (!strncmp(line, "notif=", 6)) {
        /* Bench: present a notification line as if the relay sent it (text only; use /notifications
         * for the real path with an agent). Opens nothing -- there is no relay id behind it. */
        muse_state_set_caption("%s", line + 6);
        muse_state_poke();
        return true;
    }
    if (!strcmp(line, "tap") || !strncmp(line, "tap=", 4)) {
        /* Bench: drive the REAL tap counter (count_tap -- the double-tap agent switch) without a
         * finger. ">tap" = one tap; ">tap=2" = a double-tap (one cycle step). The bench 'd'/'u'
         * keys only reach the voice queue, never count_tap, so this is the way in. */
        int n = line[3] == '=' ? atoi(line + 4) : 1;
        for (int i = 0; i < n && i < 8; i++) {
            count_tap(true);
            vTaskDelay(pdMS_TO_TICKS(50));    /* a press well under the 300 ms tap limit */
            count_tap(false);
            vTaskDelay(pdMS_TO_TICKS(200));   /* gap inside the 1200 ms accumulate window */
        }
        printf("@tap %d agent=%d\n", n, muse_settings_agent());
        fflush(stdout);
        return true;
    }
    if (!strncmp(line, "task.auto", 9) && (line[9] == '\0' || line[9] == ' ')) {
        const char *v = line + 9;
        while (*v == ' ') {
            v++;
        }
        bool on = strcmp(v, "off") != 0;   /* "off" -> false; bare or "on" -> true */
        muse_hatch_task_auto(on);
        printf("@task.auto %s\n", on ? "on" : "off");
        fflush(stdout);
        return true;
    }
    if (!strncmp(line, "react=", 6)) {
        printf("@react %s %s\n", line + 6, muse_ui_react(line + 6) ? "ok" : "unknown");
        fflush(stdout);
        return true;
    }
    if (!strncmp(line, "choose=", 7)) {
        static char buf[160];
        strlcpy(buf, line + 7, sizeof(buf));
        const char *opts[4];
        int n = 0;
        char *prompt = buf, *p = strchr(buf, '|');
        while (p && n < 4) {
            *p = 0;
            opts[n++] = p + 1;
            p = strchr(p + 1, '|');
        }
        printf("@choose-start %s\n", muse_ui_choose(prompt, opts, n, 20000, choose_done, NULL) ? "ok" : "refused");
        fflush(stdout);
        return true;
    }
    return false;
}

/*
 * Bench testing over the USB cable: 'd' / 'u' act as the talk button going
 * down / up, so the voice path can be driven without a finger on the button;
 * 'm' plays a built-in MP3 through the reply decoder; 'a' / 's' press the
 * menu's Down / Select; 'p' sends a screenshot; 'z' / 'w' sleep and wake.
 * A line starting with '>' is a setup command, the same "key=value" text as
 * the BLE CMD characteristic, or one of the console's own: "status" prints
 * the device's state, "power" the battery meter (muse_battery.h) and
 * "power.reset" starts it over, "nap" sleeps and leaves Wi-Fi at once (as
 * two minutes asleep on battery would; 'w' rejoins), "face=" shows a face
 * (see set_face), and "chat=" sends a typed message to Hatch (see chat_line
 * and tools/muse/chat.py).
 */
static void serial_task(void *arg)
{
    if (muse_console_install(SERIAL_RX) != ESP_OK) {
        vTaskDelete(NULL);
    }
    for (;;) {
        uint8_t c;
        if (!muse_console_getc(&c)) {
            continue;
        }
        if (c == 'm') {
            muse_voice_request_mp3test();
        } else if (c == 'a' || c == 's') {
            muse_state_poke();
            muse_menu_key(c == 'a' ? MUSE_MENU_DOWN : MUSE_MENU_SELECT);
        } else if (c == 'p') {
            muse_ui_request_snapshot();
        } else if (c == 'z' || c == 'w') {
            set_asleep(c == 'z', "serial");
        } else if (c == '>') {
            char *line = heap_caps_malloc(SERIAL_LINE, MUSE_BIG_CAPS);
            char none[1];   /* no room: the line is read and dropped */
            bool whole = read_line(line ? line : none, line ? SERIAL_LINE : sizeof(none));
            if (line && !console_command(line, whole)) {
                muse_ble_command(line);
            }
            free(line);
        } else if (c == 'd' || c == 'u') {
            muse_state_poke();
            s_talk_down = c == 'd';   /* bench keys act as the physical button, hold included */
            post(c == 'd' ? MUSE_PTT_DOWN : MUSE_PTT_UP, false);
        }
    }
}

esp_err_t muse_input_start(QueueHandle_t queue)
{
    s_queue = queue;
    if (xTaskCreate(input_task, "muse_input", 4096, NULL, 6, &s_input) != pdPASS) {
        return ESP_FAIL;
    }
    /* Bench-test and setup console; the input still works if it can't start. */
    xTaskCreate(serial_task, "muse_serial", 3584, NULL, 5, NULL);
    return ESP_OK;
}

void muse_input_request_power_off(void)
{
    s_power_off_requested = true;
}

void muse_input_request_restart(void)
{
    s_restart_requested = true;
}
