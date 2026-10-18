/* Per-channel easing for the face expression channels (the T3.1 rig core).
 *
 * The faces compute TARGETS every frame (mode / reaction / happy logic writes plain
 * values); the rig eases the visible value toward the target with one exponential
 * step, so an expression change reads as motion instead of a cut:
 *
 *     current += (target - current) * (1 - exp(-dt / tau))
 *
 * tau is the time constant in seconds (63% of the gap closed). Eye channels are fast
 * enough to stay snappy (a wink still lands within a frame or two); glow channels are
 * slower, because light doesn't snap. Faces own their channels and which of them are
 * live; this header owns only the smoothing law.
 *
 *   muse_rig_t s_rig;                                    // one per face
 *   muse_rig_t tgt = { .open = open, .bright = bright };  // per frame, the targets
 *   if (!inited) { s_rig = tgt; inited = true; }           // first frame: settled
 *   muse_rig_step(&s_rig, &tgt, dt);
 *
 * dt <= 0 freezes (no time passed); tau <= 0 applies the target immediately (a
 * deliberate hard cut). Pure math, no ESP deps -- host-tested (tools/muse/rig_test.c,
 * tests/test_muse_rig.py).
 */
#pragma once

#include <math.h>

typedef struct {
    float open;    /* eyelid openness: 0 shut .. 1 open (some faces go a little past 1) */
    float widen;   /* wide-eye push, 0..1 */
    float bright;  /* glow / light energy, 0..1 */
    float mouth;   /* mouth openness / presence weight, 0..1 */
    float bloom;   /* eye-bloom flare, 0..1 */
} muse_rig_t;

static inline float muse_ease(float cur, float tgt, float dt, float tau)
{
    if (tau <= 0.0f) {
        return tgt;
    }
    if (dt <= 0.0f) {
        return cur;
    }
    float k = 1.0f - expf(-dt / tau);
    if (k > 1.0f) {
        k = 1.0f;
    }
    return cur + (tgt - cur) * k;
}

/* One step for every channel toward `tgt`. Per-channel taus: lid and mouth 60-90 ms
 * (a syllable beat); glow and bloom 120-160 ms (light eases). */
static inline void muse_rig_step(muse_rig_t *r, const muse_rig_t *tgt, float dt)
{
    r->open   = muse_ease(r->open,   tgt->open,   dt, 0.07f);
    r->widen  = muse_ease(r->widen,  tgt->widen,  dt, 0.09f);
    r->bright = muse_ease(r->bright, tgt->bright, dt, 0.16f);
    r->mouth  = muse_ease(r->mouth,  tgt->mouth,  dt, 0.06f);
    r->bloom  = muse_ease(r->bloom,  tgt->bloom,  dt, 0.12f);
}
