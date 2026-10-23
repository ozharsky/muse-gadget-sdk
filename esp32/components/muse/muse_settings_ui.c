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

#include "muse_settings_ui.h"
#include "muse_iphone.h"
#include "avatar/icon_hermes.h"
#include "avatar/icon_meta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_attr.h"   /* EXT_RAM_BSS_ATTR: the inbox rows belong in PSRAM */
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "muse_audio.h"
#include "muse_battery.h"
#include "muse_ble.h"
#include "muse_board.h"
#include "muse_chat.h"
#include "muse_input.h"
#include "muse_keypad.h"
#include "muse_link.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_text.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"

#include "sdkconfig.h"   /* CONFIG_MUSE_HATCH / CONFIG_MUSE_HERMES_URL: which pages exist */

/* Keep content in a column that stays inside a round panel (and fits a 368 px one). */
#define LIST_W 330
#define LIST_TOP 92     /* 84 shaved the top row's corners on the round 466 (safe width ~324 there) */
#define ROW_H 58
#define MAX_APS 12

#define COLOR_TEXT 0xe8fff0
#define COLOR_DIM 0x5fa070
#define COLOR_CARD 0x0e2016
#define COLOR_CARD_PRESSED 0x1d4a2c
#define COLOR_ACCENT 0x4dff3c
#define COLOR_OK 0x4dff3c      /* one green, same as the main screen's connected state */
#define COLOR_WARN 0xffb45c    /* connecting: amber, as on the main screen */
#define COLOR_DANGER 0xff5c5c  /* failed / destructive */

typedef void (*text_done_cb_t)(const char *text);

/* The screen size the text page is scaled to (see text_px). */
static int s_text_scale = 466;
static lv_obj_t *s_tile;
static lv_obj_t *s_current;
static bool s_hold_page;   /* bench: the next visibility tick keeps the open sub-page */
static lv_obj_t *s_home, *s_wifi, *s_hatch, *s_ble, *s_sound, *s_sleep, *s_battery, *s_usage, *s_power, *s_text;

/*
 * Only home is kept. A sub-page is built when it opens and deleted on the way
 * back, so only the one on screen holds RAM: all of them at once cost ~50 KB,
 * which a board without PSRAM needs for Link's session.
 */
typedef struct {
    lv_obj_t **obj;
    void (*build)(lv_obj_t *tile);
} page_t;

/* Home values. */
static lv_obj_t *s_home_list;   /* the menu's scroll column (scrolls home on arrival) */
static lv_obj_t *s_home_agent, *s_home_wifi, *s_home_hatch, *s_home_hermes, *s_home_ble, *s_home_sound, *s_home_sleep, *s_home_battery, *s_about;

/* Wi-Fi page. */
static lv_obj_t *s_wifi_sw, *s_wifi_status, *s_wifi_saved, *s_wifi_scan_btn, *s_wifi_scan_lbl, *s_wifi_list;
static uint32_t s_shown_scan_gen = UINT32_MAX;
static muse_wifi_ap_t s_aps[MAX_APS];
static muse_wifi_saved_t s_saved[MUSE_WIFI_SAVED_MAX];
static lv_obj_t *s_saved_vals[MUSE_WIFI_SAVED_MAX];
static int s_saved_n = -1;   /* as listed; -1 before the first fill */
static char s_join_ssid[MUSE_SSID_MAX + 1];

/* Hatch page. */
static lv_obj_t *s_hatch_status, *s_hatch_host, *s_hatch_vm, *s_hatch_token;
static lv_obj_t *s_link_status, *s_link_reset_lbl;
static int64_t s_link_reset_armed_us;

/* Bluetooth page. */
static lv_obj_t *s_ble_sw, *s_ble_status;

/* Sound page. */
static lv_obj_t *s_spk_sw, *s_vol_val, *s_vol_sl, *s_gain_val, *s_gain_sl, *s_bright_val, *s_bright_sl, *s_mic_bar, *s_mic_val;

/* Sleep page. */
static const int SLEEP_CHOICES[] = { 0, 30, 60, 120, 300, 600 };
static const char *const SLEEP_NAMES[] = { "Never", "30 seconds", "1 minute", "2 minutes", "5 minutes", "10 minutes" };
#define SLEEP_COUNT (int)(sizeof(SLEEP_CHOICES) / sizeof(SLEEP_CHOICES[0]))
static lv_obj_t *s_sleep_checks[SLEEP_COUNT];

/* Battery page. */
static lv_obj_t *s_batt_status, *s_batt_level, *s_batt_drain, *s_batt_full, *s_batt_off, *s_batt_slept, *s_batt_wakes,
    *s_batt_busy, *s_batt_awake;
static int64_t s_batt_shown_us;

/* Agents page. */
static const int AGENT_CHOICES[] = { MUSE_AGENT_MUSE, MUSE_AGENT_HERMES, MUSE_AGENT_HERMES_ALT };
#define AGENT_COUNT (int)(sizeof(AGENT_CHOICES) / sizeof(AGENT_CHOICES[0]))
static lv_obj_t *s_agents, *s_agent_labels[AGENT_COUNT], *s_agent_checks[AGENT_COUNT];

/* Notifications page. s_notif_rows is the page's own copy of the session task's snapshot, so a
 * tap or a hold acts on the row that was on screen even if a refresh lands mid-decision.
 * PSRAM: ~3 KB of rows that would otherwise hold internal RAM for good. */
#define NOTIF_ROWS_MAX 12
static lv_obj_t *s_home_notif;
static lv_obj_t *s_notif, *s_notif_list, *s_notif_status, *s_notif_clear_lbl;
static int64_t s_notif_clear_armed_us;   /* a Clear tap arms a 5 s confirm window */
static uint32_t s_notif_shown_gen = UINT32_MAX;
static EXT_RAM_BSS_ATTR muse_notif_row_t s_notif_rows[NOTIF_ROWS_MAX];
static int s_notif_n;

/* Text entry page. */
static lv_obj_t *s_text_title, *s_text_ta;
static lv_obj_t *s_text_kp;                 /* a screen under 2" */
static lv_obj_t *s_text_kb, *s_text_show;   /* a bigger one */
static text_done_cb_t s_text_done;
static lv_obj_t *s_text_back;

/* ---------- building blocks ---------- */

/* Network and phone names can have characters the fonts lack (muse_text.h). */
#define SHOWN_MAX 96

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    char shown[SHOWN_MAX];
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, muse_text_showable(text, shown, sizeof(shown)));
    return l;
}

static void set_text(lv_obj_t *l, const char *text)
{
    char shown[SHOWN_MAX];
    text = muse_text_showable(text, shown, sizeof(shown));
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

static lv_obj_t *note(lv_obj_t *list, const char *text)
{
    lv_obj_t *l = label(list, &lv_font_montserrat_16, COLOR_DIM, text);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static void go_back(void);

static void on_gesture(lv_event_t *e)
{
    (void)e;
    /* Dragging a slider right isn't a "back" swipe. */
    lv_obj_t *pressed = lv_indev_get_active_obj();
    if (pressed && lv_obj_check_type(pressed, &lv_slider_class)) {
        return;
    }
    /* Either direction gets you out of a sub-page: RIGHT is "back", and LEFT is what people
     * try first when a page has no visible exit -- there is no page to the left of a sub-page,
     * so nothing is stolen. On the menu itself LEFT must stay free: it is the tileview swipe
     * to the face. */
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    if (d == LV_DIR_RIGHT || (d == LV_DIR_LEFT && s_current != s_home)) {
        lv_indev_wait_release(lv_indev_active());
        go_back();
    }
}

/* Swipe right on a page to go back. By default a swipe bubbles up past the
 * page to the screen, so the page has to stop it. */
static void catch_swipes(lv_obj_t *p)
{
    lv_obj_remove_flag(p, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(p, on_gesture, LV_EVENT_GESTURE, NULL);
}

static void on_back(lv_event_t *e)
{
    (void)e;
    go_back();
}

static lv_obj_t *back_button(lv_obj_t *p)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 56, 48);
    lv_obj_align(b, LV_ALIGN_TOP_MID, -112, 28);
    lv_obj_add_event_cb(b, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = label(b, &lv_font_montserrat_20, COLOR_ACCENT, LV_SYMBOL_LEFT);
    lv_obj_center(arrow);
    return b;
}

/* A page: title, optional back arrow, and a vertically scrolling column. */
static lv_obj_t *page(lv_obj_t *tile, const char *title, bool back, lv_obj_t **list_out)
{
    /* Flat short panels, and ones too narrow for LIST_W, get tighter rows. */
    const bool compact = !muse_board->round && (muse_board->height <= 240 || muse_board->width < LIST_W);
    const int list_top = compact ? (back ? 48 : 36) : LIST_TOP;
    lv_obj_t *p = lv_obj_create(tile);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    catch_swipes(p);

    lv_obj_t *t = label(p, compact ? &lv_font_montserrat_16 : &lv_font_unscii_16, COLOR_ACCENT, title);
    lv_obj_set_style_text_letter_space(t, compact ? 0 : 2, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, compact ? (back ? 12 : 8) : 44);

    if (back) {
        lv_obj_t *b = back_button(p);
        if (compact) {
            lv_obj_set_size(b, 44, 44);
            lv_obj_align(b, LV_ALIGN_TOP_LEFT, 4, 0);
        }
    }

    lv_obj_t *list = lv_obj_create(p);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, compact ? muse_board->width - 16 : LIST_W, muse_board->height - list_top);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, list_top);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 10, 0);
    /* Clear the page dots on flat panels, or the bottom curve on round ones. */
    lv_obj_set_style_pad_bottom(list, compact ? 32 : 110, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    *list_out = list;
    return p;
}

static lv_obj_t *card(lv_obj_t *list, bool clickable)
{
    lv_obj_t *c = clickable ? lv_button_create(list) : lv_obj_create(list);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, lv_pct(100), ROW_H);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(COLOR_CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(c, 16, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, 12, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* Tappable row: icon, text, right-aligned value. The TAP is SHORT_CLICKED, not CLICKED:
 * CLICKED also fires on a long press's release, so a row that carries a long-press action
 * (a saved Wi-Fi network: hold = forget) would run its tap handler right after the hold
 * -- and after a forget the list has been rebuilt, so the stale index could act on a
 * different row. SHORT_CLICKED is skipped once a long press has fired (lv_indev.c).
 *
 * Both text labels are CLAMPED to one line height. LVGL's DOTS mode (the "…" ellipsis)
 * only ellipsizes when the label has a FIXED height -- with auto height it silently wraps
 * instead, so a long value ("Free Internet 2.4") broke onto two lines mid-number and a
 * long SSID overgrew its row. The clamp makes them ellipsize, which is what DOTS intends. */
static lv_obj_t *row(lv_obj_t *list, const char *icon, const char *text, lv_obj_t **value_out,
                     lv_event_cb_t cb, void *user)
{
    lv_obj_t *c = card(list, true);
    if (icon) {
        label(c, &lv_font_montserrat_20, COLOR_ACCENT, icon);
    }
    lv_obj_t *t = label(c, &lv_font_montserrat_20, COLOR_TEXT, text);
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(t, lv_font_get_line_height(&lv_font_montserrat_20));
    if (value_out) {
        lv_obj_t *v = label(c, &lv_font_montserrat_16, COLOR_DIM, "");
        lv_obj_set_style_max_width(v, 130, 0);
        lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(v, lv_font_get_line_height(&lv_font_montserrat_16));
        *value_out = v;
    }
    lv_obj_add_event_cb(c, cb, LV_EVENT_SHORT_CLICKED, user);
    return c;
}

static lv_obj_t *switch_row(lv_obj_t *list, const char *text, bool on, lv_event_cb_t cb)
{
    lv_obj_t *c = card(list, false);
    lv_obj_t *t = label(c, &lv_font_montserrat_20, COLOR_TEXT, text);
    lv_obj_set_flex_grow(t, 1);
    lv_obj_t *sw = lv_switch_create(c);
    lv_obj_set_size(sw, 60, 32);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3a3358), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static lv_obj_t *button(lv_obj_t *list, const char *text, uint32_t color, lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *b = card(list, true);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = label(b, &lv_font_montserrat_20, color, text);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (label_out) {
        *label_out = l;
    }
    return b;
}

/* Label + value on one line, slider below. */
static lv_obj_t *slider(lv_obj_t *list, const char *text, int lo, int hi, int value, lv_obj_t **value_out,
                        lv_event_cb_t cb)
{
    lv_obj_t *c = lv_obj_create(list);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(c, 8, 0);
    lv_obj_set_style_pad_ver(c, 6, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    label(c, &lv_font_montserrat_20, COLOR_TEXT, text);
    lv_obj_t *v = label(c, &lv_font_montserrat_20, COLOR_ACCENT, "");
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, 0);
    *value_out = v;

    lv_obj_t *s = lv_slider_create(c);
    lv_obj_set_width(s, lv_pct(94));
    lv_obj_set_height(s, 12);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 40);
    lv_slider_set_range(s, lo, hi);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x1a3a24), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 16);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *spacer = lv_obj_create(c);   /* room below the slider */
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_align(spacer, LV_ALIGN_TOP_LEFT, 0, 64);
    return s;
}

/* A column of rows inside the page's list, filled in later. */
static lv_obj_t *column(lv_obj_t *list)
{
    lv_obj_t *l = lv_obj_create(list);
    lv_obj_remove_style_all(l);
    lv_obj_set_size(l, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(l, 10, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    return l;
}

/* Label and value, not tappable. */
static lv_obj_t *info_row(lv_obj_t *list, const char *text)
{
    lv_obj_t *c = card(list, false);
    lv_obj_set_height(c, 44);
    lv_obj_t *t = label(c, &lv_font_montserrat_16, COLOR_TEXT, text);
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(t, lv_font_get_line_height(&lv_font_montserrat_16));
    lv_obj_t *v = label(c, &lv_font_montserrat_16, COLOR_ACCENT, "");
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(v, lv_font_get_line_height(&lv_font_montserrat_16));
    lv_obj_set_style_max_width(v, 180, 0);
    return v;
}

/* ---------- navigation ---------- */

static void drop(lv_obj_t *p)
{
    lv_obj_t **const pages[] = { &s_agents, &s_wifi, &s_hatch, &s_ble, &s_sound, &s_sleep, &s_battery, &s_usage, &s_power, &s_text, &s_notif };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        if (*pages[i] == p) {
            *pages[i] = NULL;
        }
    }
    lv_obj_delete_async(p);   /* we may be in one of its own events */
}

static void show(lv_obj_t *p)
{
    lv_obj_t *prev = s_current;
    if (prev == p) {
        return;
    }
    if (prev) {
        lv_obj_add_flag(prev, LV_OBJ_FLAG_HIDDEN);
    }
    if (prev == s_sound) {
        muse_voice_set_monitor(false);
    }
    lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
    s_current = p;
    if (p == s_sound) {
        muse_voice_set_monitor(true);
    }
    muse_ui_set_swipe_enabled(p == s_home);
    /* Home keeps nothing open; the text page returns to the page that opened it. */
    if (prev && prev != s_home && (p == s_home || prev == s_text)) {
        drop(prev);
    }
}

static void close_text(void);

static void go_back(void)
{
    if (s_current == s_text) {
        close_text();
    } else if (s_current != s_home) {
        show(s_home);
    }
}

static void on_nav(lv_event_t *e)
{
    const page_t *page = lv_event_get_user_data(e);
    if (!*page->obj) {
        page->build(s_tile);
    }
    show(*page->obj);
    muse_settings_ui_tick(true);   /* fill it in now, not at the next tick */
}

/* ---------- text entry ---------- */

/* Laid out for a 466 px screen; a smaller round one scales it about its centre. */
static int text_px(int v)
{
    return v * s_text_scale / 466;
}

static int text_y(int y)
{
    return muse_board->height / 2 + text_px(y - 233);
}

static void close_text(void)
{
    lv_textarea_set_text(s_text_ta, "");   /* don't leave secrets in the widget */
    show(s_text_back);
}

static void on_text_ready(lv_event_t *e)
{
    (void)e;
    text_done_cb_t done = s_text_done;
    char *text = strdup(lv_textarea_get_text(s_text_ta));
    close_text();
    if (done && text) {
        done(text);
    }
    if (text) {
        memset(text, 0, strlen(text));
        free(text);
    }
}

static void on_text_cancel(lv_event_t *e)
{
    (void)e;
    close_text();
}

static void on_text_show(lv_event_t *e)
{
    (void)e;
    bool pw = !lv_textarea_get_password_mode(s_text_ta);
    lv_textarea_set_password_mode(s_text_ta, pw);
    lv_label_set_text(lv_obj_get_child(s_text_show, 0), pw ? "Show" : "Hide");
}

/* Beside the keyboard's field, a button to show a password. */
static void fit_show_button(bool password)
{
    int w = text_px(300), show_w = 70, gap = 8;
    lv_obj_set_width(s_text_ta, password ? w - show_w - gap : w);
    lv_obj_align(s_text_ta, LV_ALIGN_TOP_MID, password ? -(show_w + gap) / 2 : 0, text_y(76));
    lv_obj_align(s_text_show, LV_ALIGN_TOP_MID, (w - show_w) / 2, text_y(76));
    lv_label_set_text(lv_obj_get_child(s_text_show, 0), "Show");
    if (password) {
        lv_obj_remove_flag(s_text_show, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_text_show, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_keyboard(void)
{
    s_text_show = lv_button_create(s_text);
    lv_obj_remove_style_all(s_text_show);
    lv_obj_set_size(s_text_show, 70, text_px(48));
    lv_obj_set_style_radius(s_text_show, 14, 0);
    lv_obj_set_style_bg_opa(s_text_show, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_text_show, lv_color_hex(COLOR_CARD), 0);
    lv_obj_add_event_cb(s_text_show, on_text_show, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(s_text_show, &lv_font_montserrat_16, COLOR_TEXT, "Show"));

    /* Inside the circle, or across the rest of the screen. */
    s_text_kb = lv_keyboard_create(s_text);
    int y = text_y(136);
    if (muse_board->round) {
        lv_obj_set_size(s_text_kb, text_px(384), text_px(206));
    } else {
        int w = muse_board->width - 16;
        int h = muse_board->height - y - 8;
        lv_obj_set_size(s_text_kb, w, LV_MIN(h, w * 206 / 384));
    }
    lv_obj_align(s_text_kb, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_opa(s_text_kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_text_kb, 2, 0);
    lv_obj_set_style_pad_gap(s_text_kb, 4, 0);
    lv_obj_set_style_text_font(s_text_kb, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_text_kb, lv_color_hex(COLOR_CARD), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_text_kb, lv_color_hex(COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_radius(s_text_kb, 8, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_text_kb, 0, LV_PART_ITEMS);
    lv_obj_remove_flag(s_text_kb, LV_OBJ_FLAG_GESTURE_BUBBLE);   /* a sloppy swipe mustn't lose the text */
    lv_keyboard_set_textarea(s_text_kb, s_text_ta);
    lv_obj_add_event_cb(s_text_kb, on_text_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_text_kb, on_text_cancel, LV_EVENT_CANCEL, NULL);
}

static void build_text_page(lv_obj_t *tile)
{
    s_text = lv_obj_create(tile);
    lv_obj_remove_style_all(s_text);
    lv_obj_set_size(s_text, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(s_text, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_text, LV_OBJ_FLAG_HIDDEN);
    catch_swipes(s_text);
    lv_obj_t *back = back_button(s_text);

    /* Between the back arrow and its mirror image. Height-clamped so a long SSID (this
     * title takes the network name when asking for a password) ellipsizes on one line
     * instead of wrapping down over the text field below. */
    s_text_title = label(s_text, &lv_font_montserrat_20, COLOR_ACCENT, "");
    lv_obj_set_width(s_text_title, 150);
    lv_obj_set_style_text_align(s_text_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_text_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_text_title, lv_font_get_line_height(&lv_font_montserrat_20));
    lv_obj_align(s_text_title, LV_ALIGN_TOP_MID, 0, 40);

    const lv_font_t *font = &lv_font_montserrat_20;
    int h = text_px(48), border = 2;
    int pad = (h - 2 * border - lv_font_get_line_height(font)) / 2;
    s_text_ta = lv_textarea_create(s_text);
    lv_textarea_set_one_line(s_text_ta, true);
    lv_obj_set_size(s_text_ta, text_px(300), h);
    lv_obj_set_style_text_font(s_text_ta, font, 0);
    lv_obj_set_style_pad_ver(s_text_ta, pad > 0 ? pad : 0, 0);
    lv_obj_set_style_pad_hor(s_text_ta, 14, 0);
    lv_obj_set_style_bg_color(s_text_ta, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_text_color(s_text_ta, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_border_color(s_text_ta, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(s_text_ta, border, 0);
    lv_obj_set_style_radius(s_text_ta, 14, 0);
    lv_obj_set_style_text_font(s_text_ta, &lv_font_montserrat_16, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_color(s_text_ta, lv_color_hex(COLOR_DIM), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_state(s_text_ta, LV_STATE_FOCUSED);
    lv_obj_align(s_text_ta, LV_ALIGN_TOP_MID, 0, text_y(76));

    /* A full keyboard's keys are too small to hit on a screen under 2". */
    if (muse_board->diagonal_in >= 2.0f) {
        build_keyboard();
        return;
    }

    /* The keys out to the screen's edges, and the rest squeezed in above them:
     * the back arrow beside the field, the title over both. */
    lv_obj_align(s_text_title, LV_ALIGN_TOP_MID, 0, text_y(16));
    lv_obj_set_width(s_text_title, text_px(180));
    lv_obj_set_width(s_text_ta, text_px(224));
    lv_obj_align(s_text_ta, LV_ALIGN_TOP_MID, 0, text_y(44));
    lv_obj_set_size(back, text_px(48), h);
    lv_obj_align(back, LV_ALIGN_TOP_MID, -text_px(140), text_y(44));
    s_text_kp = muse_keypad_create(s_text, s_text_ta, muse_board->round);
    int top = text_y(98);
    lv_obj_set_size(s_text_kp, muse_board->width, muse_board->height - top);
    lv_obj_align(s_text_kp, LV_ALIGN_TOP_MID, 0, top);
    if (muse_board->round) {
        /* Lifts the bottom row's labels inside the circle. The button matrix
         * stretches edge keys' taps over padding up to LV_DPI_DEF / 10, and not
         * at all over more, so this much keeps them reaching the edge. */
        lv_obj_set_style_pad_bottom(s_text_kp, LV_DPI_DEF / 10, 0);
    }
    lv_obj_add_event_cb(s_text_kp, on_text_ready, LV_EVENT_READY, NULL);
}

/* The hint shows in the empty field, so keep it short. */
static void open_text(const char *title, const char *initial, bool password, int max_len, const char *hint,
                      text_done_cb_t done, lv_obj_t *back)
{
    if (!s_text) {
        build_text_page(s_tile);
    }
    set_text(s_text_title, title);
    lv_textarea_set_max_length(s_text_ta, max_len);
    lv_textarea_set_password_mode(s_text_ta, password);
    lv_textarea_set_text(s_text_ta, initial ? initial : "");
    lv_textarea_set_placeholder_text(s_text_ta, hint ? hint : "");
    if (s_text_kp) {
        muse_keypad_reset(s_text_kp, password);
    } else {
        lv_keyboard_set_mode(s_text_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        fit_show_button(password);
    }
    s_text_done = done;
    s_text_back = back;
    show(s_text);
}

/* ---------- Wi-Fi ---------- */

static void on_wifi_sw(lv_event_t *e)
{
    muse_settings_set_wifi_on(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void on_wifi_scan(lv_event_t *e)
{
    (void)e;
    muse_wifi_scan();
}

static void on_wifi_pass(const char *pass)
{
    muse_settings_set_wifi(s_join_ssid, pass);
}

static bool is_saved(const char *ssid)
{
    for (int i = 0; i < s_saved_n; i++) {
        if (!strcmp(s_saved[i].ssid, ssid)) {
            return true;
        }
    }
    return false;
}

/* A saved network asks again too, in case its password changed. */
static void on_wifi_ap(lv_event_t *e)
{
    const muse_wifi_ap_t *ap = &s_aps[(int)(intptr_t)lv_event_get_user_data(e)];
    strlcpy(s_join_ssid, ap->ssid, sizeof(s_join_ssid));
    if (ap->secure) {
        open_text(ap->ssid, "", true, MUSE_PASS_MAX, "Password", on_wifi_pass, s_wifi);
    } else {
        muse_settings_set_wifi(s_join_ssid, "");
    }
}

static void on_other_ssid(const char *ssid)
{
    if (!ssid[0]) {
        return;
    }
    strlcpy(s_join_ssid, ssid, sizeof(s_join_ssid));
    open_text(ssid, "", true, MUSE_PASS_MAX, "Empty if it's open", on_wifi_pass, s_wifi);
}

static void on_wifi_other(lv_event_t *e)
{
    (void)e;
    open_text("Other network", "", false, MUSE_SSID_MAX, "Network name", on_other_ssid, s_wifi);
}

/* Two taps within a few seconds forget a saved network. */
/* A saved network: TAP switches to it; LONG-PRESS forgets it. One gesture, one meaning --
 * the old "tap to arm forget, tap again to forget" made a plain tap look like it only forgot,
 * and lost the network if you tapped twice. Forgetting is destructive, so it takes a hold. */
static void on_wifi_saved_tap(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!muse_wifi_switch(s_saved[i].ssid)) {
        muse_state_set_caption("COULDN'T START SWITCH TO %s", s_saved[i].ssid);
        muse_state_poke();
    }
    /* success/failure is narrated by the switch task itself */
}

static void on_wifi_saved_hold(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    muse_wifi_forget(s_saved[i].ssid);
    muse_state_set_caption("FORGOT %s", s_saved[i].ssid);
    muse_state_poke();
}

static bool same_saved(const muse_wifi_saved_t *a, int n)
{
    if (n != s_saved_n) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        if (strcmp(a[i].ssid, s_saved[i].ssid) || a[i].hidden != s_saved[i].hidden) {
            return false;
        }
    }
    return true;
}

static void rebuild_saved_list(void)
{
    muse_wifi_saved_t saved[MUSE_WIFI_SAVED_MAX];
    int n = muse_wifi_saved(saved, MUSE_WIFI_SAVED_MAX);
    if (same_saved(saved, n)) {
        return;
    }
    memcpy(s_saved, saved, n * sizeof(saved[0]));
    s_saved_n = n;
    s_shown_scan_gen = UINT32_MAX;   /* re-mark the saved ones in the scan list */
    lv_obj_clean(s_wifi_saved);
    if (n) {
        note(s_wifi_saved, "Saved networks");
    }
    for (int i = 0; i < n; i++) {
        lv_obj_t *r = row(s_wifi_saved, LV_SYMBOL_WIFI, saved[i].ssid, &s_saved_vals[i],
                         on_wifi_saved_tap, (void *)(intptr_t)i);
        lv_obj_add_event_cb(r, on_wifi_saved_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    }
}

static void tick_saved_list(const muse_wifi_status_t *w)
{
    for (int i = 0; i < s_saved_n; i++) {
        const char *text = "";
        uint32_t color = COLOR_DIM;
        if (w->state == MUSE_WIFI_CONNECTED && !strcmp(w->ssid, s_saved[i].ssid)) {
            text = "Connected";
            color = COLOR_OK;
        } else if (s_saved[i].hidden) {
            text = "Hidden";
        }
        set_text(s_saved_vals[i], text);
        lv_obj_set_style_text_color(s_saved_vals[i], lv_color_hex(color), 0);
    }
}

/* Returns true when fresh results include a saved network. */
static bool rebuild_scan_list(void)
{
    uint32_t gen;
    int n = muse_wifi_scan_results(s_aps, MAX_APS, &gen);
    if (gen == s_shown_scan_gen) {
        return false;
    }
    bool fresh = s_shown_scan_gen != UINT32_MAX, saved_seen = false;
    s_shown_scan_gen = gen;
    lv_obj_clean(s_wifi_list);
    for (int i = 0; i < n; i++) {
        lv_obj_t *v;
        row(s_wifi_list, NULL, s_aps[i].ssid, &v, on_wifi_ap, (void *)(intptr_t)i);
        bool saved = is_saved(s_aps[i].ssid);
        saved_seen |= saved;
        char buf[24];
        snprintf(buf, sizeof(buf), "%s%d dBm", saved ? "saved  " : (s_aps[i].secure ? "" : "open  "), s_aps[i].rssi);
        lv_label_set_text(v, buf);
    }
    if (gen && !n) {
        note(s_wifi_list, "No networks found");
    }
    return fresh && saved_seen;
}

static void build_wifi_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_wifi = page(tile, "WI-FI", true, &list);
    s_shown_scan_gen = UINT32_MAX;   /* the lists start empty */
    s_saved_n = -1;
    s_wifi_sw = switch_row(list, "Wi-Fi", muse_settings_wifi_on(), on_wifi_sw);
    s_wifi_status = note(list, "");

    s_wifi_saved = column(list);
    s_wifi_scan_btn = button(list, LV_SYMBOL_REFRESH "  Scan for networks", COLOR_ACCENT, on_wifi_scan, &s_wifi_scan_lbl);
    s_wifi_list = column(list);

    row(list, LV_SYMBOL_EDIT, "Other network...", NULL, on_wifi_other, NULL);
    lv_obj_t *mac = info_row(list, "MAC address");
    uint8_t m[6];
    if (esp_read_mac(m, ESP_MAC_WIFI_STA) == ESP_OK) {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
        lv_label_set_text(mac, buf);
    }
    note(list, "Tap a saved network to switch to it. Hold it to forget it.");
}

static void tick_wifi(void)
{
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    char buf[128];
    switch (w.state) {
    case MUSE_WIFI_OFF:
        strlcpy(buf, "Wi-Fi is off", sizeof(buf));
        break;
    case MUSE_WIFI_NO_NETWORK:
        strlcpy(buf, "No saved networks. Scan and pick one.", sizeof(buf));
        break;
    case MUSE_WIFI_CONNECTING:
        snprintf(buf, sizeof(buf), "Joining %s\n%s", w.ssid, w.detail);
        break;
    case MUSE_WIFI_CONNECTED:
        snprintf(buf, sizeof(buf), "Connected to %s\n%s  -  %d dBm", w.ssid, w.ip, w.rssi);
        break;
    case MUSE_WIFI_NOT_NEARBY:
        strlcpy(buf, "No saved network nearby\nLooking again within a minute", sizeof(buf));
        break;
    case MUSE_WIFI_FAILED:
    default:
        snprintf(buf, sizeof(buf), "Couldn't join %s\n%s", w.ssid, w.detail);
        break;
    }
    set_text(s_wifi_status, buf);
    lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(
        w.state == MUSE_WIFI_CONNECTED ? COLOR_OK
        : w.state == MUSE_WIFI_FAILED ? COLOR_DANGER
        : w.state == MUSE_WIFI_OFF || w.state == MUSE_WIFI_NO_NETWORK || w.state == MUSE_WIFI_NOT_NEARBY ? COLOR_DIM
        : COLOR_WARN), 0);

    bool on = w.state != MUSE_WIFI_OFF;
    if (on != lv_obj_has_state(s_wifi_sw, LV_STATE_CHECKED)) {
        lv_obj_set_state(s_wifi_sw, LV_STATE_CHECKED, on);
    }
    lv_obj_set_flag(s_wifi_scan_btn, LV_OBJ_FLAG_HIDDEN, !on);
    lv_obj_set_flag(s_wifi_list, LV_OBJ_FLAG_HIDDEN, !on);
    set_text(s_wifi_scan_lbl, muse_wifi_scanning() ? "Scanning..." : LV_SYMBOL_REFRESH "  Scan for networks");
    rebuild_saved_list();
    tick_saved_list(&w);
    /* The scan found one: join it now rather than at the next look. */
    if (rebuild_scan_list() && w.state == MUSE_WIFI_NOT_NEARBY) {
        muse_wifi_apply();
    }
}

/* ---------- Hatch ---------- */

static void on_hatch_host_done(const char *text) { muse_settings_set_hatch_host(text); }
static void on_hatch_vm_done(const char *text) { muse_settings_set_hatch_vm(text); }

static void on_hatch_token_done(const char *text)
{
    if (text[0]) {
        muse_settings_set_hatch_token(text, false);
    }
}

static void on_hatch_host(lv_event_t *e)
{
    (void)e;
    char host[MUSE_HOST_MAX + 1];
    muse_settings_hatch_host(host);
    open_text("Muse server", host, false, MUSE_HOST_MAX, "Empty for the default", on_hatch_host_done, s_hatch);
}

static void on_hatch_vm(lv_event_t *e)
{
    (void)e;
    char vm[MUSE_VM_MAX + 1];
    muse_settings_hatch_vm(vm);
    open_text("VM ID", vm, false, MUSE_VM_MAX, "Optional", on_hatch_vm_done, s_hatch);
}

static void on_hatch_token(lv_event_t *e)
{
    (void)e;
    open_text("Device token", "", true, MUSE_TOKEN_MAX, "Empty keeps the current one", on_hatch_token_done, s_hatch);
}

static void on_hatch_test(lv_event_t *e)
{
    (void)e;
    muse_hatch_test();
}

/* Two taps within a few seconds: this wipes Wi-Fi and the Muse app pairing. */
static void on_link_reset(lv_event_t *e)
{
    (void)e;
    int64_t now = esp_timer_get_time();
    if (s_link_reset_armed_us && now - s_link_reset_armed_us < 5000000) {
        set_text(s_link_reset_lbl, "Resetting...");
        muse_link_reset_setup();
        return;
    }
    s_link_reset_armed_us = now;
    set_text(s_link_reset_lbl, "Tap again to reset");
}

static void build_hatch_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_hatch = page(tile, "MUSE", true, &list);
    s_link_reset_armed_us = 0;
    s_link_status = note(list, "");
    button(list, "Reset pairing", COLOR_DANGER, on_link_reset, &s_link_reset_lbl);
    s_hatch_status = note(list, "");
    row(list, NULL, "Server", &s_hatch_host, on_hatch_host, NULL);
    row(list, NULL, "VM ID", &s_hatch_vm, on_hatch_vm, NULL);
    row(list, NULL, "Device token", &s_hatch_token, on_hatch_token, NULL);
    button(list, "Test connection", COLOR_ACCENT, on_hatch_test, NULL);
    note(list, "Pair with the Muse app to use your account; a device token here overrides it, and a long one is "
               "easier to send over Bluetooth. The VM ID picks one of your VMs. "
               "Reset pairing forgets Wi-Fi and the app pairing, then restarts.");
}

static void tick_hatch(void)
{
    char link[64];
    snprintf(link, sizeof(link), "Muse app: %s\n%s", muse_link_hatch_linked() ? "paired" : "not paired",
             muse_link_state_name(muse_link_state()));
    set_text(s_link_status, link);
    if (s_link_reset_armed_us && esp_timer_get_time() - s_link_reset_armed_us >= 5000000) {
        s_link_reset_armed_us = 0;
        set_text(s_link_reset_lbl, "Reset pairing");
    }

    muse_hatch_status_t h;
    muse_hatch_status(&h);
    char buf[96];
    snprintf(buf, sizeof(buf), "%s\n%s", muse_hatch_state_name(h.state), h.detail);
    set_text(s_hatch_status, buf);
    lv_obj_set_style_text_color(s_hatch_status, lv_color_hex(h.state == MUSE_HATCH_REACHABLE ? COLOR_OK :
                                                             h.state == MUSE_HATCH_UNREACHABLE ? COLOR_DANGER : COLOR_DIM), 0);

    char host[MUSE_HOST_MAX + 1], vm[MUSE_VM_MAX + 1];
    muse_settings_hatch_host(host);
    muse_settings_hatch_vm(vm);
    set_text(s_hatch_host, host);
    set_text(s_hatch_vm, vm[0] ? vm : "Not set");
    size_t n = muse_settings_hatch_token_len();
    snprintf(buf, sizeof(buf), n ? "Set (%u chars)" : "Not set", (unsigned)n);
    set_text(s_hatch_token, buf);
}

/* ---------- Bluetooth ---------- */

static void on_ble_sw(lv_event_t *e)
{
    muse_settings_set_ble_on(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void on_ble_forget(lv_event_t *e)
{
    (void)e;
    muse_ble_forget_all();
}

static lv_obj_t *s_iphone_sw, *s_iphone_status, *s_alerts_sw;

static void on_alerts_sw(lv_event_t *e)
{
    muse_settings_set_iphone_alerts(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void on_iphone_sw(lv_event_t *e)
{
    muse_settings_set_iphone_on(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void build_ble_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_ble = page(tile, "BLUETOOTH", true, &list);
    s_ble_sw = switch_row(list, "Phone setup", muse_settings_ble_on(), on_ble_sw);
    s_ble_status = note(list, "");
    s_iphone_sw = switch_row(list, "iPhone", muse_settings_iphone_on(), on_iphone_sw);
    s_iphone_status = note(list, "");
    s_alerts_sw = switch_row(list, "iPhone alerts", muse_settings_iphone_alerts(), on_alerts_sw);
    button(list, "Forget paired phones", COLOR_DANGER, on_ble_forget, NULL);
    note(list, "When on, Muse is visible to phones nearby. Open tools/ble_setup.html in Chrome, "
               "connect, and enter the code Muse shows to pair.");
}

static void tick_ble(void)
{
    muse_ble_status_t b;
    muse_ble_status(&b);
    char buf[96];
    switch (b.state) {
    case MUSE_BLE_OFF:
        strlcpy(buf, "Off", sizeof(buf));
        break;
    case MUSE_BLE_ADVERTISING:
        snprintf(buf, sizeof(buf), "Visible as %s", b.name);
        break;
    case MUSE_BLE_CONNECTED:
    default:
        snprintf(buf, sizeof(buf), "Phone connected\n%s", b.secure ? "Paired" : "Waiting for pairing");
        break;
    }
    set_text(s_ble_status, buf);
    lv_obj_set_style_text_color(s_ble_status, lv_color_hex(b.state == MUSE_BLE_CONNECTED && b.secure ? COLOR_OK : COLOR_DIM), 0);
    bool on = muse_settings_ble_on();
    if (on != lv_obj_has_state(s_ble_sw, LV_STATE_CHECKED)) {
        lv_obj_set_state(s_ble_sw, LV_STATE_CHECKED, on);
    }
    static const char *const iphone_text[] = {
        "Off",
        "On your iPhone: Settings > Bluetooth, tap this gadget, enter the code it shows. "
        "Then allow Share System Notifications.",
        "Pairing...", "iPhone connected: notifications and media",
    };
    muse_iphone_state_t st = muse_iphone_state();
    set_text(s_iphone_status, iphone_text[st]);
    lv_obj_set_style_text_color(s_iphone_status, lv_color_hex(st == MUSE_IPHONE_CONNECTED ? COLOR_OK : COLOR_DIM), 0);
    bool ion = muse_settings_iphone_on();
    if (ion != lv_obj_has_state(s_iphone_sw, LV_STATE_CHECKED)) {
        lv_obj_set_state(s_iphone_sw, LV_STATE_CHECKED, ion);
    }
}

/* ---------- Sound ---------- */

static void set_val(lv_obj_t *l, const char *fmt, int v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), fmt, v);
    set_text(l, buf);
}

static void on_speaker_sw(lv_event_t *e)
{
    muse_settings_set_speaker_on(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void on_volume(lv_event_t *e)
{
    int v = lv_slider_get_value(s_vol_sl);
    set_val(s_vol_val, "%d%%", v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        muse_settings_set_volume(v);
        muse_voice_request_chirp();
    } else {
        muse_audio_set_volume(v);
    }
}

static void on_gain(lv_event_t *e)
{
    int db = lv_slider_get_value(s_gain_sl) * 3;
    set_val(s_gain_val, "%d dB", db);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        muse_settings_set_mic_gain(db);
    } else {
        muse_audio_set_mic_gain(db);
    }
}

static void on_bright(lv_event_t *e)
{
    int v = lv_slider_get_value(s_bright_sl);
    set_val(s_bright_val, "%d%%", v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        muse_settings_set_brightness(v);
    } else {
        muse_ui_preview_brightness(v);
    }
}

static void build_sound_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_sound = page(tile, "SOUND", true, &list);
    s_spk_sw = switch_row(list, "Speaker", muse_settings_speaker_on(), on_speaker_sw);
    s_vol_sl = slider(list, "Volume", 0, 100, muse_settings_volume(), &s_vol_val, on_volume);
    s_gain_sl = slider(list, "Mic gain", 0, MUSE_MIC_GAIN_MAX / 3, muse_settings_mic_gain() / 3, &s_gain_val, on_gain);

    lv_obj_t *meter = lv_obj_create(list);
    lv_obj_remove_style_all(meter);
    lv_obj_set_size(meter, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(meter, 8, 0);
    lv_obj_remove_flag(meter, LV_OBJ_FLAG_SCROLLABLE);
    label(meter, &lv_font_montserrat_16, COLOR_DIM, "Mic level");
    s_mic_val = label(meter, &lv_font_montserrat_16, COLOR_DIM, "");
    lv_obj_align(s_mic_val, LV_ALIGN_TOP_RIGHT, 0, 0);
    s_mic_bar = lv_bar_create(meter);
    lv_obj_set_size(s_mic_bar, lv_pct(94), 10);
    lv_obj_align(s_mic_bar, LV_ALIGN_TOP_MID, 0, 26);
    lv_bar_set_range(s_mic_bar, 0, 60);   /* -70..-10 dBFS */
    lv_obj_set_style_bg_color(s_mic_bar, lv_color_hex(0x1a3a24), LV_PART_MAIN);
    lv_obj_set_style_anim_duration(s_mic_bar, 80, 0);
    note(list, "Talk at arm's length: the bar should reach green (-30 to -15 dBFS) without going orange.");

    s_bright_sl = slider(list, "Brightness", 10, 100, muse_settings_brightness(), &s_bright_val, on_bright);

    set_val(s_vol_val, "%d%%", muse_settings_volume());
    set_val(s_gain_val, "%d dB", muse_settings_mic_gain() / 3 * 3);
    set_val(s_bright_val, "%d%%", muse_settings_brightness());
}

static void tick_sound(void)
{
    bool on = muse_settings_speaker_on();   /* also toggled from the face */
    if (on != lv_obj_has_state(s_spk_sw, LV_STATE_CHECKED)) {
        lv_obj_set_state(s_spk_sw, LV_STATE_CHECKED, on);
    }
    float db = muse_voice_monitor_db();
    int v = (int)(db + 70.0f);
    v = v < 0 ? 0 : (v > 60 ? 60 : v);
    lv_bar_set_value(s_mic_bar, v, LV_ANIM_ON);
    uint32_t color = db > -12.0f ? COLOR_WARN : (db > -30.0f ? COLOR_OK : COLOR_ACCENT);
    lv_obj_set_style_bg_color(s_mic_bar, lv_color_hex(color), LV_PART_INDICATOR);
    set_val(s_mic_val, "%d dBFS", (int)db);
}

/* ---------- Sleep ---------- */

static void on_sleep_choice(lv_event_t *e)
{
    muse_settings_set_sleep_s(SLEEP_CHOICES[(int)(intptr_t)lv_event_get_user_data(e)]);
}

static void on_sleep_now(lv_event_t *e)
{
    (void)e;
    muse_state_set_asleep(true);
}

static void build_sleep_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_sleep = page(tile, "AUTO-SLEEP", true, &list);
    note(list, "Turn the screen off after Muse has been idle for:");
    for (int i = 0; i < SLEEP_COUNT; i++) {
        row(list, NULL, SLEEP_NAMES[i], &s_sleep_checks[i], on_sleep_choice, (void *)(intptr_t)i);
        lv_obj_set_style_text_color(s_sleep_checks[i], lv_color_hex(COLOR_ACCENT), 0);
    }
    button(list, LV_SYMBOL_EYE_CLOSE "  Sleep now", COLOR_ACCENT, on_sleep_now, NULL);
    note(list, "Press the button to wake; a screen tap also works while on USB.");
}

static const char *sleep_name(int secs)
{
    for (int i = 0; i < SLEEP_COUNT; i++) {
        if (SLEEP_CHOICES[i] == secs) {
            return SLEEP_NAMES[i];
        }
    }
    return "Custom";
}

static void tick_sleep(void)
{
    int cur = muse_settings_sleep_s();
    for (int i = 0; i < SLEEP_COUNT; i++) {
        set_text(s_sleep_checks[i], SLEEP_CHOICES[i] == cur ? LV_SYMBOL_OK : "");
    }
}

/* ---------- Battery ---------- */

static lv_obj_t *s_batt_power;

static void on_battery_reset(lv_event_t *e)
{
    (void)e;
    muse_battery_reset();
    s_batt_shown_us = 0;   /* show it now */
}

static void build_battery_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_battery = page(tile, "BATTERY", true, &list);
    s_batt_shown_us = 0;
    s_batt_status = note(list, "");
    s_batt_level = info_row(list, "Battery");
    s_batt_power = info_row(list, "Power");
    s_batt_drain = info_row(list, "Used");
    s_batt_full = info_row(list, "A full charge");
    s_batt_off = info_row(list, "Screen off");
    s_batt_slept = info_row(list, "Chip asleep");
    s_batt_wakes = info_row(list, "Wakes");
    s_batt_busy = info_row(list, "CPU busy");
    s_batt_awake = note(list, "");
    button(list, LV_SYMBOL_REFRESH "  Start over", COLOR_ACCENT, on_battery_reset, NULL);
    note(list, "Measures from unplugging USB until it's plugged back in. The gauge moves in 1% steps, so give it a "
               "few hours. Chip asleep is time in light sleep; CPU busy is time a core was running a task.");
}

/* A per-mille figure as a percentage. */
static void set_pm(lv_obj_t *l, int pm)
{
    char buf[16] = "-";
    if (pm >= 0) {
        snprintf(buf, sizeof(buf), "%d.%d%%", pm / 10, pm % 10);
    }
    set_text(l, buf);
}

static void tick_battery(void)
{
    int64_t now = esp_timer_get_time();
    if (s_batt_shown_us && now - s_batt_shown_us < 1000000) {
        return;
    }
    s_batt_shown_us = now;
    muse_battery_t b;
    muse_battery_read(&b);
    muse_power_t p = muse_state_power();
    char buf[96], t[24];

    int h = (int)(b.secs / 3600), m = (int)(b.secs / 60 % 60);
    if (h) {
        snprintf(t, sizeof(t), "%d h %d min", h, m);
    } else {
        snprintf(t, sizeof(t), "%d min", m);
    }
    if (!b.started) {
        strlcpy(buf, p.battery_pct < 0 ? "No battery" : "Unplug USB to start measuring.", sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), b.running ? "On battery for %s" : "Last run: %s on battery", t);
    }
    set_text(s_batt_status, buf);

    if (p.battery_pct < 0) {
        strlcpy(buf, "None", sizeof(buf));
    } else if (p.battery_mv) {
        snprintf(buf, sizeof(buf), "%s%d%%  %d.%02d V", p.charging ? LV_SYMBOL_CHARGE " " : "", p.battery_pct,
                 p.battery_mv / 1000, p.battery_mv % 1000 / 10);
    } else {
        snprintf(buf, sizeof(buf), "%s%d%%", p.charging ? LV_SYMBOL_CHARGE " " : "", p.battery_pct);
    }
    set_text(s_batt_level, buf);

    if (p.charging) {
        strlcpy(buf, "Charging", sizeof(buf));
    } else if (p.usb) {
        strlcpy(buf, "On USB, not charging", sizeof(buf));
    } else if (p.battery_pct < 0) {
        strlcpy(buf, "No battery", sizeof(buf));
    } else {
        strlcpy(buf, "On battery", sizeof(buf));
    }
    set_text(s_batt_power, buf);

    int used = b.pct_start - b.pct_now, rate10, full_h;
    if (!b.started) {
        set_text(s_batt_drain, "-");
        set_text(s_batt_full, "-");
    } else if (muse_battery_drain(&b, &rate10, &full_h)) {
        snprintf(buf, sizeof(buf), "%d%%, %d.%d%%/h", used, rate10 / 10, rate10 % 10);
        set_text(s_batt_drain, buf);
        snprintf(buf, sizeof(buf), "lasts ~%d h", full_h);
        set_text(s_batt_full, buf);
    } else {
        snprintf(buf, sizeof(buf), "%d%% so far", used > 0 ? used : 0);
        set_text(s_batt_drain, buf);
        set_text(s_batt_full, "measuring");
    }

    set_pm(s_batt_off, b.started ? b.screen_off_pm : -1);
    set_pm(s_batt_slept, b.started ? b.slept_pm : -1);
    set_pm(s_batt_busy, b.started ? b.busy_pm : -1);
    if (b.started && b.secs && b.slept_pm >= 0) {
        int per10 = (int)(b.sleeps * 10LL / b.secs);
        snprintf(buf, sizeof(buf), "%d.%d/s", per10 / 10, per10 % 10);
        set_text(s_batt_wakes, buf);
    } else {
        set_text(s_batt_wakes, "-");
    }
    buf[0] = '\0';
    if (b.started && b.awake[0]) {
        snprintf(buf, sizeof(buf), "Also kept awake by: %s", b.awake);
    }
    set_text(s_batt_awake, buf);
}

/* ---------- Power ---------- */

static void on_power_off(lv_event_t *e)
{
    (void)e;
    show(s_home);
    muse_ui_show_face();
    muse_input_request_power_off();
}

static void on_restart(lv_event_t *e)
{
    (void)e;
    show(s_home);
    muse_ui_show_face();
    muse_input_request_restart();
}

static void build_power_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_power = page(tile, "POWER", true, &list);
    note(list, "Restart the gadget? The screen goes dark for a few seconds.");
    button(list, LV_SYMBOL_REFRESH "  Restart", COLOR_TEXT, on_restart, NULL);
    button(list, LV_SYMBOL_POWER "  Power off", COLOR_DANGER, on_power_off, NULL);
    button(list, "Cancel", COLOR_TEXT, on_back, NULL);
    char text[160];
    if (muse_board->aux_button) {
        snprintf(text, sizeof(text), "Press the %s button to turn it back on. To just turn the screen off, press the %s button.",
                 muse_board->talk_button, muse_board->aux_button);
    } else {
        snprintf(text, sizeof(text), "This board's power switch is the PWR button (hold to cut power). "
                                     "Frozen? Hold %s for 20 s.", muse_board->talk_button);
    }
    note(list, text);
}

/* ---------- Usage ---------- */

#define TTS_FREE_CHARS 1000000u   /* Google's monthly free allowance for Chirp 3 HD voices, about */
static lv_obj_t *s_use_chars, *s_use_left, *s_use_up, *s_use_net, *s_use_mem;
static int64_t s_use_shown_us;

static void on_usage_reset(lv_event_t *e)
{
    (void)e;
    muse_settings_tts_reset();
    s_use_shown_us = 0;
}

static void build_usage_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_usage = page(tile, "USAGE", true, &list);
    s_use_shown_us = 0;
    s_use_chars = info_row(list, "Voice characters");
    s_use_left = info_row(list, "Free tier left");
    s_use_up = info_row(list, "Uptime");
    s_use_net = info_row(list, "Network");
    s_use_mem = info_row(list, "Free memory");
    button(list, LV_SYMBOL_REFRESH "  Reset counter", COLOR_ACCENT, on_usage_reset, NULL);
    note(list, "Counts the characters spoken through Google Text-to-Speech since the last reset. The free allowance "
               "is about 1M characters a month; reset this on the 1st. Google Cloud has the exact figure.");
}

static void tick_usage(void)
{
    int64_t now = esp_timer_get_time();
    if (now - s_use_shown_us < 1000000) {
        return;
    }
    s_use_shown_us = now;
    char buf[48];
    uint32_t used = muse_settings_tts_chars();
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)used);
    set_text(s_use_chars, buf);
    snprintf(buf, sizeof(buf), "%.1f%%", used >= TTS_FREE_CHARS ? 0.0 : 100.0 - 100.0 * (double)used / TTS_FREE_CHARS);
    set_text(s_use_left, buf);
    int up = (int)(now / 1000000);
    snprintf(buf, sizeof(buf), "%dh %02dm", up / 3600, (up / 60) % 60);
    set_text(s_use_up, buf);
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    if (w.state == MUSE_WIFI_CONNECTED) {
        snprintf(buf, sizeof(buf), "%s  %d dBm", w.ssid, w.rssi);
    } else {
        strlcpy(buf, "offline", sizeof(buf));
    }
    set_text(s_use_net, buf);
    snprintf(buf, sizeof(buf), "%u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    set_text(s_use_mem, buf);
}

/* ---------- Home ---------- */

/* ---------- Agents ---------- */

/* A tappable row whose NAME label is returned so it can be re-labelled, plus a right-side
 * check label. row() returns only the value label; the agent rows need the name too. */
/* A row's icon is the agent's OWN mark (a real image), not a generic UI symbol: Meta's logo
 * for Muse, the Hermes character for the Hermes slots. Built once from the embedded
 * RGB565A8 data; NULL falls back to no icon. */
static const lv_image_dsc_t *agent_icon(int idx)
{
    static lv_image_dsc_t meta_dsc, hermes_dsc;
    static bool built;
    if (!built) {
        built = true;
        meta_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        meta_dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
        meta_dsc.header.w = ICON_META_W;
        meta_dsc.header.h = ICON_META_H;
        meta_dsc.header.stride = ICON_META_W * 2;
        meta_dsc.data = ICON_META_DATA;
        meta_dsc.data_size = sizeof(ICON_META_DATA);
        hermes_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        hermes_dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
        hermes_dsc.header.w = ICON_HERMES_W;
        hermes_dsc.header.h = ICON_HERMES_H;
        hermes_dsc.header.stride = ICON_HERMES_W * 2;
        hermes_dsc.data = ICON_HERMES_DATA;
        hermes_dsc.data_size = sizeof(ICON_HERMES_DATA);
    }
    return idx == MUSE_AGENT_MUSE ? &meta_dsc : &hermes_dsc;
}

static lv_obj_t *agent_row(lv_obj_t *list, const lv_image_dsc_t *icon, lv_obj_t **name_out,
                           lv_obj_t **check_out, lv_event_cb_t cb, void *user)
{
    lv_obj_t *c = card(list, true);
    if (icon) {
        lv_obj_t *img = lv_image_create(c);
        lv_image_set_src(img, icon);
        lv_obj_set_size(img, ICON_META_W, ICON_META_H);   /* both icons are the same 40x40 */
    } else {
        label(c, &lv_font_montserrat_20, COLOR_ACCENT, LV_SYMBOL_SHUFFLE);
    }
    lv_obj_t *t = label(c, &lv_font_montserrat_20, COLOR_TEXT, "");
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(t, lv_font_get_line_height(&lv_font_montserrat_20));   /* long profile names ellipsize, never wrap */
    lv_obj_t *v = label(c, &lv_font_montserrat_16, COLOR_ACCENT, "");
    lv_obj_set_style_max_width(v, 130, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(v, lv_font_get_line_height(&lv_font_montserrat_16));
    lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, user);
    *name_out = t;
    *check_out = v;
    return c;
}

static void on_agent_pick(lv_event_t *e)
{
    /* Direct, not a cycle: tap the one you want. ALT with no profile name cannot be routed,
     * so refuse it here rather than strand the turn. */
    int idx = AGENT_CHOICES[(int)(intptr_t)lv_event_get_user_data(e)];
    if (idx == MUSE_AGENT_HERMES_ALT) {
        char alt[MUSE_AGENT_PROFILE_MAX + 1];
        muse_settings_agent_alt(alt);
        if (!alt[0]) {
            return;
        }
    }
    muse_settings_set_agent(idx);
}

static void on_agent_profile_done(const char *name)
{
    muse_settings_set_agent_profile(name);
}

static void on_agent_profile(lv_event_t *e)
{
    (void)e;
    char alt[MUSE_AGENT_PROFILE_MAX + 1];
    muse_settings_agent_alt(alt);
    open_text("Alt profile", alt, false, MUSE_AGENT_PROFILE_MAX, "e.g. sidekick",
              on_agent_profile_done, s_agents);
}

static void build_agents_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_agents = page(tile, "AGENTS", true, &list);
    note(list, "Talk to one of these. Two quick taps of the talk button also cycles them.");
    for (int i = 0; i < AGENT_COUNT; i++) {
        int idx = AGENT_CHOICES[i];
        agent_row(list, agent_icon(idx), &s_agent_labels[i], &s_agent_checks[i],
                  on_agent_pick, (void *)(intptr_t)i);
    }
    row(list, LV_SYMBOL_EDIT, "Alt profile name...", NULL, on_agent_profile, NULL);
    note(list, "The alt slot is one of your own Hermes profiles: set its name to enable it. "
               "Its looks and voice come from that profile.");
}

static void tick_agents(void)
{
    char name[MUSE_AGENT_PROFILE_MAX + 1];
    char alt[MUSE_AGENT_PROFILE_MAX + 1];
    muse_settings_agent_alt(alt);
    int cur = muse_settings_agent();
    for (int i = 0; i < AGENT_COUNT; i++) {
        int idx = AGENT_CHOICES[i];
        bool off = (idx == MUSE_AGENT_HERMES_ALT && !alt[0]);   /* can't be routed yet */
        muse_settings_agent_name(idx, name);
        set_text(s_agent_labels[i], name);
        set_text(s_agent_checks[i], idx == cur ? LV_SYMBOL_OK : "");
        lv_obj_set_style_text_color(s_agent_labels[i], lv_color_hex(off ? COLOR_DIM : COLOR_TEXT), 0);
        lv_obj_t *r = lv_obj_get_parent(s_agent_labels[i]);
        if (r) {
            lv_obj_set_style_opa(r, off ? LV_OPA_60 : LV_OPA_COVER, 0);
        }
    }
}

/* ---------- Notifications ---------- */

/* The value column: "in 2h" for an un-fired reminder, "3m ago" for the rest. */
static void notif_when(long ts, char *out, size_t cap)
{
    long now = (long)time(NULL);
    if (now < 1700000000 || ts <= 0) {
        out[0] = 0;   /* no clock yet: a wall-clock column would lie */
        return;
    }
    long d = ts > now ? ts - now : now - ts;
    char n[16];
    if (d < 60) {
        snprintf(n, sizeof(n), "%lds", d);
    } else if (d < 5400) {
        snprintf(n, sizeof(n), "%ldm", d / 60);
    } else if (d < 86400 + 43200) {
        snprintf(n, sizeof(n), "%ldh", d / 3600);
    } else {
        snprintf(n, sizeof(n), "%ldd", d / 86400);
    }
    snprintf(out, cap, ts > now ? "in %s" : "%s ago", n);
}

/* A notification line: bell, "<LABEL>  <message>", right-aligned when. row() does not hand
 * back its text label, and an unread line is tinted, so this is its own small builder. The
 * card GROWS with the wrapped message (a fixed ROW_H clipped a 3-line nudge against its own
 * bottom edge) but never shrinks below the shared row height. Note the TAP is registered on
 * SHORT_CLICKED, not CLICKED: CLICKED fires on a long press's release too, so "hold to remove"
 * would also have opened the row (muse_ui.c documents the same trap for the face). */
static lv_obj_t *notif_row(lv_obj_t *list, lv_obj_t **text_out, lv_obj_t **value_out)
{
    lv_obj_t *c = card(list, true);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(c, ROW_H, 0);
    lv_obj_set_style_pad_ver(c, 10, 0);
    label(c, &lv_font_montserrat_16, COLOR_ACCENT, LV_SYMBOL_BELL);
    lv_obj_t *t = label(c, &lv_font_montserrat_16, COLOR_TEXT, "");
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *v = label(c, &lv_font_montserrat_16, COLOR_DIM, "");
    lv_obj_set_style_max_width(v, 110, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(v, lv_font_get_line_height(&lv_font_montserrat_16));
    *text_out = t;
    *value_out = v;
    return c;
}

/* Tap: switch to the agent that raised it (same meaning as tapping a live nudge). It is NOT
 * acked: read means "has been shown unprompted", so an un-fired reminder must still fire. If
 * the agent is not routable, fall back to its slot: `agent` for ALT is only applied when set. */
static void on_notif_open(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_notif_n) {
        return;
    }
    const muse_notif_row_t *r = &s_notif_rows[i];
    if (r->is_default) {
        muse_settings_set_agent(MUSE_AGENT_HERMES);
    } else {
        /* The profile may have been removed since the nudge was raised: set_agent would then
         * fall to Muse (a different agent entirely). Land on the Hermes slot instead. */
        muse_settings_set_agent_profile(r->agent);
        char alt[MUSE_AGENT_PROFILE_MAX + 1];
        muse_settings_agent_alt(alt);
        muse_settings_set_agent(alt[0] ? MUSE_AGENT_HERMES_ALT : MUSE_AGENT_HERMES);
    }
    muse_state_set_caption("OPENED %s", r->label[0] ? r->label : r->agent);
    muse_state_poke();
}

/* Hold: remove ONE line on the relay -- cancelling an un-fired reminder, or clearing a past
 * line. The row disappears when the confirming refresh lands (a second or two). */
static void on_notif_hold(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_notif_n) {
        return;
    }
    const muse_notif_row_t *r = &s_notif_rows[i];
    muse_hatch_notif_cancel(r->id);
    muse_state_set_caption("REMOVING %s...", r->label[0] ? r->label : r->agent);
    muse_state_poke();
}

/* Clear the whole history in two taps (the same arm-then-confirm as "Reset pairing"): one
 * tap is too easy to hit by accident, and this is the only destructive bulk action here.
 * Un-fired reminders survive a clear -- they are commitments, not history. */
static void on_notif_clear(lv_event_t *e)
{
    (void)e;
    int64_t now = esp_timer_get_time();
    if (s_notif_clear_armed_us && now - s_notif_clear_armed_us < 5000000) {
        s_notif_clear_armed_us = 0;
        set_text(s_notif_clear_lbl, "Clearing...");
        muse_hatch_notif_clear();
        return;
    }
    s_notif_clear_armed_us = now;
    set_text(s_notif_clear_lbl, "Tap again to clear");
}

static void build_notif_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_notif = page(tile, "NOTIFICATIONS", true, &list);
    s_notif_status = note(list, "");
    /* Clear sits high where it is found, like every notification center: status line, then
     * Clear, then the rows. Two-tap confirm guards it (see on_notif_clear). */
    s_notif_clear_armed_us = 0;
    button(list, LV_SYMBOL_TRASH "  Clear", COLOR_DANGER, on_notif_clear, &s_notif_clear_lbl);
    s_notif_list = column(list);
    s_notif_shown_gen = UINT32_MAX;   /* first tick fills it */
    note(list, "Tap a line to talk to that agent; hold it to remove it. Clear empties the "
               "list but keeps reminders that have not gone off yet. Timers you set arrive "
               "here, as do print results.");
}

static void tick_notif(void)
{
    /* The refresh rate is bounded in the session task; asking on this tick keeps the page
     * live, and asking is free when a fetch is already queued. */
#ifndef CONFIG_MUSE_HATCH
    set_text(s_notif_status, "Not available on this board");   /* no session, no inbox */
    return;
#else
    muse_notif_hist_t st = muse_hatch_notif_hist_state();
    int count = muse_hatch_notif_hist_count();
    if (!muse_settings_relay_configured()) {
        set_text(s_notif_status, "Hermes link not set up");
    } else if (!muse_settings_agent_hermes()) {
        set_text(s_notif_status, "Switch to a Hermes agent to see these");
    } else if (st == MUSE_NOTIF_HIST_IDLE || st == MUSE_NOTIF_HIST_WAIT) {
        set_text(s_notif_status, count ? "" : "Checking...");
    } else if (st == MUSE_NOTIF_HIST_FAIL) {
        set_text(s_notif_status, "Can't reach the relay");
    } else if (count == 0) {
        set_text(s_notif_status, "Nothing yet");
    } else {
        set_text(s_notif_status, "");
    }
    /* An empty note is a dead line of padding under the title: hide it, don't blank it. */
    if (s_notif_status) {
        lv_obj_set_flag(s_notif_status, LV_OBJ_FLAG_HIDDEN, lv_label_get_text(s_notif_status)[0] == 0);
    }
    /* The arm window is one-shot: after 5 s it lapses and the button re-arms on the next
     * tap. Without this the label would stay "Tap again to clear" forever after a tap that
     * was never confirmed, and you could not tell the button was idle again. */
    if (s_notif_clear_armed_us && esp_timer_get_time() - s_notif_clear_armed_us >= 5000000) {
        s_notif_clear_armed_us = 0;
    }
    /* The label follows the state: armed while the confirm window is open, back to normal
     * once the window lapses or a fetch confirms what the clear left behind. */
    if (s_notif_clear_lbl && !s_notif_clear_armed_us && st == MUSE_NOTIF_HIST_OK) {
        set_text(s_notif_clear_lbl, LV_SYMBOL_TRASH "  Clear");
    }
    if (muse_settings_relay_configured() && muse_settings_agent_hermes() && st != MUSE_NOTIF_HIST_WAIT) {
        static int64_t s_ask_us;
        int64_t gap = st == MUSE_NOTIF_HIST_FAIL ? 6000000 : 1200000;   /* slow down when down */
        if (esp_timer_get_time() - s_ask_us > gap) {
            s_ask_us = esp_timer_get_time();
            muse_hatch_notif_refresh();
        }
    }
    uint32_t gen = muse_hatch_notif_hist_gen();
    if (gen == s_notif_shown_gen) {
        return;
    }
    s_notif_shown_gen = gen;
    s_notif_n = 0;
    lv_obj_clean(s_notif_list);
    for (int i = 0; i < NOTIF_ROWS_MAX; i++) {
        muse_notif_row_t r;
        if (!muse_hatch_notif_hist(i, &r)) {
            break;
        }
        s_notif_rows[s_notif_n++] = r;
        lv_obj_t *t, *v;
        lv_obj_t *c = notif_row(s_notif_list, &t, &v);
        char text[200];
        snprintf(text, sizeof(text), "%s  %s", r.label[0] ? r.label : r.agent, r.text);
        set_text(t, text);
        lv_obj_set_style_text_color(t, lv_color_hex(r.read ? COLOR_TEXT : COLOR_ACCENT), 0);
        char when[24];
        long now = (long)time(NULL);
        notif_when(r.due > 0 && r.due > now ? r.due : r.ts, when, sizeof(when));
        set_text(v, when);
        lv_obj_set_style_text_color(v, lv_color_hex(r.due > 0 && r.due > now ? COLOR_WARN : COLOR_DIM), 0);
        lv_obj_add_event_cb(c, on_notif_open, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)(s_notif_n - 1));
        lv_obj_add_event_cb(c, on_notif_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)(s_notif_n - 1));
    }
#endif   /* CONFIG_MUSE_HATCH */
}

static const page_t WIFI = { &s_wifi, build_wifi_page };
static const page_t HATCH = { &s_hatch, build_hatch_page };
static const page_t BLE = { &s_ble, build_ble_page };
static const page_t SOUND = { &s_sound, build_sound_page };
static const page_t SLEEP = { &s_sleep, build_sleep_page };
static const page_t BATTERY = { &s_battery, build_battery_page };
static const page_t USAGE = { &s_usage, build_usage_page };
static const page_t POWER = { &s_power, build_power_page };
static const page_t AGENTS = { &s_agents, build_agents_page };
static const page_t NOTIF = { &s_notif, build_notif_page };

/* Bench (serial task): open the notifications page directly -- the lock is the caller's
 * (muse_ui_show_notifications in muse_ui.c). s_hold_page tells the next visibility tick not
 * to reset to the menu: the bench deliberately jumps straight to a sub-page. */
void muse_settings_ui_show_notifications(void)
{
    if (!s_tile) {
        return;
    }
    if (!s_notif) {
        build_notif_page(s_tile);
    }
    s_hold_page = true;
    show(s_notif);
    muse_settings_ui_tick(true);
}

/* Back to the menu from any sub-page (see the header). Scrolls the list HOME too: a menu
 * that keeps an old scroll position (you left it showing "Power off") also reads as
 * "the settings are gone" when you come back. */
void muse_settings_ui_show_home(void)
{
    if (!s_tile) {
        return;
    }
    if (s_current == s_text) {
        close_text();   /* leave the text page to the page that opened it, as go_back does */
    }
    show(s_home);
    if (s_home_list) {
        lv_obj_scroll_to_y(s_home_list, 0, LV_ANIM_OFF);
    }
    muse_settings_ui_tick(true);
}

static void build_home(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_home = page(tile, "SETTINGS", false, &list);
    s_home_list = list;
    if (muse_settings_relay_configured()) {
        row(list, LV_SYMBOL_SHUFFLE, "Agents", &s_home_agent, on_nav, (void *)&AGENTS);
        row(list, LV_SYMBOL_BELL, "Notifications", &s_home_notif, on_nav, (void *)&NOTIF);
    }
    row(list, LV_SYMBOL_WIFI, "Wi-Fi", &s_home_wifi, on_nav, (void *)&WIFI);
    row(list, LV_SYMBOL_HOME, "Muse", &s_home_hatch, on_nav, (void *)&HATCH);
    if (muse_settings_relay_configured()) {
        /* Hermes is a separate transport (your own relay); show its link on its own row so a
         * Muse outage never reads as "Hermes is down". No navigation: the relay address is
         * set from the setup page or the build, not on the glass. */
        lv_obj_t *hermes_row = row(list, LV_SYMBOL_UPLOAD, "Hermes", &s_home_hermes, NULL, NULL);
        lv_obj_set_flag(hermes_row, LV_OBJ_FLAG_CLICKABLE, false);   /* no action: do not fake one */
    }
    row(list, LV_SYMBOL_BLUETOOTH, "Bluetooth", &s_home_ble, on_nav, (void *)&BLE);
    row(list, LV_SYMBOL_VOLUME_MAX, "Sound", &s_home_sound, on_nav, (void *)&SOUND);
    row(list, LV_SYMBOL_EYE_CLOSE, "Sleep", &s_home_sleep, on_nav, (void *)&SLEEP);
    row(list, LV_SYMBOL_BATTERY_FULL, "Battery", &s_home_battery, on_nav, (void *)&BATTERY);
    row(list, LV_SYMBOL_LIST, "Usage", NULL, on_nav, (void *)&USAGE);
    row(list, LV_SYMBOL_POWER, "Power off", NULL, on_nav, (void *)&POWER);
    s_about = note(list, "");
}

static void tick_home(void)
{
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    static const char *const WIFI_VALUES[] = { "Off", "Not set", "Joining", "", "Failed", "Not nearby" };
    set_text(s_home_wifi, w.state == MUSE_WIFI_CONNECTED ? w.ssid : WIFI_VALUES[w.state]);

    if (s_home_notif) {
        /* Still-coming count: unread (never presented) or an un-fired reminder. Recounted only
         * when the snapshot changes: the rows are a few hundred bytes each and the tick is hot. */
        static uint32_t shown_gen;
        static int att;
        if (muse_hatch_notif_hist_gen() != shown_gen) {
            shown_gen = muse_hatch_notif_hist_gen();
            long now = (long)time(NULL);
            att = 0;
            for (int i = 0; i < NOTIF_ROWS_MAX; i++) {
                muse_notif_row_t r;
                if (!muse_hatch_notif_hist(i, &r)) {
                    break;
                }
                if (!r.read || (r.due > 0 && r.due > now)) {
                    att++;
                }
            }
        }
        set_val(s_home_notif, att ? "%d" : "", att);
        /* Keep the count fresh while the menu is open (the session task spaces the actual
         * fetches to one per couple of seconds). */
        static int64_t s_ask_us;
        if (esp_timer_get_time() - s_ask_us > 4000000) {
            s_ask_us = esp_timer_get_time();
            muse_hatch_notif_refresh();
        }
    }

    if (s_home_agent) {
        char alt[MUSE_AGENT_PROFILE_MAX + 1];
        muse_settings_agent_profile(alt);
        switch (muse_settings_agent()) {
        case MUSE_AGENT_HERMES_ALT:
            set_text(s_home_agent, alt[0] ? alt : "Alt");
            break;
        case MUSE_AGENT_HERMES:
            set_text(s_home_agent, "Hermes");
            break;
        default:
            set_text(s_home_agent, "Muse");
            break;
        }
    }
    muse_hatch_status_t h;
    muse_hatch_status(&h);
    set_text(s_home_hatch, muse_hatch_state_name(h.state));
    if (s_home_hermes) {
        /* Hermes's own link, independent of Muse's. "Idle" until a turn has run, since the
         * firmware only learns the relay's state from actual turns (no polling). */
        switch (muse_hatch_hermes_link()) {
        case 1: set_text(s_home_hermes, "Connected"); break;
        case 2: set_text(s_home_hermes, "Can't reach"); break;
        default: set_text(s_home_hermes, muse_settings_agent_hermes() ? "Ready" : "Not active"); break;
        /* (unchanged: true for either Hermes slot, so ALT reads Ready too) */
        }
    }

    muse_ble_status_t b;
    muse_ble_status(&b);
    set_text(s_home_ble, b.state == MUSE_BLE_OFF ? "Off" : (b.state == MUSE_BLE_CONNECTED ? "Connected" : "On"));

    if (muse_settings_speaker_on()) {
        set_val(s_home_sound, "Vol %d%%", muse_settings_volume());
    } else {
        set_text(s_home_sound, "Muted");
    }
    set_text(s_home_sleep, sleep_name(muse_settings_sleep_s()));

    muse_power_t p = muse_state_power();
    char buf[96];
    if (p.battery_pct < 0) {
        strlcpy(buf, "USB", sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "%s%d%%", p.charging ? LV_SYMBOL_CHARGE " " : "", p.battery_pct);
    }
    set_text(s_home_battery, buf);

    snprintf(buf, sizeof(buf), "Muse %s  -  %s", esp_app_get_description()->version,
             w.state == MUSE_WIFI_CONNECTED ? w.ip : "offline");
    set_text(s_about, buf);
}

/* ---------- public ---------- */

void muse_settings_ui_build(lv_obj_t *tile)
{
    if (muse_board->round && muse_board->height < 466) {
        s_text_scale = muse_board->height;
    }
    s_tile = tile;
    build_home(tile);
    show(s_home);
}

void muse_settings_ui_tick(bool visible)
{
    static bool was_visible;
    if (visible != was_visible) {
        was_visible = visible;
        if (visible && !s_hold_page && s_current != s_home) {
            /* ARRIVING at Settings shows the menu. A sub-page that was left open stayed
             * CURRENT (show() only hid it), so swiping back to Settings re-showed it and the
             * menu rows read as "all the settings are gone". Reset the scroll too: a menu
             * left showing its last rows reads the same way. */
            show(s_home);
        }
        if (visible && s_current == s_home && s_home_list) {
            lv_obj_scroll_to_y(s_home_list, 0, LV_ANIM_OFF);
        }
        /* Only listen to the mic while the Sound page is actually on screen. */
        muse_voice_set_monitor(visible && s_current == s_sound);
    }
    if (!visible) {
        return;
    }
    s_hold_page = false;   /* one-shot: >notifs may show a sub-page through the NEXT tick only */
    if (s_current == s_home) {
        tick_home();
    } else if (s_current == s_wifi) {
        tick_wifi();
    } else if (s_current == s_hatch) {
        tick_hatch();
    } else if (s_current == s_ble) {
        tick_ble();
    } else if (s_current == s_sound) {
        tick_sound();
    } else if (s_current == s_sleep) {
        tick_sleep();
    } else if (s_current == s_battery) {
        tick_battery();
    } else if (s_current == s_usage) {
        tick_usage();
    } else if (s_current == s_agents) {
        tick_agents();
    } else if (s_current == s_notif) {
        tick_notif();
    }
}

bool muse_settings_ui_in_subpage(void)
{
    return s_current != s_home;
}
