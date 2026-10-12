/*
 * Reaction-transition dump: plays idle at the device frame rate, fires one
 * [react:<name>] partway through (the exact call the relay path makes), and writes
 * every frame as a PPM (<dir>/NNN.ppm). anim.c drives the base modes only; this
 * driver exists for how a reaction CHANGES the face -- the T3.1 rig evidence: a cut
 * lands as one huge per-frame delta, an eased transition spreads over a few frames.
 *
 * A SECOND reaction can fire later (the mood-pair case, for the cross-mood blend):
 *
 *   cc -O2 -I components/muse tools/muse/react_demo.c <face renderer>.c -lm -o /tmp/rdemo
 *   /tmp/rdemo OUTDIR smirk [fire_s] [total_s] [name2 [fire2_s]]
 */
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "muse_pixel.h"

extern bool muse_pixel_react(const char *name);

#define S 5
#define N (MUSE_PX_W * S)
#define DT 0.04f        /* 40 ms, same as muse_ui */
#define WARMUP 1.5f     /* idle frames before the recording starts */

static uint16_t buf[N * N];

static bool save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    fprintf(f, "P6 %d %d 255\n", N, N);
    for (int i = 0; i < N * N; i++) {
        uint16_t c = buf[i];
        uint8_t rgb[3] = { (uint8_t)(((c >> 11) & 31) * 255 / 31),
                           (uint8_t)(((c >> 5) & 63) * 255 / 63),
                           (uint8_t)((c & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f) == 0;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    const char *name = argc > 2 ? argv[2] : "smirk";
    const char *name2 = argc > 5 ? argv[5] : NULL;
    float fire = argc > 3 ? (float)atof(argv[3]) : 1.5f;
    float fire2 = argc > 6 ? (float)atof(argv[6]) : 0.0f;
    float total = argc > 4 ? (float)atof(argv[4]) : fire + 4.5f;
    if (name2 && fire2 <= 0.0f) {
        fire2 = fire + 1.5f;
        if (total < fire2 + 1.5f) {
            total = fire2 + 1.5f;
        }
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "react_demo: cannot create %s\n", dir);
        return 1;
    }
    muse_pixel_set_size(N);
    const float base = 10.0f;
    int frames = (int)(total / DT + 0.5f);
    int warm = (int)(WARMUP / DT + 0.5f);
    bool fired = false, fired2 = false;
    int wrote = 0;
    for (int i = -warm; i < frames; i++) {
        float rt = i * DT;
        if (!fired && rt >= fire) {
            if (!muse_pixel_react(name)) {
                fprintf(stderr, "react_demo: the face does not know '%s'\n", name);
                return 1;
            }
            fprintf(stderr, "react_demo: fired %s at t=%.2f (%d recorded frames)\n",
                    name, rt, wrote);
            fired = true;
        }
        if (name2 && !fired2 && rt >= fire2) {
            if (!muse_pixel_react(name2)) {
                fprintf(stderr, "react_demo: the face does not know '%s'\n", name2);
                return 1;
            }
            fprintf(stderr, "react_demo: fired %s at t=%.2f (%d recorded frames)\n",
                    name2, rt, wrote);
            fired2 = true;
        }
        muse_pose_t p = {
            .mode = MUSE_MODE_IDLE,
            .t = base + (float)i * DT,
            .mode_t = 10.0f + rt,
            .level = 0,
        };
        muse_pixel_render(&p);
        if (i >= 0) {
            muse_pixel_scale(buf, N, 0, N - 1, 0, N - 1);
            char path[512];
            snprintf(path, sizeof(path), "%s/%03d.ppm", dir, i);
            if (!save(path)) {
                fprintf(stderr, "react_demo: cannot write %s\n", path);
                return 1;
            }
            wrote++;
        }
    }
    if (!fired) {
        fprintf(stderr, "react_demo: %s never fired (fire %.2f > total %.2f)\n", name, fire, total);
        return 1;
    }
    if (name2 && !fired2) {
        fprintf(stderr, "react_demo: %s never fired (fire2 %.2f > total %.2f)\n", name2, fire2, total);
        return 1;
    }
    fprintf(stderr, "react_demo: %d frames\n", wrote);
    return 0;
}
