/* The cross-mood blend: a small weight vector over the moods a face has live at once.
 *
 * Why this exists: a [react:name] used to REPLACE the previous mood in a single
 * frame. The rig eases the continuous channels, but the weighted layers (props,
 * poses) popped: a mood mid-flight lost all of its paint the instant another
 * arrived. This module holds the blend state; the faces turn it into pixels:
 *
 *   - each SLOT is one live mood with its own age (its rt clock), its duration, and
 *     a blend weight w;
 *   - firing a mood makes it the owner; every other slot stops owning and starts
 *     fading: w falls to 0 over FALL. The owner's w rises from 0 to 1 over RISE
 *     (so even its first frame is a slice of the way there, never the whole face);
 *   - a mood that reaches its duration stops owning and fades the same way -- its
 *     mood-clock ramp rk is already 0 by then, so the props are gone exactly at the
 *     end, but the acting weight hands over instead of snapping;
 *   - slots are dropped once their weight reaches ~0, and the oldest is evicted if
 *     MUSE_BLEND_MAX moods overlap.
 *
 * RISE and FALL are the dissolve constants the faces already used for rk (0.15 s
 * in, 0.4 s out), so a SINGLE mood renders exactly as before -- rk's twin:
 *
 *     rk(s)   = clamp(min(age/RISE, (dur-age)/FALL), 0, 1)   // the mood-clock ramp
 *     rend(s) = min(w, rk(s))                               // what weighted layers draw with
 *
 * A single mood has w == rk through its whole life, so rend == rk: byte-identical
 * single-fire behavior. With two moods live, each weighted layer cross-fades (the
 * old one's rend falls while the new one's rises) and the acting channels blend:
 *
 *     target_c = base_c + sum_i ( w_i / max(1, sum w) ) * (act_i.c - base_c)
 *
 * which is the new mood at full weight once it owns the face alone, a smooth
 * hand-over while both are live, and a 0.4 s ramp back to the mode's own acting
 * when the last mood leaves. Thresholded things (3-state lids, shakes, glitch
 * bursts) stay discrete: the faces read those from the OWNING slot.
 *
 * Pure math, no ESP deps -- host-tested (tools/muse/blend_test.c, pinned by
 * tests/test_muse_blend.py).
 *
 *   static muse_blend_t s_blend;
 *   ... on [react:]:        muse_blend_fire(&s_blend, id, dur);
 *   ... once per frame:     muse_blend_step(&s_blend, dt);
 *   for (int i = 0; i < s_blend.n; i++) {
 *       float w = muse_blend_rend(&s_blend.s[i]);   // weighted layers
 *       ...
 *   }
 *   int rid = muse_blend_top(&s_blend);             // discrete effects, scheme
 */
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define MUSE_BLEND_MAX  3        /* live moods tracked at once */
#define MUSE_BLEND_RISE 0.15f    /* the old rk in-ramp: props/poses dissolve in */
#define MUSE_BLEND_FALL 0.40f    /* the old rk out-ramp: and the cross-fade out */
#define MUSE_BLEND_EPS  0.004f   /* below this a fading mood is gone */

typedef struct {
    int   id;      /* the face's mood id (R_*) */
    float age;     /* seconds since this mood started (its own rt clock) */
    float dur;     /* the mood's lifetime, from the face's REACTIONS table */
    float w;       /* blend weight: rises to 1 for the owner, falls to 0 for the rest */
    float w0;      /* the weight when this mood stopped owning (the fall eases from it) */
    float fall_t;  /* seconds since the fall began */
    bool  own;     /* still the owning mood (not superseded, not expired) */
} muse_blend_slot_t;

typedef struct {
    muse_blend_slot_t s[MUSE_BLEND_MAX];
    int   n;
    int   req;       /* pending fire: mood id; -1 = none */
    float req_dur;
} muse_blend_t;

static inline void muse_blend_init(muse_blend_t *b)
{
    *b = (muse_blend_t){ 0 };
    b->req = -1;   /* 0 and -1 both mean "no pending fire" (R_NONE is 0) */
}

/* A mood arrives (from the render task consuming its [react:] request). */
static inline void muse_blend_fire(muse_blend_t *b, int id, float dur)
{
    b->req = id;
    b->req_dur = dur;
}

/* The mood's own ramp (the old rk): dissolve in over RISE, hold, dissolve out
 * over the last FALL seconds of its life. 0 before it starts and after it ends. */
static inline float muse_blend_rk(const muse_blend_slot_t *s)
{
    float a = s->age / MUSE_BLEND_RISE;
    float z = (s->dur - s->age) / MUSE_BLEND_FALL;
    float v = a < z ? a : z;
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* What the weighted layers (props, poses, decorations) draw with: the blend
 * weight, but never above the mood's own ramp. One mood: == rk, exactly as before. */
static inline float muse_blend_rend(const muse_blend_slot_t *s)
{
    float rk = muse_blend_rk(s);
    return s->w < rk ? s->w : rk;
}

static inline float muse_blend_sum(const muse_blend_t *b)
{
    float w = 0.0f;
    for (int i = 0; i < b->n; i++) {
        w += b->s[i].w;
    }
    return w;
}

/* Some mood is still in charge (the idle beats stay cancelled while it is). */
static inline bool muse_blend_busy(const muse_blend_t *b)
{
    for (int i = 0; i < b->n; i++) {
        if (b->s[i].own && b->s[i].age < b->s[i].dur) {
            return true;
        }
    }
    return false;
}

/* The slot the discrete effects (shake, dy, glitch bursts, the palette's mood
 * override) read from: the owner, else the strongest survivor; -1 if none. */
static inline int muse_blend_top(const muse_blend_t *b)
{
    int top = -1;
    for (int i = 0; i < b->n; i++) {
        if (b->s[i].own && b->s[i].age < b->s[i].dur) {
            return i;
        }
        if (top < 0 || b->s[i].w > b->s[top].w) {
            top = i;
        }
    }
    return top;
}

/* One step per rendered frame. dt <= 0 freezes (nothing ages, nothing fades).
 * A zero-initialized blend (the faces' static) is valid: req == 0 means none. */
static inline void muse_blend_step(muse_blend_t *b, float dt)
{
    if (b->req > 0) {
        int id = b->req, f = -1;
        b->req = -1;
        for (int i = 0; i < b->n; i++) {
            if (b->s[i].id == id) {
                f = i;              /* a repeat: restart the mood, keep the weight */
            }
        }
        if (f < 0) {
            if (b->n == MUSE_BLEND_MAX) {
                /* full: evict the weakest survivor to make room */
                int lo = 0;
                for (int i = 1; i < b->n; i++) {
                    if (b->s[i].w < b->s[lo].w) {
                        lo = i;
                    }
                }
                b->s[lo] = b->s[b->n - 1];
                b->n--;
                f = b->n++;
            } else {
                f = b->n++;
            }
            b->s[f].id = id;
            b->s[f].age = 0.0f;
            b->s[f].dur = b->req_dur;
            b->s[f].w = 0.0f;
        } else {
            b->s[f].age = 0.0f;     /* a repeat restarts the ramp; w stays (no dip) */
            b->s[f].dur = b->req_dur;
        }
        b->s[f].own = true;
        b->s[f].w0 = 0.0f;
        b->s[f].fall_t = 0.0f;
        for (int i = 0; i < b->n; i++) {
            if (b->s[i].own && i != f) {
                b->s[i].own = false;   /* superseded: the face hands over */
                b->s[i].w0 = b->s[i].w;
                b->s[i].fall_t = 0.0f;
            }
        }
    }
    if (dt <= 0.0f) {
        return;
    }
    for (int i = 0; i < b->n; i++) {
        muse_blend_slot_t *s = &b->s[i];
        s->age += dt;
        if (s->own && s->age >= s->dur) {
            s->own = false;         /* outlived its life: hand the face over */
            s->w0 = s->w;
            s->fall_t = 0.0f;
        }
        if (s->own) {
            s->w += dt / MUSE_BLEND_RISE;
            if (s->w > 1.0f) {
                s->w = 1.0f;
            }
        } else {
            /* The fall eases IN -- w = w0 * (1 - p^2) -- so the frame a mood is
             * replaced barely moves it (the dither loses no cell on frame one),
             * then the hand-over completes over FALL. Linear steps would drop two
             * dither cells (12.5%) of the old mood's props on the very first frame. */
            s->fall_t += dt;
            float p = s->fall_t / MUSE_BLEND_FALL;
            s->w = p >= 1.0f ? 0.0f : s->w0 * (1.0f - p * p);
        }
    }
    /* drop the dead, keep the array compact and stable */
    for (int i = 0; i < b->n;) {
        if (!b->s[i].own && b->s[i].w <= MUSE_BLEND_EPS) {
            for (int j = i; j < b->n - 1; j++) {
                b->s[j] = b->s[j + 1];
            }
            b->n--;
        } else {
            i++;
        }
    }
}
