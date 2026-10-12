// Bench/relay video player: plays compiled-in indexed 64x64 clips (clips/*.h, listed by
// bench_videos.h from gen_video_registry.py) through the avatar canvas. Two formats:
// u8 palette indices, or packed 4-bit (two pixels/byte, even-x in the LOW nibble).
// This is the playback half of the video_to_gadget.py v2 pipeline. It also backs the
// relay's themed reaction clips: any [react:<name>] a face does not implement is looked
// up here and played full-face for a few loops (muse_pixel_switch.c).
//
// Ownership: the console/session task only sets a request (it never touches the frame
// buffer or the palette); the LVGL task applies it at the top of muse_pixel_video_render.
// All player state moves on one task -- same law as the speaker and the display.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__has_include) && __has_include("esp_timer.h")
#include "esp_timer.h"
#else
extern int64_t esp_timer_get_time(void);   /* host harness provides the clock */
#endif
#include "muse_pixel.h"
#include "muse_pixel_video.h"
#include "bench_videos.h"

#if defined(__has_include) && __has_include("esp_attr.h")
#include "esp_attr.h"
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR   /* host preview builds: ordinary memory */
#endif

#define W MUSE_PX_W
#define H MUSE_PX_H
#define FMT_U8 1
#define FMT_I4 2

typedef struct bench_video_t bench_video_t;

/* Requests from any task; applied in render (the LVGL task). */
static volatile int s_req;              /* 1 = play s_req_clip, 2 = stop */
static const bench_video_t *s_req_clip;
static int s_req_loops;

/* Player state (LVGL task only). */
static const bench_video_t *s_clip;     /* NULL = faces own the canvas */
static int s_loops_left;                /* -1 = play forever */
static int64_t s_start_us;
static int s_frame_last;
static int s_decodes;                   /* clip frames decoded (bench counter) */

static uint16_t s_pal[32];
static uint16_t s_pal_dim[32];
static int s_pal_n;

static EXT_RAM_BSS_ATTR uint8_t s_fb[W * H];     /* current frame, palette indices */
static EXT_RAM_BSS_ATTR uint8_t s_map[512];      /* the faces' grid map scheme */
static int s_size;

static uint16_t dim565(uint16_t c)
{
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r = (int)((float)r * 0.72f + 0.5f);
    g = (int)((float)g * 0.72f + 0.5f);
    b = (int)((float)b * 0.72f + 0.5f);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static const bench_video_t *find(const char *name)
{
    for (int i = 0; i < BENCH_VIDEO_COUNT; i++) {
        if (strcmp(name, BENCH_VIDEOS[i].name) == 0) {
            return &BENCH_VIDEOS[i];
        }
    }
    return NULL;
}

bool muse_pixel_video(const char *name)
{
    if (!name) {
        return false;
    }
    if (!strcmp(name, "stop")) {
        s_req = 2;
        return true;
    }
    const bench_video_t *v = find(name);
    if (!v) {
        return false;
    }
    s_req_clip = v;
    s_req_loops = -1;
    s_req = 1;
    return true;
}

bool muse_pixel_video_play(const char *name, int loops)
{
    if (!name || loops == 0) {
        return false;
    }
    const bench_video_t *v = find(name);
    if (!v) {
        return false;
    }
    s_req_clip = v;
    s_req_loops = loops;
    s_req = 1;
    return true;
}

bool muse_pixel_video_active(void)
{
    return s_clip != NULL;
}

static void apply_pending(void)
{
    int r = s_req;
    if (!r) {
        return;
    }
    s_req = 0;
    if (r == 2) {
        s_clip = NULL;
        return;
    }
    const bench_video_t *v = s_req_clip;
    if (!v) {
        return;
    }
    s_clip = v;
    s_loops_left = s_req_loops;
    s_start_us = esp_timer_get_time();
    s_frame_last = -1;
    s_pal_n = v->npal < 32 ? v->npal : 32;
    for (int i = 0; i < s_pal_n; i++) {
        s_pal[i] = v->pal[i];
        s_pal_dim[i] = dim565(v->pal[i]);
    }
}

static void decode_frame(const bench_video_t *v, int fi)
{
    s_decodes++;
    if (v->fmt == FMT_I4) {
        const uint8_t *src = v->frames + (size_t)fi * (W / 2) * H;
        for (int y = 0; y < H; y++) {
            const uint8_t *row = src + y * (W / 2);
            uint8_t *out = &s_fb[y * W];
            for (int x = 0; x < W / 2; x++) {
                uint8_t b = row[x];
                uint8_t lo = b & 0x0F, hi = (uint8_t)((b >> 4) & 0x0F);
                out[x * 2] = lo < (uint8_t)s_pal_n ? lo : 0;
                out[x * 2 + 1] = hi < (uint8_t)s_pal_n ? hi : 0;
            }
        }
    } else {
        const uint8_t *src = v->frames + (size_t)fi * W * H;
        for (int i = 0; i < W * H; i++) {
            uint8_t v2 = src[i];
            s_fb[i] = v2 < (uint8_t)s_pal_n ? v2 : 0;   /* guard: index past the palette */
        }
    }
}

void muse_pixel_video_render(void)
{
    apply_pending();
    const bench_video_t *v = s_clip;
    if (!v) {
        return;
    }
    int64_t us = esp_timer_get_time() - s_start_us;
    /* Frame clock from MICROSECONDS, not truncated ms: ms-truncation biases every frame
     * late by up to 1 ms which, at 12 fps, turns into an occasional repeat or skip
     * (the harness caught frame 2 skipped at exact step boundaries). */
    int64_t fidx = us * (int64_t)v->fps / 1000000;
    if (s_loops_left >= 0 && fidx >= (int64_t)v->count * s_loops_left) {
        s_clip = NULL;                                  /* a reaction clip finished */
        return;
    }
    int fi = (int)(fidx % v->count);
    if (fi != s_frame_last) {
        s_frame_last = fi;
        decode_frame(v, fi);
    }
}

int muse_pixel_video_decodes(bool reset)
{
    int v = s_decodes;
    if (reset) {
        s_decodes = 0;
    }
    return v;
}

void muse_pixel_video_set_size(int px_size)
{
    s_size = px_size < 512 ? px_size : 512;
    bool grid = s_size >= 3 * W;
    for (int i = 0; i < s_size; i++) {
        int cell = i * W / s_size;
        bool edge = grid && (i + 1) * W / s_size != cell;
        s_map[i] = (uint8_t)(cell | (edge ? 0x80 : 0));
    }
}

/* The faces' scale scheme, over the clip palette: grid cells darken their last
 * destination row/column so the chunky pixels keep the same seam look as the avatar. */
void muse_pixel_video_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    const uint8_t *xmap = &s_map[x0];
    const uint16_t *prev = NULL;
    uint8_t prev_m = 0;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        uint8_t m = s_map[y];
        if (prev && m == prev_m) {
            memcpy(dst, prev, n * sizeof(uint16_t));
            continue;
        }
        const uint8_t *row = &s_fb[(m & 0x7f) * W];
        if (m & 0x80) {
            for (int i = 0; i < n; i++) {
                dst[i] = s_pal_dim[row[xmap[i] & 0x7f]];
            }
        } else {
            for (int i = 0; i < n; i++) {
                uint8_t xm = xmap[i];
                uint8_t c = row[xm & 0x7f];
                dst[i] = xm & 0x80 ? s_pal_dim[c] : s_pal[c];
            }
        }
        prev = dst;
        prev_m = m;
    }
}
