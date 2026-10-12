/* Host-only probe: includes the face renderer source directly so it can read the
 * file-static scheduler state and print the TRUE runtime schedule of idle events.
 * Mirrors idle_demo's clock exactly (first render at t = 10 + (-50)*0.04).
 *
 *   cc -O2 -w -I components/muse -I components/muse/avatar probe_idle.c -lm -o probe
 *   ./probe <secs>
 * (probe_idle.c #includes the face file named by -DFACE_C; default golem)
 */
#include <stdio.h>
#include <stdlib.h>

#ifndef FACE_C
#define FACE_C "muse_pixel_golem.c"
#endif
#include FACE_C

int main(int argc, char **argv)
{
    float secs = argc > 1 ? (float)atof(argv[1]) : 160.0f;
    float from = argc > 2 ? (float)atof(argv[2]) : 0.0f;
    float to = argc > 3 ? (float)atof(argv[3]) : secs;
    muse_pixel_set_size(256);
    int i0 = (int)((from - 2.0f) / 0.04f + 0.5f);
    int i1 = (int)(to / 0.04f + 0.5f);
    int prev_ev = 0;
    float prev_bat = -100.0f;
    int blink_on = 0;
    for (int i = i0; i < i1; i++) {
        float rt = (float)i * 0.04f;
        muse_pose_t p = { .mode = MUSE_MODE_IDLE, .t = 10.0f + rt, .mode_t = 10.0f + rt, .level = 0 };
        muse_pixel_render(&p);
        if (s_idle.ev != prev_ev) {
            if (s_idle.ev != MUSE_IDLE_EV_NONE && rt >= from) {
                const char *nm = s_idle.ev == MUSE_IDLE_EV_WIDEN ? "widen"
                               : s_idle.ev == MUSE_IDLE_EV_SQUINT ? "squint"
                               : s_idle.ev == MUSE_IDLE_EV_SMILE ? "smile"
                               : s_idle.ev == MUSE_IDLE_EV_WINK ? "wink" : "ACCENT";
                printf("ev %-7s @ rt %.2f (dur %.2f)\n", nm, rt, s_idle.ev_dur);
            }
            prev_ev = s_idle.ev;
        }
        float sh = muse_idle_shut(&s_idle, 10.0f + rt);
        if (!blink_on && sh > 0.02f) {
            blink_on = 1;
            if (rt >= from) {
                printf("blink         @ rt %.2f (dur %.3f)\n", rt, s_idle.blink_dur);
            }
        } else if (blink_on && sh <= 0.02f) {
            blink_on = 0;
        }
        (void)prev_bat;
    }
    return 0;
}
