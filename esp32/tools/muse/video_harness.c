/* Host harness for the video player (muse_pixel_video.c): proves frame advance, the i4
 * unpack, the loop wrap and the one-shot reaction stop, and dumps PPMs to eyeball.
 *
 *   cc -O2 -I <esp32/components/muse> -I <esp32/components/muse/avatar> \
 *      video_harness.c <.../avatar/muse_pixel_video.c> -o vh && ./vh outdir
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "muse_pixel.h"
#include "muse_pixel_video.h"

#define S 6
#define N (MUSE_PX_W * S)
#define STEP_US 83334              /* the clip's frame time (12 fps), rounded UP so 48
                                    * steps is a full 4 s -- 83333 lands 16 us short and
                                    * trips the loop-wrap check on the player's side of
                                    * the boundary */

static uint16_t buf[N * N];
static int64_t g_us;

int64_t esp_timer_get_time(void)
{
    return g_us;
}

static uint32_t hash_buf(void)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < N * N; i++) {
        h ^= buf[i];
        h *= 16777619u;
    }
    return h;
}

static void save(const char *path, int frame_no)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6 %d %d 255\n", N, N);
    for (int i = 0; i < N * N; i++) {
        uint16_t c = buf[i];
        uint8_t rgb[3] = { (uint8_t)(((c >> 11) & 31) * 255 / 31),
                           (uint8_t)(((c >> 5) & 63) * 255 / 63),
                           (uint8_t)((c & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    (void)frame_no;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    muse_pixel_video_set_size(N);
    muse_pixel_video_render();                 /* idle: nothing pending, must be a no-op */
    if (muse_pixel_video_active()) {
        printf("FAIL: active before any request\n");
        return 1;
    }

    if (!muse_pixel_video("construct")) {
        printf("FAIL: clip not found\n");
        return 1;
    }
    g_us = 1000000;                            /* start "now" */
    muse_pixel_video_render();                 /* applies the request */

    uint32_t h[97];
    for (int i = 0; i <= 96; i++) {            /* 97 frames = 2 loops of 48 + 1 */
        muse_pixel_video_render();
        muse_pixel_video_scale(buf, N, 0, N - 1, 0, N - 1);
        h[i] = hash_buf();
        if (i % 12 == 0) {
            char p[512];
            snprintf(p, sizeof(p), "%s/v%02d.ppm", dir, i);
            save(p, i);
        }
        g_us += STEP_US;
    }
    if (!muse_pixel_video_active()) {
        printf("FAIL: infinite play stopped on its own\n");
        return 1;
    }
    int same_next = 0, wrap_ok = 0, distinct = 0;
    for (int i = 1; i <= 96; i++) {
        if (h[i] == h[i - 1]) {
            same_next++;
        }
    }
    if (h[0] == h[48] && h[1] == h[49]) {
        wrap_ok = 1;
    }
    for (int i = 0; i < 12; i++) {
        if (h[i] != h[i + 1]) {
            distinct++;
        }
    }
    printf("frames 0..96: distinct-consecutive(first 12)=%d/12 same-consecutive=%d loop-wrap(h0==h48,h1==h49)=%s\n",
           distinct, same_next, wrap_ok ? "yes" : "NO");

    /* one-shot reaction: 1 loop then back to the face */
    if (!muse_pixel_video_play("construct", 1)) {
        printf("FAIL: play() rejected the clip\n");
        return 1;
    }
    g_us += 1000000;                           /* a gap: apply happens on next render */
    muse_pixel_video_render();
    if (!muse_pixel_video_active()) {
        printf("FAIL: reaction did not start\n");
        return 1;
    }
    int stopped_at = -1;
    for (int i = 0; i < 100; i++) {
        muse_pixel_video_render();
        g_us += STEP_US;
        if (!muse_pixel_video_active()) {
            stopped_at = i;
            break;
        }
    }
    printf("one-shot reaction stopped after %d steps (expect ~47)\n", stopped_at);

    int ok = wrap_ok && distinct >= 10 && stopped_at >= 44 && stopped_at <= 50;
    printf(ok ? "HARNESS OK\n" : "HARNESS FAIL\n");
    return ok ? 0 : 1;
}
