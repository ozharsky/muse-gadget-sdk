/*
 * dump_anim — multi-animation frame sampler for preview sheets.
 *
 * Renders a face's STATES or MOOD reactions through its REAL renderer and writes
 * strided PPM samples (<dir>/<name>/<NNN>.ppm) for tools/muse/preview_sheets.py,
 * which assembles labelled contact sheets for vision QA.
 *
 *   cc -O2 -w -I components/muse tools/muse/dump_anim.c <face>.c -lm -o /tmp/dump_anim
 *   /tmp/dump_anim OUTDIR S TARGET state boot idle ...          (anim.c's envelopes)
 *   /tmp/dump_anim OUTDIR S TARGET mood smirk:2.8 scan:2.6 ...  (react_demo.c's envelope)
 *
 * TARGET is the approximate number of saved frames per animation (stride is derived
 * per animation, so long and short clips keep a comparable number of samples).
 * The state driver mirrors tools/muse/anim.c exactly; the mood driver mirrors
 * tools/muse/react_demo.c (warm up on idle, fire muse_pixel_react(name), record).
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

#define DT 0.04f        /* 40 ms, same as muse_ui */
#define WARMUP 1.5f     /* anim.c's warm-up, so state sheets match the GIF tool */

static int S = 4;
static uint16_t buf[(MUSE_PX_W * 4) * (MUSE_PX_H * 4)];

typedef struct {
    const char *name;
    muse_mode_t mode;
    float secs;
    float start_mode_t;     /* mode_t at the first recorded frame */
    bool talky;             /* drive `level` like live speech */
    bool pet;               /* play a happy reaction partway through */
} anim_t;

/* Same table as tools/muse/anim.c — keep the two in sync. */
static const anim_t ANIMS[] = {
    { "boot", MUSE_MODE_BOOT, 3.0f, 0 },
    { "idle", MUSE_MODE_IDLE, 6.0f, 5 },
    { "listening", MUSE_MODE_LISTENING, 4.0f, 0, .talky = true },
    { "thinking", MUSE_MODE_THINKING, 4.0f, 0 },
    { "speaking", MUSE_MODE_SPEAKING, 4.0f, 0, .talky = true },
    { "happy", MUSE_MODE_IDLE, 3.0f, 9, .pet = true },
    { "off", MUSE_MODE_OFF, 2.0f, 0 },
    { "error", MUSE_MODE_ERROR, 3.0f, 0 },
};

/* Bursty syllable-like envelope in 0..1 (anim.c). */
static float speech_level(float t)
{
    float syl = fabsf(sinf(t * 6.3f)) * (0.55f + 0.45f * sinf(t * 1.7f + 1.0f));
    float gap = sinf(t * 0.9f) > -0.6f ? 1.0f : 0.15f;
    float v = syl * gap;
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

/* Matches muse_state_happiness(): full for HAPPY_SECS - 0.4, then eases out (anim.c). */
static float happiness(float since_pet)
{
    float left = 1.6f - since_pet;
    if (since_pet < 0 || left <= 0) {
        return 0;
    }
    return left > 0.4f ? 1.0f : left / 0.4f;
}

static bool save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    int N = MUSE_PX_W * S;
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

static int frames_for(float secs)
{
    return (int)(secs / DT + 0.5f);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: dump_anim OUTDIR S TARGET state <name>...\n"
            "       dump_anim OUTDIR S TARGET mood  <name:dur>...\n");
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        usage();
        return 2;
    }
    const char *dir = argv[1];
    S = atoi(argv[2]);
    if (S < 1) {
        S = 1;
    }
    if (S > 4) {
        S = 4;
    }
    int target = atoi(argv[3]);
    if (target < 2) {
        target = 2;
    }
    const char *mode = argv[4];
    const int N = MUSE_PX_W * S;
    muse_pixel_set_size(N);
    mkdir(dir, 0755);      /* EEXIST for ".", ENOENT handled per animation */

    float base = 10.0f;
    int failed = 0;

    if (!strcmp(mode, "state")) {
        for (int a = 5; a < argc; a++) {
            const anim_t *an = NULL;
            for (size_t i = 0; i < sizeof(ANIMS) / sizeof(ANIMS[0]); i++) {
                if (!strcmp(ANIMS[i].name, argv[a])) {
                    an = &ANIMS[i];
                }
            }
            if (!an) {
                fprintf(stderr, "dump: no state '%s'\n", argv[a]);
                failed = 1;
                continue;
            }
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dir, an->name);
            mkdir(path, 0755);
            int frames = frames_for(an->secs);
            int warm = frames_for(WARMUP);
            int stride = frames / (target * 2);
            if (stride < 1) {
                stride = 1;
            }
            int saved = 0;
            for (int i = -warm; i < frames; i++) {
                float rt = i * DT;
                muse_pose_t p = {
                    .mode = an->mode,
                    .t = base + rt,
                    .mode_t = an->start_mode_t + rt,
                    .level = an->talky ? speech_level(rt) : 0,
                    .happy = an->pet ? happiness(rt - 0.5f) : 0,
                };
                if (p.mode_t < 0) {
                    p.mode_t = 0;
                }
                muse_pixel_render(&p);
                if (i >= 0 && i % stride == 0) {
                    muse_pixel_scale(buf, N, 0, N - 1, 0, N - 1);
                    snprintf(path, sizeof(path), "%s/%s/%03d.ppm", dir, an->name, i);
                    if (!save(path)) {
                        fprintf(stderr, "dump: cannot write %s\n", path);
                        failed = 1;
                        break;
                    }
                    saved++;
                }
            }
            fprintf(stderr, "dump: state %-12s %d saved\n", an->name, saved);
            base += 100.0f;
        }
    } else if (!strcmp(mode, "mood")) {
        for (int a = 5; a < argc; a++) {
            char nm[64];
            const char *colon = strchr(argv[a], ':');
            size_t nl = colon ? (size_t)(colon - argv[a]) : strlen(argv[a]);
            if (nl >= sizeof(nm)) {
                nl = sizeof(nm) - 1;
            }
            memcpy(nm, argv[a], nl);
            nm[nl] = 0;
            float dur = colon ? (float)atof(colon + 1) : 3.5f;

            char path[512];
            for (size_t i = 0; i < sizeof(nm) && nm[i]; i++) {
                if (nm[i] == '/') {
                    nm[i] = '_';
                }
            }
            snprintf(path, sizeof(path), "%s/%s", dir, nm);
            mkdir(path, 0755);
            int frames = frames_for(dur);
            int warm = frames_for(WARMUP);
            int stride = frames / (target * 2);
            if (stride < 1) {
                stride = 1;
            }
            bool fired = false;
            int saved = 0;
            for (int i = -warm; i < frames; i++) {
                float rt = i * DT;
                if (!fired && i >= 0) {
                    if (!muse_pixel_react(nm)) {
                        fprintf(stderr, "dump: face does not know '%s'\n", nm);
                        failed = 1;
                        break;
                    }
                    fired = true;
                }
                muse_pose_t p = {
                    .mode = MUSE_MODE_IDLE,
                    .t = base + rt,
                    .mode_t = 10.0f + rt,
                    .level = 0,
                    .happy = 0,
                };
                muse_pixel_render(&p);
                if (i >= 0 && i % stride == 0) {
                    muse_pixel_scale(buf, N, 0, N - 1, 0, N - 1);
                    snprintf(path, sizeof(path), "%s/%s/%03d.ppm", dir, nm, i);
                    if (!save(path)) {
                        fprintf(stderr, "dump: cannot write %s\n", path);
                        failed = 1;
                        break;
                    }
                    saved++;
                }
            }
            fprintf(stderr, "dump: mood  %-16s %d saved\n", nm, saved);
            base += 100.0f;
        }
    } else {
        usage();
        return 2;
    }
    if (failed) {
        fprintf(stderr, "dump: FAILED\n");
        return 1;
    }
    return 0;
}
