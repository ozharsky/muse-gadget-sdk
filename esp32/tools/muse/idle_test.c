/* Idle-scheduler math test (muse_idle.h): the blink cadence (double + doze blinks),
 * the saccade speed classes, the micro-expression sampler's priority rules, and the
 * breath wobble -- all deterministic under a fixed seed.
 *
 *   cc -O2 -Wall -I components/muse/avatar tools/muse/idle_test.c -lm -o /tmp/idle_test
 *   /tmp/idle_test          # prints "IDLE TEST OK" on success
 *
 * Pinned by tests/test_muse_idle.py, the same pattern as rig_test.c.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "muse_idle.h"

#define DT (1.0f / 25.0f)

static int fails;

static void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        fails++;
    }
}

/* ---- 1. blink cadence over 900 s: counts, doubles, doze blinks, gaps ---------- */
static void test_cadence(void)
{
    muse_idle_t s;
    muse_idle_init(&s, 12345u);
    int blinks = 0, doubles = 0, longs = 0, regular = 0;
    bool in_blink = false;
    float prev_start = -1e9f;
    float min_start_gap = 1e9f, min_regular_gap = 1e9f;
    float t = 0;
    for (; t < 900.0f; t += DT) {
        muse_idle_step(&s, t, DT, 0, true);
        float sh = muse_idle_shut(&s, t);
        if (!in_blink && sh > 0.02f) {                     /* a blink window opens */
            blinks++;
            float dur = s.blink_dur;                       /* this blink's true duration */
            if (dur > 0.28f) {
                longs++;
            }
            if (prev_start > -1e8f) {
                float gap = t - prev_start;                /* start-to-start */
                if (gap < min_start_gap) {
                    min_start_gap = gap;
                }
                if (gap > 0.25f && gap < 0.75f) {
                    doubles++;                             /* the queued follow-up */
                } else {
                    regular++;
                    if (gap < min_regular_gap) {
                        min_regular_gap = gap;
                    }
                }
            }
            prev_start = t;
            in_blink = true;
        } else if (in_blink && sh <= 0.02f) {
            in_blink = false;
        }
    }
    check(blinks >= 140 && blinks <= 420, "blink count in a sane band for 900 s");
    check(doubles >= 8, "double blinks occur (a second blink right after the first)");
    check(longs >= 5, "slow doze blinks occur");
    check(min_start_gap >= 0.25f, "no overlapping blinks (min start gap >= 0.25 s)");
    check(min_regular_gap >= 1.3f, "regular blinks keep a breathe-worthy spacing");
    printf("cadence: %d blinks, %d doubles, %d doze, %d regular (min regular gap %.2f s)\n",
           blinks, doubles, longs, regular, min_regular_gap);
}

/* ---- 2. gaze: bounded targets, activity, and the two speed classes ----------- */
static void test_gaze(void)
{
    muse_idle_t s;
    muse_idle_init(&s, 999u);
    int changes = 0;
    float ltx = 0, lty = 0;
    float t = 0;
    bool bounded = true;
    for (; t < 900.0f; t += DT) {
        muse_idle_step(&s, t, DT, 0, false);
        if (s.tx != ltx || s.ty != lty) {
            changes++;
            ltx = s.tx;
            lty = s.ty;
        }
        if (fabsf(s.tx) > 1.001f || fabsf(s.ty) > 0.701f || fabsf(s.gx) > 1.05f || fabsf(s.gy) > 0.75f) {
            bounded = false;
        }
    }
    check(bounded, "gaze targets and eased gaze stay inside -1..1 / -0.7..0.7");
    check(changes >= 150, "the gaze keeps moving (>=150 target changes in 900 s)");

    /* listening holds forward; thinking holds the up-corner ponder */
    muse_idle_init(&s, 7u);
    muse_idle_step(&s, 0.0f, DT, 1, false);
    check(s.tx == 0.0f && s.ty == 0.0f, "listening centers the gaze");
    muse_idle_init(&s, 7u);
    muse_idle_step(&s, 4.0f, DT, 2, false);
    check(fabsf(s.tx) == 1.0f && s.ty == -0.7f, "thinking darts to the up corner");

    /* speed classes: a fast dart lands in ~3 frames; a slow drift is still short */
    muse_idle_init(&s, 7u);
    s.next_gaze = 1e9f;                                    /* freeze the target picker */
    s.tx = 1.0f;
    s.ty = 0.0f;
    s.gaze_tau = 0.045f;
    for (int i = 0; i < 3; i++) {
        muse_idle_step(&s, 10.0f + (float)i * DT, DT, 0, false);
    }
    check(s.gx > 0.85f, "a fast dart reaches 85% of the target in ~0.12 s");
    muse_idle_init(&s, 7u);
    s.next_gaze = 1e9f;
    s.tx = 1.0f;
    s.ty = 0.0f;
    s.gaze_tau = 0.30f;
    for (int i = 0; i < 2; i++) {
        muse_idle_step(&s, 10.0f + (float)i * DT, DT, 0, false);
    }
    check(s.gx < 0.65f, "a slow drift has NOT arrived after 0.08 s");
}

/* ---- 3. micro beats: cadence, envelope, suppression, cancellation ------------- */
static void test_micro(void)
{
    muse_idle_t s;
    muse_idle_init(&s, 4242u);
    int events = 0, ev_open = 0;
    int widen = 0, squint = 0, smile = 0, wink = 0;
    bool envelopes_ok = true;
    float t = 0;
    for (; t < 900.0f; t += DT) {
        muse_idle_step(&s, t, DT, 0, true);
        float k = muse_idle_ev_k(&s, t);
        if (k > 1.0001f) {
            envelopes_ok = false;
        }
        if (!ev_open && s.ev != MUSE_IDLE_EV_NONE && k > 0.05f) {
            ev_open = 1;
            events++;
            switch (s.ev) {
            case MUSE_IDLE_EV_WIDEN: widen++; break;
            case MUSE_IDLE_EV_SQUINT: squint++; break;
            case MUSE_IDLE_EV_SMILE: smile++; break;
            case MUSE_IDLE_EV_WINK: wink++; break;
            default: break;
            }
            check(s.ev_dur >= 0.4f && s.ev_dur <= 1.75f, "beat duration inside 0.4-1.75 s");
            if (s.ev == MUSE_IDLE_EV_WINK) {
                check(s.ev_side == 0 || s.ev_side == 1, "wink side is 0 or 1");
            }
        } else if (ev_open && s.ev == MUSE_IDLE_EV_NONE) {
            ev_open = 0;
        }
    }
    check(envelopes_ok, "beat envelope never exceeds 1");
    check(events >= 35, "micro beats keep coming (>=35 in 900 s)");
    check(widen > 0 && squint > 0 && smile > 0, "the beat mix includes widen/squint/smile");
    printf("micro: %d beats (%d widen, %d squint, %d smile, %d wink)\n",
           events, widen, squint, smile, wink);

    /* suppression: no new beats, but blinks keep running */
    muse_idle_init(&s, 11u);
    int blinks = 0, in_blink = 0;
    t = 0;
    for (; t < 60.0f; t += DT) {
        muse_idle_step(&s, t, DT, 0, false);
        check(s.ev == MUSE_IDLE_EV_NONE, "no beat while suppressed");
        if (!in_blink && muse_idle_shut(&s, t) >= 0.9f) {
            blinks++;
            in_blink = 1;
        } else if (in_blink && muse_idle_shut(&s, t) < 0.05f) {
            in_blink = 0;
        }
    }
    check(blinks >= 8, "blinks run even while beats are suppressed");

    /* cancellation: a beat in flight dies the moment a reaction starts */
    muse_idle_init(&s, 11u);
    t = 0;
    for (; t < 60.0f && s.ev == MUSE_IDLE_EV_NONE; t += DT) {
        muse_idle_step(&s, t, DT, 0, true);
    }
    check(s.ev != MUSE_IDLE_EV_NONE, "a beat fires when allowed");
    muse_idle_step(&s, t, DT, 0, false);
    check(s.ev == MUSE_IDLE_EV_NONE, "REACT pre-empts: the beat cancels immediately");
    check(muse_idle_ev_k(&s, t + 0.01f) == 0.0f, "a canceled beat has no envelope");

    /* and the resumption is not instant */
    muse_idle_step(&s, t + 1.0f, DT, 0, true);
    check(s.ev == MUSE_IDLE_EV_NONE, "idle does not pop a beat the frame it resumes");
}

/* ---- 4. breath: moves, and does not keep a metronome period ------------------- */
static void test_breath(void)
{
    muse_idle_t s;
    muse_idle_init(&s, 77u);
    float lo = 1e9f, hi = -1e9f;
    float prev = 0, prev_cross = 0;
    float min_gap = 1e9f, max_gap = -1e9f;
    float t = 0;
    for (; t < 900.0f; t += DT) {
        muse_idle_step(&s, t, DT, 0, false);
        float b = muse_idle_breath(&s);
        if (b < lo) lo = b;
        if (b > hi) hi = b;
        if (prev <= 0 && b > 0) {                          /* a rising zero crossing */
            if (prev_cross > 0) {
                float gap = t - prev_cross;
                if (gap < min_gap) min_gap = gap;
                if (gap > max_gap) max_gap = gap;
            }
            prev_cross = t;
        }
        prev = b;
    }
    check(lo < -0.35f && hi > 0.35f, "the breath bob swings both ways");
    check(max_gap - min_gap > 0.20f, "the breath period wanders (not a metronome)");
    printf("breath: %.2f..%.2f, period %.2f-%.2f s\n", lo, hi, min_gap, max_gap);
}

int main(void)
{
    test_cadence();
    test_gaze();
    test_micro();
    test_breath();
    if (fails) {
        fprintf(stderr, "IDLE TEST FAILED (%d)\n", fails);
        return 1;
    }
    printf("IDLE TEST OK\n");
    return 0;
}
