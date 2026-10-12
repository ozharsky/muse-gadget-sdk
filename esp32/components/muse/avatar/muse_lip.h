/* Lip openness from the live audio level: three states (closed / half / open) with
 * hysteresis bands and a ~100 ms minimum hold.
 *
 * Why not a raw threshold: a level hovering at a boundary re-decides every frame, and the
 * mouth chatters (the anti-flicker lesson from pixel talking-head projects). Here the entry
 * threshold for each state sits well above its exit threshold, and any change is gated on
 * 100 ms since the last one, so a syllable envelope reads as clean open/close beats.
 *
 * Pure math, no ESP deps -- host-testable (tools/muse/lip_test.c).
 *
 *   muse_lip_t s_lip;                             // one per face, reset when not speaking
 *   float mouth = muse_lip(&s_lip, level, dt);   // 0.0 / 0.55 / 1.0
 */
#pragma once

#define MUSE_LIP_HOLD_S 0.10f   /* minimum time a state is held (s) */

typedef struct {
    int state;                  /* 0 closed, 1 half, 2 open */
    float hold;                 /* seconds since the last state change */
} muse_lip_t;

/* Entry threshold of each upward step, exit of each downward step. The gap between
 * OPEN[i] and CLOSE[i] is the hysteresis band. */
#define MUSE_LIP_OPEN_1  0.18f
#define MUSE_LIP_OPEN_2  0.62f
#define MUSE_LIP_CLOSE_1 0.10f
#define MUSE_LIP_CLOSE_2 0.48f

static inline float muse_lip(muse_lip_t *l, float level, float dt)
{
    int s = l->state;
    if (s == 0) {
        if (level > MUSE_LIP_OPEN_2) {
            s = 2;                                    /* a loud start opens straight up */
        } else if (level > MUSE_LIP_OPEN_1) {
            s = 1;
        }
    } else if (s == 1) {
        if (level > MUSE_LIP_OPEN_2) {
            s = 2;
        } else if (level < MUSE_LIP_CLOSE_1) {
            s = 0;
        }
    } else {                                          /* s == 2 */
        if (level < MUSE_LIP_CLOSE_1) {
            s = 0;
        } else if (level < MUSE_LIP_CLOSE_2) {
            s = 1;
        }
    }
    l->hold += dt;
    if (s != l->state && l->hold >= MUSE_LIP_HOLD_S) {
        l->state = s;
        l->hold = 0;
    }
    return l->state == 2 ? 1.0f : (l->state == 1 ? 0.55f : 0.0f);
}
