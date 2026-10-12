/* Host per-frame cost of a face renderer, steady state + crossfade. Not part of the
 * suite; used to check the blend stays in the render budget.
 *   cc -O2 -w -I components/muse -I components/muse/avatar  *      tools/muse/tbench.c components/muse/avatar/muse_pixel_golem.c -lm -o /tmp/tb
 *   /tmp/tb        # 20k frames, idle then a mood pair (cross-fade territory)
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "muse_pixel.h"
#include "muse_blend.h"

extern bool muse_pixel_react(const char *name);
extern void muse_pixel_render(const muse_pose_t *p);

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

int main(void)
{
    int n = 20000;
    muse_pixel_set_size(64);
    for (int i = 0; i < 500; i++) {
        muse_pose_t p = { .mode = MUSE_MODE_IDLE, .t = i * 0.04f, .mode_t = i * 0.04f };
        muse_pixel_render(&p);
    }
    double t0 = now_us();
    for (int i = 0; i < n; i++) {
        muse_pose_t p = { .mode = MUSE_MODE_IDLE, .t = 10.0f + i * 0.04f, .mode_t = 10.0f + i * 0.04f };
        muse_pixel_render(&p);
    }
    double t1 = now_us();
    for (int i = 0; i < n; i++) {
        muse_pose_t p = { .mode = MUSE_MODE_IDLE, .t = 10.0f + i * 0.04f, .mode_t = 10.0f + i * 0.04f };
        if (i == 100) {
            muse_pixel_react("smirk");
        }
        if (i == 400) {
            muse_pixel_react("angry");      /* cross-fade territory from here */
        }
        muse_pixel_render(&p);
    }
    double t2 = now_us();
    printf("idle %.2f us/frame | with moods+crossfade %.2f us/frame (n=%d)\n",
           (t1 - t0) / n, (t2 - t1) / n, n);
    return 0;
}
