/* The idle scheduler (T3.2 of the face-aliveness pack): one shared brain for the
 * resting face. Pure math, no ESP deps -- host-tested (tools/muse/idle_test.c,
 * tests/test_muse_idle.py).
 *
 * What it owns, all O(1) per frame:
 *   - blink cadence: randomized intervals (with an occasional longer stare), a
 *     randomized duration, an occasional DOUBLE blink (a follow-up right after the
 *     first) and a rarer slow "doze" blink that holds the lid down longer;
 *   - gaze: saccade targets with speed classes -- a fast dart, a slow drift, or a
 *     small corrective micro-saccade -- instead of one fixed smoothing constant;
 *   - micro-expressions: every ~6-20 s one small acted beat (a brief widen, a
 *     squint, a faint smile, or a one-eyed wink) with a bell envelope;
 *   - breath phase: a slow bob whose rate and depth wander so it never reads as
 *     a metronome.
 *
 * Priority (the caller passes both flags per frame): blinks run ALWAYS; a micro
 * beat only STARTS while allow_ev is true, and is CANCELED the moment it turns
 * false -- the faces pass false during a reaction, speech, boot/off/error, so a
 * REACT pre-empts idle and idle never rides on top of SPEAK.
 *
 * Randomness is a private xorshift seeded through muse_idle_init(), so a face gets
 * a different rhythm every boot while tests pin a seed and reproduce exactly.
 *
 *   static muse_idle_t s_idle;                       // one per face
 *   if (!s_idle.rng) muse_idle_init(&s_idle, (uint32_t)(frand() * 100000.0f) + 7u);
 *   muse_idle_step(&s_idle, p->t, dt, gaze_mode, allow_ev);
 *   float close = muse_idle_shut(&s_idle, p->t);    // blink, 0 open .. 1 shut
 *   float beat  = muse_idle_ev_k(&s_idle, p->t);    // micro beat envelope, 0..1
 *   int   dy    = iround(muse_idle_breath(&s_idle)); // the breath bob, ~ +/-1 px
 */
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

enum {
    MUSE_IDLE_EV_NONE = 0,
    MUSE_IDLE_EV_WIDEN,     /* a brief "noticed something" beat */
    MUSE_IDLE_EV_SQUINT,    /* the eyes narrow for a moment */
    MUSE_IDLE_EV_SMILE,     /* a faint smile */
    MUSE_IDLE_EV_WINK,      /* one eye closes (ev_side 0 = left, 1 = right) */
};

typedef struct {
    uint32_t rng;           /* private xorshift state; 0 = unseeded */
    float last_t;           /* the face's own frame clock (dt tracking) */
    /* blink */
    float blink_at;         /* start of the blink playing now; < -5 = none */
    float blink_dur;
    float next_blink;
    int   blink_queue;      /* follow-up blinks queued (a double/triple) */
    float blink_gap;
    /* gaze */
    float next_gaze;
    float tx, ty;           /* saccade target, -1..1 */
    float gx, gy;           /* eased gaze */
    float gaze_tau;
    /* micro expressions */
    float next_ev;
    int   ev, ev_side;
    float ev_at, ev_dur;
    /* breath */
    float breath_ph;
} muse_idle_t;

static inline float muse_idle_randf(muse_idle_t *s)
{
    s->rng ^= s->rng << 13;
    s->rng ^= s->rng >> 17;
    s->rng ^= s->rng << 5;
    return (float)(s->rng & 0xffffu) / 65535.0f;
}

/* Seed once per boot: pass a value from the face's own frand() so every power-up
 * gets a different rhythm; tests pass a fixed seed to reproduce exactly. */
static inline void muse_idle_init(muse_idle_t *s, uint32_t seed)
{
    *s = (muse_idle_t){ 0 };
    s->rng = seed ? seed : 0x9e3779b9u;
    s->blink_at = -10.0f;
    s->next_blink = 1.0f + muse_idle_randf(s) * 2.5f;
    s->next_gaze = 0.8f + muse_idle_randf(s) * 1.6f;
    s->gaze_tau = 0.05f;
    s->next_ev = 2.5f + muse_idle_randf(s) * 7.0f;
}

/* One step per rendered frame.
 *   t          seconds since boot (the pose clock)
 *   dt         frame time; clamp upstream; dt <= 0 freezes the gaze easing
 *   gaze_mode  0 = wander, 1 = hold forward (listening), 2 = ponder up (thinking)
 *   allow_ev   false cancels any micro beat in flight and starts no new one
 */
static inline void muse_idle_step(muse_idle_t *s, float t, float dt, int gaze_mode, bool allow_ev)
{
    if (!s->rng) {
        muse_idle_init(s, 0);
    }
    /* -- blinks: fired on a timer, with an occasional follow-up (a double). -- */
    if (t >= s->next_blink) {
        s->blink_at = t;
        float d = 0.13f + muse_idle_randf(s) * 0.09f;               /* 130-220 ms */
        if (muse_idle_randf(s) < 0.09f) {
            d = 0.30f + muse_idle_randf(s) * 0.14f;                 /* a slow doze blink */
        }
        s->blink_dur = d;
        float r = muse_idle_randf(s);
        s->blink_queue = r < 0.12f ? 1 : (r > 0.98f ? 2 : 0);        /* double; rarely a triple */
        s->blink_gap = 0.20f + muse_idle_randf(s) * 0.12f;
        float gap = 1.5f + muse_idle_randf(s) * 3.6f;               /* 1.5-5.1 s */
        if (muse_idle_randf(s) < 0.16f) {
            gap += 1.5f + muse_idle_randf(s) * 2.5f;                /* now and then, a longer stare */
        }
        s->next_blink = t + d + gap;
    }
    if (s->blink_queue > 0 && t >= s->blink_at + s->blink_dur + s->blink_gap) {
        s->blink_queue--;
        s->blink_at = t;
        s->blink_dur = 0.13f + muse_idle_randf(s) * 0.09f;
        s->blink_gap = 0.20f + muse_idle_randf(s) * 0.12f;
        if (s->next_blink < s->blink_at + s->blink_dur + 1.2f) {
            s->next_blink = s->blink_at + s->blink_dur + 1.2f + muse_idle_randf(s) * 2.0f;
        }
    }
    /* -- gaze: speed-classed saccades instead of one fixed smoothing. -- */
    if (gaze_mode == 1) {                                            /* listening: forward, still */
        s->tx = s->ty = 0.0f;
        s->gaze_tau = 0.05f;
    } else if (t >= s->next_gaze) {
        float r = muse_idle_randf(s);
        if (gaze_mode == 2) {                                        /* thinking: up into a corner, then ponder */
            s->tx = r < 0.5f ? -1.0f : 1.0f;
            s->ty = -0.7f;
            s->gaze_tau = 0.20f;
            s->next_gaze = t + 2.2f + muse_idle_randf(s) * 2.0f;
        } else if (r < 0.30f) {                                      /* a small corrective micro-saccade */
            float nx = s->tx + (muse_idle_randf(s) * 2.0f - 1.0f) * 0.25f;
            float ny = s->ty + (muse_idle_randf(s) * 2.0f - 1.0f) * 0.20f;
            s->tx = nx < -1.0f ? -1.0f : (nx > 1.0f ? 1.0f : nx);
            s->ty = ny < -0.7f ? -0.7f : (ny > 0.7f ? 0.7f : ny);
            s->gaze_tau = 0.06f;
            s->next_gaze = t + 0.5f + muse_idle_randf(s) * 1.1f;
        } else if (r < 0.78f) {                                      /* a fast dart */
            s->tx = muse_idle_randf(s) * 2.0f - 1.0f;
            s->ty = (muse_idle_randf(s) * 2.0f - 1.0f) * 0.6f;
            s->gaze_tau = 0.045f;
            s->next_gaze = t + 1.1f + muse_idle_randf(s) * 2.4f;
        } else {                                                     /* a slow deliberate drift */
            s->tx = muse_idle_randf(s) * 2.0f - 1.0f;
            s->ty = (muse_idle_randf(s) * 2.0f - 1.0f) * 0.6f;
            s->gaze_tau = 0.30f;
            s->next_gaze = t + 1.6f + muse_idle_randf(s) * 2.2f;
        }
    }
    if (dt > 0.0f) {
        float k = 1.0f - expf(-dt / (s->gaze_tau > 0.001f ? s->gaze_tau : 0.05f));
        s->gx += (s->tx - s->gx) * k;
        s->gy += (s->ty - s->gy) * k;
    }
    /* -- micro-expressions: one small acted beat every ~6-20 s, canceled the
     *    moment the caller says idle is no longer in charge. -- */
    if (!allow_ev) {
        s->ev = MUSE_IDLE_EV_NONE;
        if (s->next_ev < t + 2.0f) {
            s->next_ev = t + 2.0f;                                   /* don't pop the instant idle resumes */
        }
    } else if (s->ev == MUSE_IDLE_EV_NONE && t >= s->next_ev) {
        float r = muse_idle_randf(s);
        if (r < 0.30f) {
            s->ev = MUSE_IDLE_EV_WIDEN;
            s->ev_dur = 0.45f + muse_idle_randf(s) * 0.45f;
        } else if (r < 0.58f) {
            s->ev = MUSE_IDLE_EV_SQUINT;
            s->ev_dur = 0.55f + muse_idle_randf(s) * 0.65f;
        } else if (r < 0.88f) {
            s->ev = MUSE_IDLE_EV_SMILE;
            s->ev_dur = 0.90f + muse_idle_randf(s) * 0.80f;
        } else {
            s->ev = MUSE_IDLE_EV_WINK;
            s->ev_dur = 0.50f;
        }
        s->ev_side = muse_idle_randf(s) < 0.5f ? 0 : 1;
        s->ev_at = t;
        s->next_ev = t + 6.0f + muse_idle_randf(s) * 14.0f;
    }
    if (s->ev != MUSE_IDLE_EV_NONE && t >= s->ev_at + s->ev_dur) {
        s->ev = MUSE_IDLE_EV_NONE;
    }
    /* -- breath: rate and depth wander slowly so the bob never reads as a metronome. -- */
    s->breath_ph += dt * (1.15f + 0.30f * sinf(t * 0.09f + 1.7f));
}

/* How closed the eyes are right now (0 open .. 1 shut), from the playing blink. */
static inline float muse_idle_shut(const muse_idle_t *s, float t)
{
    float u = (t - s->blink_at) / (s->blink_dur > 0.01f ? s->blink_dur : 0.2f);
    return (u >= 0.0f && u < 1.0f) ? sinf(u * 3.14159265f) : 0.0f;
}

/* The micro beat's bell envelope (0..1); 0 when no beat is playing. */
static inline float muse_idle_ev_k(const muse_idle_t *s, float t)
{
    if (s->ev == MUSE_IDLE_EV_NONE) {
        return 0.0f;
    }
    float u = (t - s->ev_at) / (s->ev_dur > 0.01f ? s->ev_dur : 0.5f);
    return (u >= 0.0f && u < 1.0f) ? sinf(u * 3.14159265f) : 0.0f;
}

/* The breath bob, roughly -0.8..0.8 grid pixels (faces round it to whole pixels). */
static inline float muse_idle_breath(const muse_idle_t *s)
{
    return sinf(s->breath_ph) * (0.55f + 0.22f * sinf(s->breath_ph * 0.23f + 0.6f));
}
