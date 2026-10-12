/* The idle scheduler (T3.2 of the face-aliveness pack; Stage C adds the per-face
 * personas + the accent beat). One shared brain for the resting face. Pure math,
 * no ESP deps -- host-tested (tools/muse/idle_test.c, tests/test_muse_idle.py;
 * cadence metrics: tools/muse/idle_sim.c).
 *
 * What it owns, all O(1) per frame:
 *   - blink cadence: randomized intervals (with an occasional longer stare), a
 *     randomized duration, an occasional DOUBLE blink (a follow-up right after the
 *     first) and a rarer slow "doze" blink that holds the lid down longer;
 *   - gaze: saccade targets with speed classes -- a fast dart, a slow drift, or a
 *     small corrective micro-saccade -- instead of one fixed smoothing constant;
 *   - micro-expressions: every few seconds one small acted beat (a brief widen, a
 *     squint, a faint smile, a one-eyed wink, or the face's ACCENT beat) with a
 *     bell envelope;
 *   - breath phase: a slow bob whose rate and depth wander so it never reads as
 *     a metronome.
 *
 * Stage C gives each face its OWN rhythm through muse_idle_cfg_t (the personas
 * below) instead of one generic cadence, and adds MUSE_IDLE_EV_ACCENT -- one
 * signature beat per face (shodan: the halo tick; stone: a glint along the
 * kintsugi veins; golem: the chest-core thump). The renderer decides how the
 * accent looks; the scheduler only times it (the per-face weight and duration
 * live in the persona).
 *
 * Priority (the caller passes both flags per frame): blinks run ALWAYS; a micro
 * beat only STARTS while allow_ev is true, and is CANCELED the moment it turns
 * false -- the faces pass false during a reaction, speech, boot/off/error, so a
 * REACT pre-empts idle and idle never rides on top of SPEAK.
 *
 * Randomness is a private xorshift seeded through muse_idle_init(), so a face
 * gets a different rhythm every boot while tests pin a seed and reproduce
 * exactly.
 *
 *   static muse_idle_t s_idle;                       // one per face
 *   static const muse_idle_cfg_t s_idle_cfg = MUSE_IDLE_PERSONA_GOLEM;
 *   if (!s_idle.rng) muse_idle_init_cfg(&s_idle, (uint32_t)(frand() * 100000.0f) + 11u, &s_idle_cfg);
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
    MUSE_IDLE_EV_ACCENT,    /* the face's signature beat (the renderer defines the look) */
};

/* One face's resting rhythm (the Stage C personas). Durations are seconds, spans
 * are the randomize-on-top amount (value = lo + rand * span), chances are
 * per-fire probabilities. The neutral profile reproduces the pre-Stage-C cadence
 * digit for digit -- muse_idle_init() keeps using it, so nothing that pinned the
 * old behavior moves. */
typedef struct {
    /* blink */
    float blink_gap_lo, blink_gap_span;     /* quiet time between blinks */
    float stare_chance;                    /* chance the gap grows longer ... */
    float stare_lo, stare_span;            /* ... by this extra pause */
    float blink_dur_lo, blink_dur_span;    /* lid travel time */
    float doze_chance;                     /* chance the blink turns slow and heavy */
    float doze_dur_lo, doze_dur_span;
    float dbl_chance;                      /* follow-up blink */
    float triple_chance;                   /* the rare second follow-up */
    /* gaze */
    float gaze_min, gaze_span;             /* quiet time between darts */
    float micro_chance;                    /* share of slots that micro-correct in place */
    float dart_extra;                      /* further share that dart (the rest drift) */
    float gaze_slow_tau;                   /* the drift's time constant */
    /* micro beats */
    float ev_lo, ev_span;                  /* quiet time between beats */
    float w_widen, w_squint, w_smile, w_wink, w_accent;   /* beat mix (relative weights) */
    float accent_dur_lo, accent_dur_span;
    /* breath */
    float breath_rate;                     /* rad/s of the bob */
    float breath_amp;                      /* how far the rate itself wanders */
    float breath_depth;                    /* px amplitude */
    float breath_wobble;                   /* slow wander of that amplitude */
} muse_idle_cfg_t;

/* The pre-Stage-C cadence, digit for digit (muse_idle_init() uses this). */
#define MUSE_IDLE_PERSONA_NEUTRAL \
    { 1.5f, 3.6f, 0.16f, 1.5f, 2.5f, 0.13f, 0.09f, 0.09f, 0.30f, 0.14f, 0.12f, 0.02f, \
      1.1f, 2.4f, 0.30f, 0.48f, 0.30f, \
      6.0f, 14.0f, 0.30f, 0.28f, 0.30f, 0.12f, 0.0f, 0.5f, 0.3f, \
      1.15f, 0.30f, 0.55f, 0.22f }

/* shodan (Muse): precise and a little impatient -- crisp quick blinks, an alert
 * gaze that micro-corrects more than it wanders, an occasional wink, and her
 * halo tick. */
#define MUSE_IDLE_PERSONA_SHODAN \
    { 1.2f, 2.6f, 0.12f, 1.2f, 2.0f, 0.10f, 0.07f, 0.05f, 0.26f, 0.10f, 0.16f, 0.015f, \
      0.9f, 1.7f, 0.42f, 0.44f, 0.10f, \
      5.0f, 11.0f, 0.26f, 0.30f, 0.24f, 0.12f, 0.08f, 0.5f, 0.2f, \
      1.30f, 0.34f, 0.42f, 0.17f }

/* stone (Hermes): massive and slow -- heavy doze blinks, long stares, a gaze
 * that drifts more than it darts, few but deep beats, a slow deep breath, and
 * the vein glint. */
#define MUSE_IDLE_PERSONA_STONE \
    { 2.2f, 4.0f, 0.22f, 2.0f, 2.8f, 0.17f, 0.11f, 0.14f, 0.38f, 0.17f, 0.06f, 0.02f, \
      1.4f, 2.8f, 0.22f, 0.46f, 0.42f, \
      8.0f, 18.0f, 0.24f, 0.26f, 0.34f, 0.06f, 0.10f, 0.9f, 0.4f, \
      0.90f, 0.24f, 0.72f, 0.30f }

/* golem (BEEPBOP): eager and bouncy -- quick blinks with frequent doubles, wide
 * darts, beats twice as often as stone, a quick breath, and the core thump. */
#define MUSE_IDLE_PERSONA_GOLEM \
    { 1.1f, 2.1f, 0.08f, 1.2f, 2.0f, 0.11f, 0.07f, 0.04f, 0.24f, 0.10f, 0.24f, 0.025f, \
      0.8f, 1.6f, 0.34f, 0.52f, 0.14f, \
      4.5f, 8.5f, 0.30f, 0.20f, 0.32f, 0.10f, 0.08f, 0.5f, 0.2f, \
      1.42f, 0.38f, 0.60f, 0.26f }

/* The neutral default lives in the header so muse_idle_init() can point at it;
 * the attribute keeps TUs that include us without initializing quiet. */
static const muse_idle_cfg_t muse_idle_default_cfg __attribute__((unused)) = MUSE_IDLE_PERSONA_NEUTRAL;

typedef struct {
    uint32_t rng;           /* private xorshift state; 0 = unseeded */
    const muse_idle_cfg_t *cfg;   /* the personality; never NULL once initialized */
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
 * gets a different rhythm, plus the face's persona; tests pass a fixed seed to
 * reproduce exactly. muse_idle_init() is the neutral-profile form. */
static inline void muse_idle_init_cfg(muse_idle_t *s, uint32_t seed, const muse_idle_cfg_t *cfg)
{
    *s = (muse_idle_t){ 0 };
    s->cfg = cfg ? cfg : &muse_idle_default_cfg;
    s->rng = seed ? seed : 0x9e3779b9u;
    s->blink_at = -10.0f;
    s->next_blink = 1.0f + muse_idle_randf(s) * 2.5f;
    s->next_gaze = 0.8f + muse_idle_randf(s) * 1.6f;
    s->gaze_tau = 0.05f;
    s->next_ev = 2.5f + muse_idle_randf(s) * 7.0f;
}

static inline void muse_idle_init(muse_idle_t *s, uint32_t seed)
{
    muse_idle_init_cfg(s, seed, NULL);
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
    const muse_idle_cfg_t *c = s->cfg ? s->cfg : &muse_idle_default_cfg;
    /* -- blinks: fired on a timer, with an occasional follow-up (a double). -- */
    if (t >= s->next_blink) {
        s->blink_at = t;
        float d = c->blink_dur_lo + muse_idle_randf(s) * c->blink_dur_span;    /* 130-220 ms */
        if (muse_idle_randf(s) < c->doze_chance) {
            d = c->doze_dur_lo + muse_idle_randf(s) * c->doze_dur_span;        /* a slow doze blink */
        }
        s->blink_dur = d;
        float r = muse_idle_randf(s);
        s->blink_queue = r < c->dbl_chance ? 1 : (r > 1.0f - c->triple_chance ? 2 : 0);   /* double; rarely a triple */
        s->blink_gap = 0.20f + muse_idle_randf(s) * 0.12f;
        float gap = c->blink_gap_lo + muse_idle_randf(s) * c->blink_gap_span;
        if (muse_idle_randf(s) < c->stare_chance) {
            gap += c->stare_lo + muse_idle_randf(s) * c->stare_span;           /* now and then, a longer stare */
        }
        s->next_blink = t + d + gap;
    }
    if (s->blink_queue > 0 && t >= s->blink_at + s->blink_dur + s->blink_gap) {
        s->blink_queue--;
        s->blink_at = t;
        s->blink_dur = c->blink_dur_lo + muse_idle_randf(s) * c->blink_dur_span;
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
        } else if (r < c->micro_chance) {                            /* a small corrective micro-saccade */
            float nx = s->tx + (muse_idle_randf(s) * 2.0f - 1.0f) * 0.25f;
            float ny = s->ty + (muse_idle_randf(s) * 2.0f - 1.0f) * 0.20f;
            s->tx = nx < -1.0f ? -1.0f : (nx > 1.0f ? 1.0f : nx);
            s->ty = ny < -0.7f ? -0.7f : (ny > 0.7f ? 0.7f : ny);
            s->gaze_tau = 0.06f;
            s->next_gaze = t + 0.5f + muse_idle_randf(s) * 1.1f;
        } else if (r < c->micro_chance + c->dart_extra) {           /* a fast dart */
            s->tx = muse_idle_randf(s) * 2.0f - 1.0f;
            s->ty = (muse_idle_randf(s) * 2.0f - 1.0f) * 0.6f;
            s->gaze_tau = 0.045f;
            s->next_gaze = t + c->gaze_min + muse_idle_randf(s) * c->gaze_span;
        } else {                                                     /* a slow deliberate drift */
            s->tx = muse_idle_randf(s) * 2.0f - 1.0f;
            s->ty = (muse_idle_randf(s) * 2.0f - 1.0f) * 0.6f;
            s->gaze_tau = c->gaze_slow_tau;
            s->next_gaze = t + 1.6f + muse_idle_randf(s) * 2.2f;
        }
    }
    if (dt > 0.0f) {
        float k = 1.0f - expf(-dt / (s->gaze_tau > 0.001f ? s->gaze_tau : 0.05f));
        s->gx += (s->tx - s->gx) * k;
        s->gy += (s->ty - s->gy) * k;
    }
    /* -- micro-expressions: one small acted beat every few seconds, canceled the
     *    moment the caller says idle is no longer in charge. -- */
    if (!allow_ev) {
        s->ev = MUSE_IDLE_EV_NONE;
        if (s->next_ev < t + 2.0f) {
            s->next_ev = t + 2.0f;                                   /* don't pop the instant idle resumes */
        }
    } else if (s->ev == MUSE_IDLE_EV_NONE && t >= s->next_ev) {
        float total = c->w_widen + c->w_squint + c->w_smile + c->w_wink + c->w_accent;
        if (total <= 0.0f) {
            total = 1.0f;
        }
        float r = muse_idle_randf(s) * total;
        if (r >= total) {
            r = total - 0.0001f;                                    /* randf() can return exactly 1 */
        }
        if (r < c->w_widen) {
            s->ev = MUSE_IDLE_EV_WIDEN;
            s->ev_dur = 0.45f + muse_idle_randf(s) * 0.45f;
        } else if (r < c->w_widen + c->w_squint) {
            s->ev = MUSE_IDLE_EV_SQUINT;
            s->ev_dur = 0.55f + muse_idle_randf(s) * 0.65f;
        } else if (r < c->w_widen + c->w_squint + c->w_smile) {
            s->ev = MUSE_IDLE_EV_SMILE;
            s->ev_dur = 0.90f + muse_idle_randf(s) * 0.80f;
        } else if (r < c->w_widen + c->w_squint + c->w_smile + c->w_wink) {
            s->ev = MUSE_IDLE_EV_WINK;
            s->ev_dur = 0.50f;
        } else {
            s->ev = MUSE_IDLE_EV_ACCENT;                            /* the face's signature beat */
            s->ev_dur = c->accent_dur_lo + muse_idle_randf(s) * c->accent_dur_span;
        }
        s->ev_side = muse_idle_randf(s) < 0.5f ? 0 : 1;
        s->ev_at = t;
        s->next_ev = t + c->ev_lo + muse_idle_randf(s) * c->ev_span;
    }
    if (s->ev != MUSE_IDLE_EV_NONE && t >= s->ev_at + s->ev_dur) {
        s->ev = MUSE_IDLE_EV_NONE;
    }
    /* -- breath: rate and depth wander slowly so the bob never reads as a metronome. -- */
    s->breath_ph += dt * (c->breath_rate + c->breath_amp * sinf(t * 0.09f + 1.7f));
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

/* The beat's 0..1 progress (0 = just started, 1 = finished); -1 = no beat playing.
 * The renderers use it for shaped accents (a flare's rise/fall, a double thump). */
static inline float muse_idle_ev_u(const muse_idle_t *s, float t)
{
    if (s->ev == MUSE_IDLE_EV_NONE) {
        return -1.0f;
    }
    float u = (t - s->ev_at) / (s->ev_dur > 0.01f ? s->ev_dur : 0.5f);
    return (u >= 0.0f && u < 1.0f) ? u : -1.0f;
}

/* The breath bob in grid pixels (the faces round it to whole pixels). */
static inline float muse_idle_breath(const muse_idle_t *s)
{
    const muse_idle_cfg_t *c = s->cfg ? s->cfg : &muse_idle_default_cfg;
    return sinf(s->breath_ph) * (c->breath_depth + c->breath_wobble * sinf(s->breath_ph * 0.23f + 0.6f));
}
