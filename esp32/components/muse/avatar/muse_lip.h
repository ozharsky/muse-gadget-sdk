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

/* Continuous openness for faces that cross-fade (the portraits): the voice's syllables, not its
 * loudness. Speech sits high in the level's range (about 0.5..0.9), so a fixed mapping holds the
 * mouth open; this tracks a decaying peak and a rising floor and opens between them, so every
 * syllable opens and every dip closes, loud voice or quiet. Feed it the raw per-chunk level.
 *
 *   muse_lip_agc_t s_agc;                            // one per face
 *   float open = muse_lip_agc(&s_agc, level, dt);   // 0..1, smoothed
 */
typedef struct {
    float peak, floor, open;
} muse_lip_agc_t;

static inline float muse_lip_agc(muse_lip_agc_t *a, float level, float dt)
{
    float target = 0;
    if (level > 0.01f) {
        a->peak = level > a->peak ? level : a->peak - dt * 0.25f;
        a->floor = level < a->floor ? level : a->floor + dt * 0.2f;
        a->floor = a->floor > a->peak ? a->peak : a->floor;
        float span = a->peak - a->floor;
        span = span < 0.15f ? 0.15f : span;
        target = (level - a->floor) / span;
        target = target < 0 ? 0 : target > 1 ? 1 : target;
    } else {
        a->peak = a->peak > dt * 0.3f ? a->peak - dt * 0.3f : 0;   /* silence: close, forget slowly */
    }
    float k = (target > a->open ? 25.0f : 14.0f) * dt;          /* opens faster than it closes */
    a->open += (target - a->open) * (k > 1 ? 1 : k);
    return a->open;
}
