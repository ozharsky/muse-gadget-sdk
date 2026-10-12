/*
 * pose_dump — apply each baked pose of a face at full weight through the REAL renderer and
 * save 6x PPM stills for contact sheets.
 *
 *   cc -O2 -w -I components/muse -DFACE_C='"muse_pixel.c"' tools/muse/pose_dump.c -lm -o /tmp/pose_dump
 *   /tmp/pose_dump OUTDIR [label0,label1,...]
 *
 * Includes the face's .c directly so its static pose machinery is reachable in this TU.
 * The POSE_COUNT / POSE_TABLE / SHODAN_POSES symbols come from the face's own headers.
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include FACE_C

#define S 6
static uint16_t buf[(MUSE_PX_W * S) * (MUSE_PX_H * S)];

static void save_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    int N = MUSE_PX_W * S;
    fprintf(f, "P6 %d %d 255\n", N, N);
    for (int i = 0; i < N * N; i++) {
        uint16_t c = buf[i];
        uint8_t rgb[3] = { (uint8_t)(((c >> 11) & 31) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63), (uint8_t)((c & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    const char *labels = argc > 2 ? argv[2] : NULL;
    int N = MUSE_PX_W * S;
    muse_pixel_set_size(N);
    mkdir(dir, 0755);

    muse_pose_t p = { .mode = MUSE_MODE_IDLE, .t = 20.0f, .mode_t = 5.0f, .level = 0, .happy = 0 };
    /* labels: comma-separated names for P_* order; default pose_%02d */
    char nm[64][48];
    int nl = 0;
    if (labels) {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s", labels);
        for (char *t = strtok(tmp, ","); t && nl < 64; t = strtok(NULL, ",")) {
            snprintf(nm[nl], sizeof nm[nl], "%s", t);
            nl++;
        }
    }
    for (int pi = 0; pi < POSE_COUNT; pi++) {
        /* a settled idle frame through the real renderer (palette built, light pass run) */
        for (float t = 18.8f; t <= 20.0f; t += 0.04f) {
            p.t = t;
            muse_pixel_render(&p);
        }
#ifdef SHODAN_STYLE
        pose_apply(pi, 1.0f);                   /* shodan: sparse delta with dithering */
#else
        const pose_t *pt = &POSE_TABLE[pi];     /* stone/golem: pos/val + eye zone */
        for (int k = 0; k < pt->n; k++) {
            s_fb[pt->pos[k]] = pt->val[k];
        }
        if (pt->zone) {
            for (int k = 0; k < EYE_ZONE_N; k++) {
                s_fb[EYE_ZONE_POS[k]] = pt->zone[k];
            }
        }
#endif
        muse_pixel_scale(buf, N, 0, N - 1, 0, N - 1);
        char path[512];
        if (pi < nl) {
            snprintf(path, sizeof path, "%s/%02d_%s.ppm", dir, pi, nm[pi]);
        } else {
            snprintf(path, sizeof path, "%s/%02d.ppm", dir, pi);
        }
        save_ppm(path);
        fprintf(stderr, "pose %2d saved %s\n", pi, path);
    }
    return 0;
}
