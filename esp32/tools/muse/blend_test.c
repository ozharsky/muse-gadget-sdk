/* muse_blend.h's invariants, host-side.
 *
 *   cc -O2 -Wall -I components/muse/avatar tools/muse/blend_test.c -lm -o /tmp/blend_test
 *
 * What is pinned here:
 *   1. a LONE mood renders exactly as before -- its blend weight equals its own
 *      mood-clock ramp (rk) through the whole life (the single-fire parity the
 *      blend must not disturb);
 *   2. the cross-fade: when B arrives over A, A starts handing over the very same
 *      frame (its render weight moves < 2%), falls to zero within MUSE_BLEND_FALL,
 *      and B rises to 1; the acting target takes no first-frame jump;
 *   3. the fall eases in (the first frame after hand-over barely moves) and the
 *      whole hand-over completes in FALL;
 *   4. a repeat of the same mood restarts its ramp without dipping the weight;
 *   5. eviction keeps at most MUSE_BLEND_MAX slots (the weakest goes);
 *   6. top()/busy() disagree correctly across owner -> expired -> gone;
 *   7. two identical runs land in identical states (determinism).
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "muse_blend.h"

static float blend_scalar(const muse_blend_t *b, float v_a, float v_b)
{
    /* the faces' target law for a scalar channel: base + sum(w_i/S)(v_i - base) */
    float wsum = muse_blend_sum(b);
    float S = wsum > 1.0f ? wsum : 1.0f;
    float acc = 0.0f;
    for (int i = 0; i < b->n; i++) {
        acc += b->s[i].w * (b->s[i].id == 1 ? v_a : v_b);
    }
    return acc / S;
}

static void run(muse_blend_t *b, float dt, int n, float *ws, float *rs)
{
    for (int i = 0; i < n; i++) {
        muse_blend_step(b, dt);
        ws[i] = b->n > 0 ? b->s[0].w : 0.0f;
        rs[i] = b->n > 0 ? muse_blend_rend(&b->s[0]) : 0.0f;
    }
}

int main(void)
{
    /* 1. lone mood: the drawn weight is exactly the old rk through the whole life
     *    (single parity), the acting weight holds at 1 until the life ends, and the
     *    mood leaves on its own. */
    muse_blend_t b;
    muse_blend_init(&b);
    assert(b.n == 0 && b.req == -1);
    muse_blend_fire(&b, 7, 1.0f);
    float worst_parity = 0.0f, worst_hold = 0.0f;
    int seen = 0;
    for (int i = 0; i < 80; i++) {
        muse_blend_step(&b, 0.04f);
        if (b.n == 1) {
            float rk = muse_blend_rk(&b.s[0]);
            float rend = muse_blend_rend(&b.s[0]);
            float d = fabsf(rend - rk);
            worst_parity = d > worst_parity ? d : worst_parity;
            if (b.s[0].own) {
                float expect = b.s[0].age / MUSE_BLEND_RISE;
                expect = expect > 1.0f ? 1.0f : expect;
                float dw = fabsf(b.s[0].w - expect);
                worst_hold = dw > worst_hold ? dw : worst_hold;
            }
            seen++;
        }
    }
    assert(seen >= 30 && seen <= 36);           /* 1.0 s life + 0.4 s fall at dt 0.04 */
    assert(worst_parity < 1e-6f);                /* single parity, every frame */
    assert(worst_hold < 1e-5f);                  /* the owner holds at full weight */
    assert(b.n == 0);                            /* gone after life + fall */

    /* 2. cross-fade A -> B: no first-frame jump in the drawn weight, B lands at 1,
     *    A is gone within FALL, the acting target moves smoothly. */
    muse_blend_init(&b);
    muse_blend_fire(&b, 1, 3.0f);
    for (int i = 0; i < 25; i++) {
        muse_blend_step(&b, 0.04f);              /* run A to its stride */
    }
    assert(b.n == 1 && b.s[0].id == 1 && b.s[0].w > 0.99f);
    float target_before = blend_scalar(&b, 0.0f, 1.0f);
    assert(target_before < 1e-6f);               /* A alone: target = A = 0 */
    muse_blend_fire(&b, 2, 2.8f);
    muse_blend_step(&b, 0.04f);                  /* the hand-over frame */
    float a_rend_before = 1.0f;
    assert(b.n == 2);
    float a_rend_now = 0.0f;
    for (int i = 0; i < b.n; i++) {
        if (b.s[i].id == 1) {
            a_rend_now = muse_blend_rend(&b.s[i]);
        }
    }
    assert(fabsf(a_rend_now - a_rend_before) < 0.02f);   /* A barely moved on frame 1 */
    float target_after = blend_scalar(&b, 0.0f, 1.0f);
    assert(target_after < 0.30f);                /* the acting target takes a small step, not a cut */
    float prev = target_after, max_step = 0.0f, max_step_f = 0.0f;
    int frames_to_a_gone = 0, frames_to_b_full = 0;
    for (int i = 0; i < 30; i++) {
        muse_blend_step(&b, 0.04f);
        float tgt = blend_scalar(&b, 0.0f, 1.0f);
        float st = fabsf(tgt - prev);
        if (st > max_step) max_step = st;
        prev = tgt;
        bool a_gone = true;
        for (int j = 0; j < b.n; j++) {
            if (b.s[j].id == 1) a_gone = false;
        }
        if (!a_gone) frames_to_a_gone++;
        if (b.n == 1 && b.s[0].id == 2 && b.s[0].w > 0.999f) {
            frames_to_b_full++;
        }
        max_step_f = max_step_f > st ? max_step_f : st;
    }
    assert(max_step < 0.20f);                    /* no single frame carries the hand-over */
    assert(frames_to_a_gone <= (int)(MUSE_BLEND_FALL / 0.04f) + 1);  /* gone within FALL */
    assert(frames_to_b_full >= 2);              /* B finished rising */
    assert(b.n == 1 && b.s[0].id == 2 && b.s[0].w > 0.999f);         /* only B is left */

    /* 3. the fall eases in: consecutive fall deltas start tiny and grow. */
    muse_blend_init(&b);
    muse_blend_fire(&b, 1, 5.0f);
    for (int i = 0; i < 25; i++) {
        muse_blend_step(&b, 0.04f);
    }
    muse_blend_fire(&b, 2, 5.0f);
    float w_prev = 1.0f, d1 = -1.0f, d2 = -1.0f;
    muse_blend_step(&b, 0.04f);
    for (int i = 0; i < b.n; i++) {
        if (b.s[i].id == 1) {
            d1 = w_prev - b.s[i].w;             /* the very first fall step */
        }
    }
    muse_blend_step(&b, 0.04f);
    for (int i = 0; i < b.n; i++) {
        if (b.s[i].id == 1) {
            /* second step: from w0*(1-0.01) to w0*(1-0.04) */
            d2 = (1.0f * (1.0f - 0.01f)) - b.s[i].w;
        }
    }
    assert(d1 > 0.0f && d1 < 0.02f);             /* barely moves on frame one */
    assert(d2 > d1);                             /* and accelerates */

    /* 4. a repeat of the SAME mood: one slot, no weight dip, age restarted. */
    muse_blend_init(&b);
    muse_blend_fire(&b, 5, 3.0f);
    for (int i = 0; i < 25; i++) {
        muse_blend_step(&b, 0.04f);
    }
    float w_before = b.s[0].w;
    muse_blend_fire(&b, 5, 3.0f);
    muse_blend_step(&b, 0.04f);
    assert(b.n == 1 && b.s[0].id == 5);
    assert(fabsf(b.s[0].w - w_before) < 1e-6f); /* acting weight held: no dip */
    assert(b.s[0].age < 0.05f);                 /* the mood clock restarted */

    /* 5. eviction: fire four moods quickly; only MUSE_BLEND_MAX stay, weakest out. */
    muse_blend_init(&b);
    for (int id = 1; id <= 4; id++) {
        muse_blend_fire(&b, id, 5.0f);
        muse_blend_step(&b, 0.04f);
    }
    assert(b.n == MUSE_BLEND_MAX);
    bool has1 = false;
    for (int i = 0; i < b.n; i++) {
        if (b.s[i].id == 1) has1 = true;
    }
    assert(!has1);                              /* the oldest, weakest, was evicted */

    /* 6. top()/busy(): the owner while it lives, then the strongest survivor, then -1. */
    muse_blend_init(&b);
    assert(muse_blend_top(&b) < 0 && !muse_blend_busy(&b));
    muse_blend_fire(&b, 9, 0.5f);
    muse_blend_step(&b, 0.04f);
    assert(muse_blend_busy(&b));
    assert(b.s[muse_blend_top(&b)].id == 9);
    for (int i = 0; i < 15; i++) {              /* outlive its 0.5 s life */
        muse_blend_step(&b, 0.04f);
    }
    assert(!muse_blend_busy(&b));               /* expired: no longer in charge */
    if (b.n > 0) {
        assert(muse_blend_top(&b) == 0);        /* a fading ghost is still the top */
    }
    for (int i = 0; i < 15; i++) {
        muse_blend_step(&b, 0.04f);
    }
    assert(b.n == 0 && muse_blend_top(&b) < 0);

    /* 7. determinism. */
    muse_blend_t x, y;
    muse_blend_init(&x);
    muse_blend_init(&y);
    for (int i = 0; i < 200; i++) {
        muse_blend_fire(&x, (i % 7) + 1, 1.0f + (float)(i % 5) * 0.5f);
        muse_blend_fire(&y, (i % 7) + 1, 1.0f + (float)(i % 5) * 0.5f);
        muse_blend_step(&x, 0.04f);
        muse_blend_step(&y, 0.04f);
    }
    assert(x.n == y.n && memcmp(&x, &y, sizeof x) == 0);

    (void)run;
    printf("BLEND TEST OK\n");
    return 0;
}
