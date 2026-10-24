/* Host check for muse_lip.h: hysteresis + minimum hold.
 *
 *   cc -O2 -I components/muse/avatar lip_test.c -o /tmp/lip && /tmp/lip
 */
#include <math.h>
#include <stdio.h>

#include "muse_lip.h"

#define DT 0.04f

int main(void)
{
    int fail = 0;

    /* 1. silence -> stays closed */
    {
        muse_lip_t l = {0};
        for (int i = 0; i < 50; i++) {
            muse_lip(&l, 0.0f, DT);
        }
        int ok = l.state == 0;
        printf("silence stays closed: %s\n", ok ? "ok" : "FAIL");
        fail |= !ok;
    }

    /* 2. a level hovering just over the half threshold must NOT chatter: crosses OPEN_1
     *    but never dips under CLOSE_1, so it opens once and holds */
    {
        muse_lip_t l = {0};
        int changes = 0, prev = 0;
        for (int i = 0; i < 300; i++) {                 /* 12 s */
            float level = 0.18f + 0.02f * sinf((float)i * 1.3f);
            muse_lip(&l, level, DT);
            if (l.state != prev) {
                changes++;
                prev = l.state;
            }
        }
        int ok = changes == 1 && l.state == 1;
        printf("boundary hover: %d change(s), state=%d (expect 1, 1): %s\n", changes, l.state,
               ok ? "ok" : "FAIL");
        fail |= !ok;
    }

    /* 3. min-hold: a 1-frame loud pulse cannot move the state twice within 100 ms */
    {
        muse_lip_t l = {0};
        float last_change = -1, min_gap = 1e9f, t = 0;
        int prev = 0, changes = 0;
        for (int i = 0; i < 400; i++) {                 /* pulses on a 3-frame period */
            float level = (i % 3 == 0) ? 0.9f : 0.0f;
            muse_lip(&l, level, DT);
            t += DT;
            if (l.state != prev) {
                if (last_change >= 0 && t - last_change < min_gap) {
                    min_gap = t - last_change;
                }
                last_change = t;
                prev = l.state;
                changes++;
            }
        }
        int ok = changes > 2 && min_gap >= MUSE_LIP_HOLD_S - 1e-4f;
        printf("pulsed level: %d changes, min gap %.2f s (>= %.2f): %s\n", changes, min_gap,
               MUSE_LIP_HOLD_S, ok ? "ok" : "FAIL");
        fail |= !ok;
    }

    /* 4. a slow ramp opens through both states, in order */
    {
        muse_lip_t l = {0};
        int seq = 0, ok = 1;
        for (int i = 0; i <= 60; i++) {                 /* 0 -> 1.0 over 2.4 s */
            float level = (float)i / 60.0f;
            float o = muse_lip(&l, level, DT);
            if (seq == 0 && l.state == 1) {
                seq = 1;
            } else if (seq == 1 && l.state == 2) {
                seq = 2;
            }
            (void)o;
        }
        ok = seq == 2 && l.state == 2;
        printf("ramp 0..1: states reached %s: %s\n", seq == 2 ? "1 then 2" : "WRONG ORDER",
               ok ? "ok" : "FAIL");
        fail |= !ok;
    }

    /* 5. full closure returns to 0 */
    {
        muse_lip_t l = {0};
        for (int i = 0; i < 30; i++) {
            muse_lip(&l, 0.95f, DT);
        }
        int opened = l.state == 2;
        for (int i = 0; i < 30; i++) {
            muse_lip(&l, 0.0f, DT);
        }
        int ok = opened && l.state == 0;
        printf("open then silence closes: %s\n", ok ? "ok" : "FAIL");
        fail |= !ok;
    }

    /* 4. muse_lip_agc: syllables at speech's usual loudness (0.6..0.9) swing the mouth wide
     *    open and nearly shut; silence closes it */
    {
        muse_lip_agc_t a = {0};
        float lo = 1, hi = 0;
        for (int i = 0; i < 250; i++) {                 /* 10 s at 4 syllables/s */
            float o = muse_lip_agc(&a, 0.75f + 0.15f * sinf((float)i * DT * 6.2832f * 4.0f), DT);
            if (i > 50) {
                lo = o < lo ? o : lo;
                hi = o > hi ? o : hi;
            }
        }
        for (int i = 0; i < 25; i++) {
            muse_lip_agc(&a, 0.0f, DT);
        }
        int ok = hi > 0.8f && lo < 0.25f && a.open < 0.05f;
        printf("agc follows syllables (open %.2f..%.2f), closes in silence (%.2f): %s\n", lo, hi, a.open, ok ? "ok" : "FAIL");
        fail |= !ok;
    }

    printf(fail ? "LIP TEST FAIL\n" : "LIP TEST OK\n");
    return fail;
}
