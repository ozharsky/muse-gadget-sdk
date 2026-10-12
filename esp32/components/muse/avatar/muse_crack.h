/* Persistent crack tears for the faces' ERROR state: the stone strains, a tear opens,
 * holds under strain, knits back, rests, then a fresh tear opens somewhere else.
 *
 * Why this exists: the error look used to redraw fresh random glitch bands EVERY frame
 * (`glitch(3, 4, 4, 60)`); at ~25 fps the bands teleported around the face, so the
 * state read as per-frame noise instead of one ongoing event. This module keeps the
 * exact visual vocabulary (thin row bands slid sideways, black where pixels left the
 * frame) but gives each tear a life: an eased open (at most `step` px of travel per
 * FRAME, so consecutive frames never jump more than 1 px after rounding), a straining
 * hold with a slow wobble, an eased close, then a rest before that slot tears again.
 *
 * Pure math, no ESP deps -- host-tested (tools/muse/crack_test.c, pinned by
 * tests/test_muse_cracks.py).
 *
 *   static muse_crack_t s_cracks;                     // one per face
 *   ... in ERROR:     muse_crack_init(&s_cracks, seed, n, y_lo, y_hi, shift_max, gap);
 *                     muse_crack_step(&s_cracks, dt);
 *                     muse_crack_apply(&s_cracks, s_fb, W, H, IDX_BLACK);
 *   ... any other mode: if (s_cracks_run) s_cracks_run = false;   // next entry starts calm
 */
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MUSE_CRACK_MAX 5

typedef struct {
    float amp;    /* current signed shift, px */
    float tgt;    /* this tear's signed magnitude (|tgt| <= shift_max) */
    float t;      /* seconds into this tear's life */
    float hold;   /* hold time before knitting back */
    float wob;    /* strain-wobble phase */
    float rest;   /* > 0 = dormant (counting down); <= 0 = tearing */
    float y;      /* the tear's row (whole pixels) */
    int   h;      /* rows tall, 1..3 */
} muse_crack_band_t;

typedef struct {
    uint32_t rng;            /* private xorshift; 0 = unseeded */
    int      n;              /* tears in flight, 1..MUSE_CRACK_MAX */
    float    step;           /* max |amp| move per frame; < = 1 px after rounding */
    float    y_lo, y_hi;     /* spawn rows */
    float    shift_max;      /* |shift| cap at spawn */
    muse_crack_band_t b[MUSE_CRACK_MAX];
} muse_crack_t;

static inline float muse_crack_randf(muse_crack_t *c)
{
    c->rng ^= c->rng << 13;
    c->rng ^= c->rng >> 17;
    c->rng ^= c->rng << 5;
    return (float)(c->rng & 0xffffu) / 65535.0f;
}

/* Configure + stagger: `gap` is roughly how far apart the first tears open, so the
 * state starts as a buildup, not every slot tearing on frame one. */
static inline void muse_crack_init(muse_crack_t *c, uint32_t seed, int n,
                                   float y_lo, float y_hi, float shift_max, float gap)
{
    memset(c, 0, sizeof *c);
    c->rng = seed ? seed : 0x9e3779b9u;
    c->n = n < 1 ? 1 : (n > MUSE_CRACK_MAX ? MUSE_CRACK_MAX : n);
    c->step = 0.5f;
    c->y_lo = y_lo;
    c->y_hi = y_hi;
    c->shift_max = shift_max;
    for (int i = 0; i < c->n; i++) {
        c->b[i].rest = gap * (float)i + 0.05f;
    }
}

/* One step per rendered frame, while the face is in ERROR. */
static inline void muse_crack_step(muse_crack_t *c, float dt)
{
    for (int i = 0; i < c->n; i++) {
        muse_crack_band_t *b = &c->b[i];
        if (b->rest > 0.0f) {
            b->rest -= dt;
            if (b->rest > 0.0f) {
                continue;
            }
            /* Spawn: a fresh tear, on a row no live tear is using. The rows are few
             * (56), so SCAN them and reservoir-sample one of the free ones -- a
             * retry loop can give up and overlap, and two live bands on one row
             * would re-open the teleport the module exists to prevent. */
            int h = 1 + (int)(muse_crack_randf(c) * 3.0f);
            b->h = h > 3 ? 3 : h;
            float pick = -1.0f;
            int free_n = 0;
            for (float y = c->y_lo; y <= c->y_hi; y += 1.0f) {
                bool clash = false;
                for (int j = 0; j < c->n; j++) {
                    if (j != i && c->b[j].rest <= 0.0f &&
                        fabsf(c->b[j].y - y) < (float)(b->h + c->b[j].h + 1)) {
                        clash = true;
                    }
                }
                if (!clash) {
                    free_n++;
                    if (muse_crack_randf(c) < 1.0f / (float)free_n) {
                        pick = y;
                    }
                }
            }
            if (pick < 0.0f) {
                pick = c->y_lo + muse_crack_randf(c) * (c->y_hi - c->y_lo);   /* no free row: anyway */
            }
            b->y = floorf(pick + 0.5f);
            float mag = 1.5f + muse_crack_randf(c) * (c->shift_max - 1.5f);
            b->tgt = muse_crack_randf(c) < 0.5f ? -mag : mag;
            b->hold = 0.55f + muse_crack_randf(c) * 0.85f;
            b->wob = muse_crack_randf(c) * 6.283f;
            b->t = 0.0f;
            b->rest = 0.0f;
            b->amp = 0.0f;
            continue;   /* the first live frame starts at amp 0 and ramps from the next step */
        }
        b->t += dt;
        float target;
        if (b->t < b->hold) {
            /* The strain wobble only ever pulls the shift NARROWER, so |amp| never
             * exceeds |tgt| and the <= 1 px/frame cap holds through the whole hold. */
            target = b->tgt * (1.0f - 0.10f * (0.5f + 0.5f * sinf(b->t * 7.0f + b->wob)));
        } else {
            target = 0.0f;
        }
        float d = target - b->amp;
        if (d > c->step) {
            d = c->step;
        }
        if (d < -c->step) {
            d = -c->step;
        }
        b->amp += d;
        if (b->t >= b->hold && b->amp == 0.0f) {
            b->rest = 0.25f + muse_crack_randf(c) * 0.80f;   /* a beat, then a new tear */
        }
    }
}

/* Paint the tears into the face's own framebuffer: rows slide sideways, the vacated
 * edge fills `fill` (the old glitch's shape -- no wraparound). */
static inline void muse_crack_apply(const muse_crack_t *c, uint8_t *fb, int w, int h, uint8_t fill)
{
    for (int i = 0; i < c->n; i++) {
        const muse_crack_band_t *b = &c->b[i];
        int sh = (int)(b->amp < 0 ? b->amp - 0.5f : b->amp + 0.5f);
        if (sh == 0 || sh <= -w || sh >= w) {
            continue;
        }
        int y0 = (int)(b->y + 0.5f);
        for (int y = y0; y < y0 + b->h; y++) {
            if (y < 0) {
                continue;
            }
            if (y >= h) {
                break;
            }
            uint8_t *row = fb + (size_t)y * w;
            if (sh > 0) {
                memmove(row + sh, row, (size_t)(w - sh));
                memset(row, fill, (size_t)sh);
            } else {
                memmove(row, row - sh, (size_t)(w + sh));
                memset(row + (w + sh), fill, (size_t)(-sh));
            }
        }
    }
}
