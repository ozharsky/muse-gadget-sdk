/*
 * The Glance page: one swipe left of the face. A big clock, then a scrolling stack of rich
 * cards -- weather, printers, reminders, the morning briefing -- built from the relay's
 * /glance feed (muse_chat_session.cpp fetches it; this file only draws).
 *
 * Each card: a 16x16 pixel icon shown 3x (glance_icons.h), the kind's colour on the title and
 * the edge, a big value, a detail line, and a progress bar when the card has one. The stack
 * rebuilds only when the cards change (muse_glance_gen); the clock updates every tick.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "glance_icons.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "muse_chat.h"
#include "muse_glance_ui.h"

#define COLOR_BG 0x000000
#define COLOR_CARD 0x0a1410
#define COLOR_TEXT 0xe8fff0
#define COLOR_DIM 0x5fa070
#define COLOR_FAINT 0x2a4a34
#define CARD_W 300
#define ICON_PX 48

static lv_obj_t *s_list, *s_time, *s_ampm, *s_date, *s_empty, *s_updated;
static uint32_t s_last_poll_ms;
static int s_shown_age = -2;
static uint32_t s_built_gen = UINT32_MAX;
static int s_shown_min = -1;
static bool s_was_visible;
static volatile int s_bench_scroll = -1;   /* bench: scroll the stack (any task -> LVGL task) */

void muse_glance_ui_bench_scroll(int y)
{
    s_bench_scroll = y;
}
static lv_image_dsc_t s_icon_dsc[sizeof(GLANCE_ICONS) / sizeof(GLANCE_ICONS[0])];

static const struct {
    const char *kind, *icon, *label;
    uint32_t color;
} KINDS[] = {
    { "weather", "cloud", "WEATHER", 0x5cc8ff },
    { "print", "printer", "PRINTER", 0xff9a5c },
    { "reminder", "bell", "REMINDER", 0xffcc4d },
    { "briefing", "spark", "BRIEFING", 0x4dff3c },
    { "task", "agent", "HERMES", 0xc792ff },
    { "system", "chip", "SYSTEM", 0x4dd8c8 },
    { "alert", "alert", "NEEDS A LOOK", 0xff5c5c },
    { "note", "note", "NOTE", 0xb8ffd0 },
};

static int kind_index(const char *kind)
{
    for (size_t i = 0; i < sizeof(KINDS) / sizeof(KINDS[0]); i++) {
        if (!strcmp(kind, KINDS[i].kind)) {
            return (int)i;
        }
    }
    return (int)(sizeof(KINDS) / sizeof(KINDS[0])) - 1;   /* note */
}

static const lv_image_dsc_t *icon_for(const char *name)
{
    for (size_t i = 0; i < sizeof(GLANCE_ICONS) / sizeof(GLANCE_ICONS[0]); i++) {
        if (!strcmp(name, GLANCE_ICONS[i].name)) {
            lv_image_dsc_t *d = &s_icon_dsc[i];
            if (!d->data) {
                /* Blown up to ICON_PX once, here, in whole pixels: drawn at its own size it costs
                 * a copy, where a scaled image is LVGL's software transform on every redraw (a
                 * swipe redraws the page every frame). PSRAM, about 9 KB an icon actually used. */
                uint32_t *big = heap_caps_malloc(ICON_PX * ICON_PX * 4, MALLOC_CAP_SPIRAM);
                if (!big) {
                    return NULL;
                }
                const uint8_t *px = GLANCE_ICONS[i].px;   /* byte array: no word alignment to rely on */
                for (int y = 0; y < ICON_PX; y++) {
                    for (int x = 0; x < ICON_PX; x++) {
                        int at = (y * GLANCE_ICON_N / ICON_PX) * GLANCE_ICON_N + x * GLANCE_ICON_N / ICON_PX;
                        memcpy(&big[y * ICON_PX + x], px + at * 4, 4);
                    }
                }
                d->header.magic = LV_IMAGE_HEADER_MAGIC;
                d->header.cf = LV_COLOR_FORMAT_ARGB8888;
                d->header.w = ICON_PX;
                d->header.h = ICON_PX;
                d->header.stride = ICON_PX * 4;
                d->data_size = ICON_PX * ICON_PX * 4;
                d->data = (const uint8_t *)big;
            }
            return d;
        }
    }
    return NULL;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static void upper(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) {
        char c = src[i];
        dst[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    dst[i] = 0;
}

/* One rich card: [icon] TITLE / value / detail, and a bar for progress. */
static void add_card(const muse_glance_t *g)
{
    int k = kind_index(g->kind);
    uint32_t color = KINDS[k].color;

    lv_obj_t *card = lv_obj_create(s_list);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, CARD_W);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_column(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* The icon sits on a soft disc of the kind's colour. */
    lv_obj_t *disc = lv_obj_create(card);
    lv_obj_remove_style_all(disc);
    lv_obj_set_size(disc, ICON_PX + 8, ICON_PX + 8);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_10, 0);
    lv_obj_remove_flag(disc, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    const lv_image_dsc_t *ic = icon_for(g->icon[0] ? g->icon : KINDS[k].icon);
    if (!ic) {
        ic = icon_for(KINDS[k].icon);
    }
    if (ic) {
        lv_obj_t *img = lv_image_create(disc);
        lv_image_set_src(img, ic);
        lv_obj_center(img);
    }

    lv_obj_t *col = lv_obj_create(card);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    char t[24];
    upper(t, g->label[0] ? g->label : g->title[0] ? g->title : KINDS[k].label, sizeof(t));
    lv_obj_t *title = label(col, &lv_font_unscii_8, color, t);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    if (g->value[0]) {
        label(col, &lv_font_montserrat_28, COLOR_TEXT, g->value);
    }
    if (g->nstats) {   /* side-by-side readings: label, big value, a thin temperature-coloured bar */
        lv_obj_t *row = lv_obj_create(col);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_set_style_pad_top(row, 4, 0);      /* air between the card title and the readings */
        lv_obj_set_style_pad_bottom(row, 4, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        for (int i = 0; i < g->nstats; i++) {
            lv_obj_t *cell = lv_obj_create(row);
            lv_obj_remove_style_all(cell);
            lv_obj_set_flex_grow(cell, 1);
            lv_obj_set_height(cell, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(cell, 1, 0);
            lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            label(cell, &lv_font_unscii_8, COLOR_DIM, g->stats[i].l);
            label(cell, &lv_font_montserrat_28, COLOR_TEXT, g->stats[i].v);
            int p = g->stats[i].p;
            if (p >= 0) {
                lv_obj_t *bar = lv_bar_create(cell);
                lv_obj_set_size(bar, lv_pct(100), 5);
                lv_bar_set_range(bar, 0, 100);
                lv_bar_set_value(bar, p > 100 ? 100 : p, LV_ANIM_OFF);
                uint32_t bc = p >= 80 ? 0xff5c5c : p >= 60 ? 0xffb45c : 0x4dff3c;   /* cool / warm / hot */
                lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_FAINT), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
                lv_obj_set_style_bg_color(bar, lv_color_hex(bc), LV_PART_INDICATOR);
                lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
            }
        }
    }
    if (g->progress >= 0) {
        lv_obj_t *bar = lv_bar_create(col);
        lv_obj_set_size(bar, lv_pct(100), 8);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, g->progress, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_FAINT), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
        lv_obj_set_style_margin_top(bar, 4, 0);
        lv_obj_set_style_margin_bottom(bar, 4, 0);
    }
    const char *detail = g->detail[0] ? g->detail : g->text;
    if (detail[0] && strcmp(detail, g->value) != 0) {
        lv_obj_t *d = label(col, g->value[0] ? &lv_font_montserrat_14 : &lv_font_montserrat_16,
                            g->value[0] ? COLOR_DIM : COLOR_TEXT, detail);
        lv_obj_set_width(d, lv_pct(100));
        lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_WRAP);
    }
}

static void rebuild(void)
{
    /* Everything below the clock header goes; the header (the first child) stays. */
    while (lv_obj_get_child_count(s_list) > 1) {
        lv_obj_delete(lv_obj_get_child(s_list, 1));
    }
    int n = muse_glance_count();
    muse_glance_t g;
    for (int i = 0; i < n; i++) {
        if (muse_glance_get(i, &g)) {
            add_card(&g);
        }
    }
    s_empty = NULL;
    if (!n) {
        s_empty = label(s_list, &lv_font_montserrat_14, COLOR_DIM, "Nothing new right now.\nWeather, prints and reminders show up here.");
        lv_obj_set_width(s_empty, CARD_W - 40);
        lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
    }
    /* Room to scroll the last card up to the middle of the round glass. */
    lv_obj_t *pad = lv_obj_create(s_list);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, 10, 70);
    lv_obj_remove_flag(pad, LV_OBJ_FLAG_CLICKABLE);
}

void muse_glance_ui_build(lv_obj_t *tile)
{
    lv_obj_set_style_bg_color(tile, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);

    s_list = lv_obj_create(tile);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(s_list, 58, 0);
    lv_obj_set_style_pad_row(s_list, 12, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    /* The header: a big clock and the date. */
    lv_obj_t *head = lv_obj_create(s_list);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, CARD_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_bottom(head, 6, 0);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(head, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *row = lv_obj_create(head);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_time = label(row, &lv_font_montserrat_28, COLOR_TEXT, "--:--");
    s_ampm = label(row, &lv_font_unscii_8, COLOR_DIM, "");
    lv_obj_set_style_pad_bottom(s_ampm, 6, 0);
    s_date = label(head, &lv_font_unscii_8, 0x4dff3c, "GLANCE");
    s_updated = label(head, &lv_font_unscii_8, COLOR_FAINT + 0x203020, "");   /* how fresh the cards are */
    lv_obj_set_style_pad_top(s_updated, 3, 0);
    rebuild();
}

void muse_glance_ui_tick(bool visible, bool settled)
{
    if (!s_list) {
        return;
    }
    if (visible && !s_was_visible) {
        muse_glance_refresh();                    /* opened: fetch if stale */
        s_last_poll_ms = lv_tick_get();
        lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    } else if (visible && lv_tick_elaps(s_last_poll_ms) > 30000) {
        muse_glance_refresh();                    /* on screen: keep it live (every 30 s) */
        s_last_poll_ms = lv_tick_get();
    }
    if (visible) {   /* "updated just now / 3 min ago": whole minutes, so it changes rarely */
        int age = muse_glance_age_s();
        int key = age < 0 ? -1 : age / 60;
        if (key != s_shown_age) {
            s_shown_age = key;
            char b[32];
            if (age < 0) {
                strlcpy(b, "WAITING FOR CARDS", sizeof(b));
            } else if (age < 60) {
                strlcpy(b, "UPDATED JUST NOW", sizeof(b));
            } else {
                snprintf(b, sizeof(b), "UPDATED %d MIN AGO", age / 60);
            }
            lv_label_set_text(s_updated, b);
        }
    }
    s_was_visible = visible;
    if (s_bench_scroll >= 0) {
        lv_obj_scroll_to_y(s_list, s_bench_scroll, LV_ANIM_OFF);
        s_bench_scroll = -1;
    }
    /* New cards land once a swipe has: rebuilt mid-swipe, the page went half-empty and the
     * frame that rebuilt it held the swipe still. */
    uint32_t gen = muse_glance_gen();
    if (gen != s_built_gen && settled) {
        s_built_gen = gen;
        rebuild();
    }
    time_t tt = time(NULL);
    if (tt > 1700000000) {
        struct tm tm;
        localtime_r(&tt, &tm);
        int key = tm.tm_hour * 60 + tm.tm_min;
        if (key != s_shown_min) {
            s_shown_min = key;
            char b[24];
            snprintf(b, sizeof(b), "%d:%02d", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min);
            lv_label_set_text(s_time, b);
            lv_label_set_text(s_ampm, tm.tm_hour < 12 ? "AM" : "PM");
            strftime(b, sizeof(b), "%A %d %B", &tm);
            char u[24];
            upper(u, b, sizeof(u));
            lv_label_set_text(s_date, u);
        }
    }
}
