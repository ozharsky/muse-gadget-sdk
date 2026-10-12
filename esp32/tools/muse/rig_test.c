/* Host check for muse_rig.h: the easing law, and the rig's per-channel step.
 *
 *   cc -O2 -I components/muse/avatar rig_test.c -o /tmp/rig && /tmp/rig
 */
#include <math.h>
#include <stdio.h>

#include "muse_rig.h"

#define DT 0.04f

int main(void)
{
    int fail = 0;

    /* 1. one step closes the expected fraction of the gap: 1-exp(-dt/tau) */
    {
        float v = muse_ease(0.0f, 1.0f, DT, 0.07f);
        float want = 1.0f - expf(-DT / 0.07f);
        int ok = fabsf(v - want) < 1e-4f && v > 0.4f && v < 0.5f;
        printf("step fraction ~%.3f (want %.3f): %s\n", v, want, ok ? "ok" : "FAIL");
        fail |= !ok;
    }

    /* 2. converges to the target, monotonically, without overshoot */
    {
        float v = 0.0f;
        int mono = 1;
        for (int i = 0; i < 100; i++) {
            float nv = muse_ease(v, 1.0f, DT, 0.07f);
            mono &= nv >= v && nv <= 1.0f;
            v = nv;
        }
        int ok = mono && fabsf(v - 1.0f) < 1e-3f;
        printf("converges to target, monotone: %.6f: %s\n", v, ok ? "ok" : "FAIL");
        fail |= !ok;
    }

    /* 3. no overshoot for any dt/tau pair, including a mid-flight target reversal */
    {
        int ok = 1;
        for (float tau = 0.02f; tau <= 1.0f; tau += 0.02f) {
            for (float dt = 0.001f; dt <= 0.5f; dt += 0.01f) {
                float v = muse_ease(0.0f, 1.0f, dt, tau);
                ok &= v >= 0.0f && v <= 1.0f;
                float r = muse_ease(1.0f, 0.0f, dt, tau);   /* reversing mid-flight */
                ok &= r >= 0.0f && r <= 1.0f;
            }
        }
        printf("no overshoot across dt x tau grid: %s\n", ok ? "ok" : "FAIL");
        fail |= !ok;
    }

    /* 4. boundary semantics: dt=0 freezes; tau=0 is an explicit hard cut */
    {
        int ok = muse_ease(0.3f, 1.0f, 0.0f, 0.07f) == 0.3f &&
                 muse_ease(0.3f, 1.0f, DT, 0.0f) == 1.0f &&
                 muse_ease(0.3f, 1.0f, -DT, 0.07f) == 0.3f;
        printf("dt<=0 freezes, tau<=0 cuts: %s\n", ok ? "ok" : "FAIL");
        fail |= !ok;
    }

    /* 5. the rig reaches ~95% of a step within 3 tau (the visible-snap time) */
    {
        muse_rig_t r = { 0 }, t = { 0 };
        t.open = 1.0f;
        for (int i = 0; i < 6; i++) {                 /* 6 x 40 ms = 240 ms > 3 tau */
            muse_rig_step(&r, &t, DT);
        }
        int ok = r.open > 0.95f && r.open <= 1.0f;
        printf("lid crosses 95%% within 240 ms: %.3f: %s\n", r.open, ok ? "ok" : "FAIL");
        fail |= !ok;
    }

    /* 6. untouched channels stay exactly where they were (a face only feeds its live
     *    channels; the rest must not drift) */
    {
        muse_rig_t r = { .open = 0.4f, .widen = 0.2f, .bright = 0.7f, .mouth = 0.1f, .bloom = 0.3f };
        muse_rig_t t = r;
        muse_rig_step(&r, &t, DT);
        int ok = r.open == 0.4f && r.widen == 0.2f && r.bright == 0.7f &&
                 r.mouth == 0.1f && r.bloom == 0.3f;
        printf("target == current stays put: %s\n", ok ? "ok" : "FAIL");
        fail |= !ok;
    }

    /* 7. a hard target flip mid-flight still lands: no oscillation, no stall */
    {
        muse_rig_t r = { 0 }, t = { 0 };
        t.open = 1.0f;
        for (int i = 0; i < 2; i++) {
            muse_rig_step(&r, &t, DT);                /* partway up */
        }
        t.open = 0.0f;
        float peak = r.open;
        for (int i = 0; i < 60; i++) {
            muse_rig_step(&r, &t, DT);
            peak = r.open > peak ? r.open : peak;
        }
        int ok = peak <= 1.0f && r.open < 1e-3f;
        printf("mid-flight reversal lands clean: peak %.3f end %.6f: %s\n", peak, r.open,
               ok ? "ok" : "FAIL");
        fail |= !ok;
    }

    printf(fail ? "RIG TEST FAIL\n" : "RIG TEST OK\n");
    return fail;
}
