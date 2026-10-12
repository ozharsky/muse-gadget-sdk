/* Crack-tear math test (muse_crack.h): the ERROR state's persistent tears -- the
 * no-teleport guarantee (no row's shift moves more than 1 px between consecutive
 * frames after rounding), tear persistence, the row/shift bounds, the black-fill row
 * shift (no wraparound, neighbours untouched) and exact reproducibility under a
 * fixed seed. The old per-frame bands would have failed the first two checks by
 * construction (fresh random rows and 1..4 px shifts every frame).
 *
 *   cc -O2 -Wall -I components/muse/avatar tools/muse/crack_test.c -lm -o /tmp/crack_test
 *   /tmp/crack_test          # prints "CRACK TEST OK" on success
 *
 * Pinned by tests/test_muse_cracks.py, the same pattern as idle_test.c.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "muse_crack.h"

#define W 64
#define H 64
#define DT (1.0f / 25.0f)

static int fails;

static void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        fails++;
    }
}

/* The applied shift of every row this frame, mirroring apply() exactly: only bands
 * with a nonzero rounded shift paint (a dormant band keeps stale y/h but writes
 * nothing), and later bands in index order win. */
static void profile(const muse_crack_t *c, int sh[H])
{
    for (int y = 0; y < H; y++) {
        sh[y] = 0;
    }
    for (int i = 0; i < c->n; i++) {
        const muse_crack_band_t *b = &c->b[i];
        int s = (int)(b->amp < 0 ? b->amp - 0.5f : b->amp + 0.5f);
        if (s == 0) {
            continue;
        }
        int y0 = (int)(b->y + 0.5f);
        for (int y = y0; y < y0 + b->h; y++) {
            if (y >= 0 && y < H) {
                sh[y] = s;
            }
        }
    }
}

/* ---- 1. the no-teleport and persistence invariants --------------------------- */
static void test_invariants(int n, float shift_max, const char *tag)
{
    muse_crack_t c;
    muse_crack_init(&c, 4242u, n, 4.0f, 59.0f, shift_max, 0.35f);

    int prev[H];
    profile(&c, prev);
    int teleports = 0, max_jump = 0, moved_rows = 0, max_run = 0, max_gap = 0, since = 0;
    int run[H] = { 0 }, moved[H] = { 0 };
    bool bounds_ok = true, rows_ok = true;
    float t = 0;

    for (; t < 40.0f; t += DT) {
        muse_crack_step(&c, DT);
        int cur[H];
        profile(&c, cur);
        bool any_now = false;
        for (int y = 0; y < H; y++) {
            int jump = cur[y] - prev[y];
            if (jump < 0) {
                jump = -jump;
            }
            if (jump > max_jump) {
                max_jump = jump;
            }
            if (jump >= 2) {
                teleports++;
            }
            if (cur[y] > (int)shift_max || cur[y] < -(int)shift_max) {
                bounds_ok = false;
            }
            if (cur[y] != 0) {
                any_now = true;
                if (!moved[y]) {
                    moved[y] = 1;
                    moved_rows++;
                }
            }
            run[y] = cur[y] != 0 ? run[y] + 1 : 0;
            if (run[y] > max_run) {
                max_run = run[y];
            }
        }
        for (int i = 0; i < c.n; i++) {
            if (c.b[i].rest <= 0.0f &&        /* a live band; dormant slots keep stale values */
                (c.b[i].h < 1 || c.b[i].h > 3 ||
                 c.b[i].y < 4.0f || c.b[i].y > 59.0f)) {
                rows_ok = false;
            }
        }
        since = any_now ? 0 : since + 1;
        if (since > max_gap) {
            max_gap = since;
        }
        memcpy(prev, cur, sizeof cur);
    }

    char msg[128];
    snprintf(msg, sizeof msg, "%s: no row shift jumps >= 2 px between frames", tag);
    check(teleports == 0, msg);
    snprintf(msg, sizeof msg, "%s: per-frame row movement <= 1 px", tag);
    check(max_jump <= 1, msg);
    snprintf(msg, sizeof msg, "%s: a tear holds its row >= 0.5 s", tag);
    check(max_run >= 13, msg);
    snprintf(msg, sizeof msg, "%s: fresh rows keep tearing over 40 s", tag);
    check(moved_rows >= 15, msg);
    snprintf(msg, sizeof msg, "%s: never crack-free for 5 s (one ongoing event)", tag);
    check(max_gap <= 125, msg);
    snprintf(msg, sizeof msg, "%s: shifts stay within the cap", tag);
    check(bounds_ok, msg);
    snprintf(msg, sizeof msg, "%s: tear rows stay in range, h in 1..3", tag);
    check(rows_ok, msg);
    printf("%s(%d, %.0f): no-teleport OK, max jump %d px, longest hold %.2f s, %d rows, max gap %.2f s\n",
           tag, n, (double)shift_max, max_jump, (double)max_run * DT, moved_rows, (double)max_gap * DT);
}

/* ---- 2. apply(): slide right, black fill, no wraparound, neighbours untouched --- */
static void test_apply(void)
{
    muse_crack_t c;
    muse_crack_init(&c, 7u, 1, 4.0f, 59.0f, 4.0f, 0.35f);
    /* force a known tear instead of waiting for the rng */
    c.b[0].rest = 0.0f;
    c.b[0].t = 0.0f;
    c.b[0].amp = 2.0f;
    c.b[0].tgt = 2.0f;
    c.b[0].hold = 5.0f;
    c.b[0].y = 10.0f;
    c.b[0].h = 2;

    uint8_t fb[W * H], before[W * H];
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            fb[y * W + x] = (uint8_t)(1 + ((x * 7 + y * 3) % 200));
        }
    }
    memcpy(before, fb, sizeof fb);

    muse_crack_apply(&c, fb, W, H, 0);
    bool ok = true;
    for (int x = 2; x < W; x++) {
        if (fb[10 * W + x] != before[10 * W + x - 2]) {
            ok = false;   /* row 10 slid right by 2 */
        }
    }
    if (fb[10 * W] != 0 || fb[10 * W + 1] != 0) {
        ok = false;       /* the vacated edge is the fill colour */
    }
    if (fb[11 * W + 3] != before[11 * W + 1]) {
        ok = false;       /* row 11 too (h = 2) */
    }
    for (int x = 0; x < W; x++) {
        if (fb[9 * W + x] != before[9 * W + x] || fb[12 * W + x] != before[12 * W + x]) {
            ok = false;   /* the rows around the tear are untouched */
        }
    }
    check(ok, "apply: rows slide by round(amp), black fill, no wraparound, neighbours untouched");

    /* and a leftward tear: the fill lands on the right edge */
    memcpy(fb, before, sizeof fb);
    c.b[0].amp = -3.0f;
    c.b[0].tgt = -3.0f;
    muse_crack_apply(&c, fb, W, H, 0);
    ok = true;
    if (fb[10 * W + 0] != before[10 * W + 3]) {
        ok = false;
    }
    if (fb[10 * W + W - 1] != 0 || fb[10 * W + W - 2] != 0 || fb[10 * W + W - 3] != 0) {
        ok = false;
    }
    check(ok, "apply: negative shift slides left, fill on the right edge");
}

/* ---- 3. reproducibility -------------------------------------------------------- */
static uint32_t run_hash(uint32_t seed)
{
    muse_crack_t c;
    muse_crack_init(&c, seed, 3, 4.0f, 59.0f, 4.0f, 0.35f);
    uint32_t h = 2166136261u;
    for (float t = 0; t < 20.0f; t += DT) {
        muse_crack_step(&c, DT);
        int cur[H];
        profile(&c, cur);
        for (int y = 0; y < H; y++) {
            h = (h ^ (uint32_t)(cur[y] + 8)) * 16777619u;
        }
    }
    return h;
}

static void test_determinism(void)
{
    check(run_hash(4242u) == run_hash(4242u), "same seed reproduces exactly");
    check(run_hash(4242u) != run_hash(99u), "a different seed makes a different pattern");
}

int main(void)
{
    test_invariants(3, 4.0f, "golem");     /* the golem's error config */
    test_invariants(5, 6.0f, "stone");     /* the stone's + shodan's error config */
    test_apply();
    test_determinism();
    if (fails) {
        fprintf(stderr, "CRACK TEST FAILED (%d)\n", fails);
        return 1;
    }
    printf("CRACK TEST OK\n");
    return 0;
}
