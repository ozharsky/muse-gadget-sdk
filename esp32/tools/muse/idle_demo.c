/* Idle-timeline dump (T3.2 evidence): renders pure IDLE through the real renderer
 * at the device frame rate for N seconds and writes every frame as a PPM
 * (<dir>/NNN.ppm), with the host render cost on stderr. anim.c drives every mode
 * in short bursts; this driver sits on the resting face long enough for the idle
 * scheduler's rhythm (blinks, doubles, saccades, micro beats, breathing) to show,
 * and tools/muse/idle_timeline.py measures it from the frames.
 *
 *   cc -O2 -I components/muse tools/muse/idle_demo.c <face renderer>.c -lm -o /tmp/idle_demo
 *   /tmp/idle_demo OUTDIR [secs] [idle|listening|thinking|speaking] [start_s]
 */
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "muse_pixel.h"

#define S 4
#define N (MUSE_PX_W * S)
#define DT 0.04f        /* 40 ms, same as muse_ui */
#define WARMUP 2.0f

static uint16_t buf[N * N];

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

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

/* Bursty syllable-like envelope in 0..1 (same shape anim.c feeds the talky modes). */
static float speech_level(float t)
{
    float syl = fabsf(sinf(t * 6.3f)) * (0.55f + 0.45f * sinf(t * 1.7f + 1.0f));
    float gap = sinf(t * 0.9f) > -0.6f ? 1.0f : 0.15f;
    float v = syl * gap;
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    float secs = argc > 2 ? (float)atof(argv[2]) : 30.0f;
    const char *mode_name = argc > 3 ? argv[3] : "idle";
    float start = argc > 4 ? (float)atof(argv[4]) : 0.0f;   /* rt seconds to begin the capture at */

    muse_mode_t mode = MUSE_MODE_IDLE;
    bool talky = false;
    if (strcmp(mode_name, "listening") == 0) {
        mode = MUSE_MODE_LISTENING;
        talky = true;
    } else if (strcmp(mode_name, "thinking") == 0) {
        mode = MUSE_MODE_THINKING;
    } else if (strcmp(mode_name, "speaking") == 0) {
        mode = MUSE_MODE_SPEAKING;
        talky = true;
    }

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "idle_demo: cannot create %s\n", dir);
        return 1;
    }
    muse_pixel_set_size(N);
    const float base = 10.0f;
    int frames = (int)(secs / DT + 0.5f);
    int warm = (int)(WARMUP / DT + 0.5f);
    int first = (int)(start / DT + 0.5f);   /* capture begins at this rt second */
    int wrote = 0;
    double render_us = 0, scale_us = 0;
    for (int i = -warm; i < first + frames; i++) {
        float rt = (float)i * DT;
        muse_pose_t p = {
            .mode = mode,
            .t = base + rt,
            .mode_t = 10.0f + rt,
            .level = talky ? speech_level(rt) : 0,
        };
        double t0 = now_us();
        muse_pixel_render(&p);
        double t1 = now_us();
        if (i >= first) {
            muse_pixel_scale(buf, N, 0, N - 1, 0, N - 1);
            char path[512];
            snprintf(path, sizeof(path), "%s/%03d.ppm", dir, i - first);
            if (!save(path)) {
                fprintf(stderr, "idle_demo: cannot write %s\n", path);
                return 1;
            }
            wrote++;
        }
        double t2 = now_us();
        render_us += t1 - t0;
        scale_us += t2 - t1;
    }
    fprintf(stderr, "idle_demo: %-9s %d frames  render %.1f us/frame  scale+save %.1f us/frame\n",
            mode_name, wrote, render_us / (frames + warm), scale_us / (frames + warm));
    printf("idle_demo:%s %d %.1f %.1f\n", mode_name, wrote,
           render_us / (frames + warm), scale_us / (frames + warm));
    return 0;
}
