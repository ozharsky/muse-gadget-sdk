// Copyright (c) Meta Platforms, Inc. and affiliates.
/*
 * Avatar: a cold, watching machine intelligence. A pale grey metal face with a stepped
 * forehead and a single circuit stripe, framed by a mass of green-lit roots and thin
 * kinked lines of light. The picture itself is fixed pixel art (sprite_data.h); what
 * moves is light: it recolours with the state (green idle, cyan listening, violet
 * thinking, lime speaking, red on error), pulses along the circuit lines, shimmers in
 * the roots, and the eyes watch and blink. It glitches when it talks.
 *
 * The face itself (eyes, brows, mouth) is a pose library baked from Qwen-generated art
 * (shodan_poses.h): each pose is a sparse set of index deltas over the sprite, blended
 * with the same 4x4 Bayer dither so transitions stay pixel-art. Rebuild the header with
 * ~/muse/art-pipeline/shodan_poses/make_shodan_poses.py; never hand-edit it.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "muse_pixel.h"
#include "muse_lip.h"
#include "muse_rig.h"   /* T3.1 rig: eased expression channels */
#include "muse_idle.h"  /* T3.2 idle scheduler: blink cadence, saccades, micro beats, breath */
#include "muse_crack.h" /* the error state's persistent crack tears */
#include "muse_blend.h" /* the cross-mood blend: live moods cross-fade instead of cutting */
#include "sprite_data.h"
#include "shodan_poses.h"   /* the baked face-pose library (art-pipeline/shodan_poses/) */

#if defined(__has_include) && __has_include("esp_attr.h")
#include "esp_attr.h"   /* PSRAM placement for the frame buffers */
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR   /* host preview builds: ordinary memory */
#endif

#define W MUSE_PX_W
#define H MUSE_PX_H
#define TAU 6.2831853f

static muse_lip_t s_lip;   /* speaking-mouth state (hysteresis + min-hold) */

/* Colours past the sprite's own: the state's glow ramp, its accent, and white. */
enum { D_G0 = SPR_N, D_G1, D_G2, D_G3, D_ACC, D_WHITE, N_PAL };

typedef struct {
    uint32_t f[4];
    uint32_t acc;
    float hue, sat, val;   /* how the sprite's greens are recoloured: hue shift in degrees, scales */
    float spread;          /* 1 keeps the sprite's range of hues; near 0 squeezes them all to one */
} scheme_t;

static const scheme_t SCHEMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT]      = { { 0xffffff, 0xc8ffd8, 0x5cff9a, 0x12a050 }, 0x7dffb0, 0, 1.0f, 1.0f, 1.0f },
    [MUSE_MODE_IDLE]      = { { 0xd8ffd0, 0x7aff52, 0x2fc92b, 0x138a1c }, 0x4dff3c, 0, 1.0f, 1.0f, 1.0f },
    [MUSE_MODE_LISTENING] = { { 0xe0fff8, 0x6bffe8, 0x24c9d9, 0x1a6fa8 }, 0x3ee8ff, 62, 1.0f, 1.05f, 1.0f },
    [MUSE_MODE_THINKING]  = { { 0xf4e6ff, 0xc58bff, 0x8a4dff, 0x4a2aa8 }, 0xa064ff, 158, 1.0f, 1.0f, 1.0f },
    [MUSE_MODE_SPEAKING]  = { { 0xf8ffd8, 0xd2ff4a, 0x8be01c, 0x4c9a10 }, 0xc4ff3c, -22, 1.0f, 1.12f, 1.0f },
    [MUSE_MODE_ERROR]     = { { 0xffe0d8, 0xff5a4d, 0xc71f2e, 0x5a1020 }, 0xff3a3a, -130, 1.25f, 1.05f, 0.12f },
    [MUSE_MODE_OFF]       = { { 0x9fb89f, 0x4f7a4f, 0x2a4a2a, 0x142214 }, 0x2a5a2a, 0, 0.35f, 0.45f, 1.0f },
};

static float s_hue, s_sat = 1.0f, s_val = 1.0f, s_spread = 1.0f;
static float s_ramp[5][3];
static bool s_pal_init;
static uint16_t s_pal[N_PAL];
static uint16_t s_pal_dim[N_PAL];

static EXT_RAM_BSS_ATTR uint8_t s_fb[W * H];   /* PSRAM: internal RAM is needed for TLS */

static const uint8_t BAYER4[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
};

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static inline int iround(float v)
{
    return (int)(v < 0 ? v - 0.5f : v + 0.5f);
}

static inline uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static uint32_t s_rng = 0x9e3779b9u;
static uint32_t irand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static float frand(void)
{
    return (float)(irand() & 0xffff) / 65535.0f;
}

/* The error cracks (muse_crack.h): persistent tears instead of per-frame teleports. */
static muse_crack_t s_cracks;
static bool s_cracks_run;

static inline void px(int x, int y, uint8_t c)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        s_fb[y * W + x] = c;
    }
}

static uint16_t to565f(float r, float g, float b)
{
    int ri = iround(r), gi = iround(g), bi = iround(b);
    ri = ri < 0 ? 0 : ri > 255 ? 255 : ri;
    gi = gi < 0 ? 0 : gi > 255 ? 255 : gi;
    bi = bi < 0 ? 0 : bi > 255 ? 255 : bi;
    return (uint16_t)(((ri >> 3) << 11) | ((gi >> 2) << 5) | (bi >> 3));
}

static void rgb_to_hsv(float r, float g, float b, float *h, float *s, float *v)
{
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    *v = mx;
    *s = mx > 0 ? d / mx : 0;
    float hh = 0;
    if (d > 0) {
        if (mx == r) {
            hh = fmodf((g - b) / d, 6.0f);
        } else if (mx == g) {
            hh = (b - r) / d + 2.0f;
        } else {
            hh = (r - g) / d + 4.0f;
        }
        hh *= 60.0f;
        if (hh < 0) {
            hh += 360.0f;
        }
    }
    *h = hh;
}

static void hsv_to_rgb(float h, float s, float v, float *r, float *g, float *b)
{
    h = fmodf(h, 360.0f);
    if (h < 0) {
        h += 360.0f;
    }
    float c = v * s, x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f)), m = v - c;
    float rr = 0, gg = 0, bb = 0;
    if (h < 60) { rr = c; gg = x; }
    else if (h < 120) { rr = x; gg = c; }
    else if (h < 180) { gg = c; bb = x; }
    else if (h < 240) { gg = x; bb = c; }
    else if (h < 300) { rr = x; bb = c; }
    else { rr = c; bb = x; }
    *r = rr + m; *g = gg + m; *b = bb + m;
}

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    return SCHEMES[mode < MUSE_MODE_COUNT ? mode : MUSE_MODE_IDLE].acc;
}

/* The sprite's greens slide to the state's hue; the pale metal only takes a faint tint of it. */
static void update_palette(const scheme_t *t, float dt)
{
    float k = s_pal_init ? 1.0f - expf(-dt * 7.0f) : 1.0f;
    float agent_hue = 0.0f;   /* Hermes has its own avatar (muse_pixel_stone.c) */
    s_hue += (t->hue + agent_hue - s_hue) * k;
    s_sat += (t->sat - s_sat) * k;
    s_val += (t->val - s_val) * k;
    s_spread += (t->spread - s_spread) * k;
    uint32_t tg[5] = { t->f[0], t->f[1], t->f[2], t->f[3], t->acc };
    for (int i = 0; i < 5; i++) {
        float tr = (float)((tg[i] >> 16) & 255), tgc = (float)((tg[i] >> 8) & 255), tb = (float)(tg[i] & 255);
        if (agent_hue != 0.0f) {
            float h, s, v;
            rgb_to_hsv(tr / 255.0f, tgc / 255.0f, tb / 255.0f, &h, &s, &v);
            hsv_to_rgb(h + agent_hue, s, v, &tr, &tgc, &tb);
            tr *= 255.0f; tgc *= 255.0f; tb *= 255.0f;
        }
        if (!s_pal_init) {
            s_ramp[i][0] = tr; s_ramp[i][1] = tgc; s_ramp[i][2] = tb;
        } else {
            s_ramp[i][0] += (tr - s_ramp[i][0]) * k;
            s_ramp[i][1] += (tgc - s_ramp[i][1]) * k;
            s_ramp[i][2] += (tb - s_ramp[i][2]) * k;
        }
    }
    s_pal_init = true;
    for (int i = 0; i < SPR_N; i++) {
        float r = SPR_PAL[i][0] / 255.0f, g = SPR_PAL[i][1] / 255.0f, b = SPR_PAL[i][2] / 255.0f;
        float h, s, v;
        rgb_to_hsv(r, g, b, &h, &s, &v);
        if (s > 0.12f) {
            h = 130.0f + s_hue + (h - 130.0f) * s_spread;
            s = clampf(s * s_sat, 0, 1);
            v = clampf(v * s_val, 0, 1);
            hsv_to_rgb(h, s, v, &r, &g, &b);
        } else {
            v = clampf(v * (s_val > 1.0f ? 1.0f : s_val), 0, 1);
            hsv_to_rgb(h, s, v, &r, &g, &b);
            r = r * 0.93f + s_ramp[4][0] / 255.0f * 0.07f * v;      /* the metal catches the glow */
            g = g * 0.93f + s_ramp[4][1] / 255.0f * 0.07f * v;
            b = b * 0.93f + s_ramp[4][2] / 255.0f * 0.07f * v;
        }
        s_pal[i] = to565f(r * 255.0f, g * 255.0f, b * 255.0f);
        s_pal_dim[i] = to565f(r * 255.0f * 0.72f, g * 255.0f * 0.72f, b * 255.0f * 0.72f);
    }
    for (int i = 0; i < 4; i++) {
        s_pal[D_G0 + i] = to565f(s_ramp[i][0], s_ramp[i][1], s_ramp[i][2]);
        s_pal_dim[D_G0 + i] = to565f(s_ramp[i][0] * 0.72f, s_ramp[i][1] * 0.72f, s_ramp[i][2] * 0.72f);
    }
    s_pal[D_ACC] = to565f(s_ramp[4][0], s_ramp[4][1], s_ramp[4][2]);
    s_pal_dim[D_ACC] = to565f(s_ramp[4][0] * 0.72f, s_ramp[4][1] * 0.72f, s_ramp[4][2] * 0.72f);
    s_pal[D_WHITE] = to565f(255, 255, 255);
    s_pal_dim[D_WHITE] = to565f(184, 184, 184);
}

/* ---------------------------------------------------------------------------
 * Eyes
 * ------------------------------------------------------------------------- */

typedef struct {
    int n;
    uint8_t x[24], y[24], c[24];
    int xmin, xmax;
} eye_t;

static eye_t s_eye[2];
static bool s_eyes_init;

static void init_eyes(void)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (SPR_KIND[y * W + x] != 2) {
                continue;
            }
            eye_t *e = &s_eye[x < 32 ? 0 : 1];
            if (e->n < 24) {
                e->x[e->n] = (uint8_t)x;
                e->y[e->n] = (uint8_t)y;
                e->c[e->n] = SPR[y * W + x];
                e->n++;
            }
        }
    }
    for (int s = 0; s < 2; s++) {
        s_eye[s].xmin = 99;
        s_eye[s].xmax = -1;
        for (int i = 0; i < s_eye[s].n; i++) {
            if (s_eye[s].x[i] < s_eye[s].xmin) s_eye[s].xmin = s_eye[s].x[i];
            if (s_eye[s].x[i] > s_eye[s].xmax) s_eye[s].xmax = s_eye[s].x[i];
        }
    }
    s_eyes_init = true;
}

/* The idle scheduler (T3.2): blink cadence (doubles, doze blinks), speed-classed
 * saccades, micro-expression beats and the breath -- the shared muse_idle.h,
 * host-tested in tools/muse/idle_test.c. The reaction poses still poke s_idle.tx/ty
 * directly; the scheduler treats a poke as the new target. */
static muse_idle_t s_idle;

/* Eased speaking level: the squint-on-speech lid droop rides the voice. */
static float s_spk;

/* The T3.1 rig: eased expression channels. The targets are computed per frame in
 * muse_pixel_render; these hold what is actually drawn, so a mood or mode change
 * reads as motion, not a cut. First frame: settled (no fade-in from zero). */
static muse_rig_t s_rig;
static bool s_rig_init;

/* Returns how closed the eyes are (0 open .. 1 shut) and steps the gaze/beats. */
static float look_update(const muse_pose_t *p, float dt, bool allow_ev)
{
    if (!s_idle.rng) {
        muse_idle_init(&s_idle, (uint32_t)(frand() * 100000.0f) + 7u);
    }
    int gm = p->mode == MUSE_MODE_LISTENING ? 1 : p->mode == MUSE_MODE_THINKING ? 2 : 0;
    muse_idle_step(&s_idle, p->t, dt, gm, allow_ev);
    return muse_idle_shut(&s_idle, p->t);
}

/* One baked pose blended into the framebuffer: a sparse index delta with ordered
 * dithering, so a partial weight reads as a pixel-art mix of the sprite and the pose. */
static void pose_apply(int pi, float w)
{
    if (w <= 0.001f) {
        return;
    }
    const shodan_pose_t *p = &SHODAN_POSES[pi];
    const uint16_t *idx = &SHODAN_POSE_IDX[p->off];
    const uint8_t *col = &SHODAN_POSE_COL[p->off];
    int thr = (int)(w * 16.0f + 0.5f);
    if (thr >= 16) {
        for (int k = 0; k < p->n; k++) {
            s_fb[idx[k]] = col[k];
        }
        return;
    }
    for (int k = 0; k < p->n; k++) {
        int i = idx[k];
        if (BAYER4[(i / W) & 3][(i % W) & 3] < thr) {
            s_fb[i] = col[k];
        }
    }
}

/* Error mode's face: the lenses become Xs. */
static void draw_xeyes(void)
{
    for (int s = 0; s < 2; s++) {
        const eye_t *e = &s_eye[s];
        for (int i = 0; i < e->n; i++) {
            px(e->x[i], e->y[i], IDX_SOCKET);
        }
        int cx = (e->xmin + e->xmax) / 2;
        for (int d = -2; d <= 2; d++) {
            px(cx + d, 33 + d, D_G1);
            px(cx + d, 33 - d, D_G1);
        }
    }
}

/* The state's glow on the lens pixels a pose left untouched (same rule as the old
 * draw_eyes path, but now the poses own most of the lens colour). */
static void pose_eye_glow(float bright)
{
    for (int y = 22; y < 42; y++) {
        for (int x = 17; x < 47; x++) {
            uint8_t *v = &s_fb[y * W + x];
            if (*v == IDX_EYE_U && bright > 0.8f) {
                *v = D_G0;
            } else if (*v == IDX_EYE_R && bright > 0.5f && ((x + y) & 1) == 0) {
                *v = D_G1;
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Light: pulses along the circuit lines, shimmer in the roots
 * ------------------------------------------------------------------------- */

static void animate_light(float t, float speed, float level)
{
    int tick = (int)(t * 3.0f);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t kind = SPR_KIND[y * W + x];
            if (kind == 1) {
                float ph = fmodf((float)(x * 9 + y * 13) / 96.0f - t * speed, 1.0f);
                if (ph < 0) {
                    ph += 1.0f;
                }
                if (ph < 0.10f) {
                    s_fb[y * W + x] = D_WHITE;
                } else if (ph < 0.22f) {
                    s_fb[y * W + x] = D_G0;
                }
            } else if (kind == 3) {
                if (((hash2(x, y) + (uint32_t)tick) & 7u) == 0u) {
                    s_fb[y * W + x] = SPR_BRIGHT[s_fb[y * W + x]];
                } else if (level > 0.3f && ((hash2(x, y + 7) + (uint32_t)(t * 12.0f)) & 15u) == 0u) {
                    s_fb[y * W + x] = SPR_BRIGHT[SPR_BRIGHT[s_fb[y * W + x]]];
                }
            }
        }
    }
}

/* A ring that ripples with the voice, in place of the dotted rings. Driven by loudness, not the waveform. */
static void draw_wave(float t, float level)
{
    float lv = level < 0.12f ? 0.12f : level;
    for (int i = 0; i < 180; i++) {
        float a = (float)i * TAU / 180.0f;
        float w = lv * 2.4f * sinf(a * 7.0f + t * 9.0f) + lv * 1.0f * sinf(a * 13.0f - t * 6.0f);
        float r = 26.0f + w;
        px(iround(31.5f + cosf(a) * r), iround(31.5f + sinf(a) * r), (i & 1) ? D_G1 : D_G0);
    }
}

/* ---------------------------------------------------------------------------
 * Reactions: short acted moods Muse can trigger (the avatar.react command)
 * ------------------------------------------------------------------------- */

enum {
    R_NONE, R_SMIRK, R_SCAN, R_ALERT, R_GLITCH, R_SQUINT, R_ANGRY, R_SURPRISED, R_PLEASED,
    R_LAUGH, R_WINK, R_SLEEPY, R_CONFUSED, R_EVIL, R_HACKED, R_POWERUP, R_BORED, R_DISGUST, R_TARGET,
    R_NERVOUS, R_PROUD, R_COUNT
};

static const struct { const char *name; float dur; } REACTIONS[R_COUNT] = {
    [R_SMIRK] = { "smirk", 2.8f },       [R_SCAN] = { "scan", 2.6f },         [R_ALERT] = { "alert", 2.2f },
    [R_GLITCH] = { "glitch", 1.8f },     [R_SQUINT] = { "squint", 2.6f },     [R_ANGRY] = { "angry", 2.6f },
    [R_SURPRISED] = { "surprised", 1.8f }, [R_PLEASED] = { "pleased", 2.6f }, [R_LAUGH] = { "laugh", 2.4f },
    [R_WINK] = { "wink", 1.6f },         [R_SLEEPY] = { "sleepy", 3.4f },     [R_CONFUSED] = { "confused", 2.6f },
    [R_EVIL] = { "evil", 3.0f },         [R_HACKED] = { "hacked", 2.4f },     [R_POWERUP] = { "powerup", 2.4f },
    [R_BORED] = { "bored", 3.2f },       [R_DISGUST] = { "disgust", 2.4f },   [R_TARGET] = { "target", 2.6f },
    [R_NERVOUS] = { "nervous", 2.6f },   [R_PROUD] = { "proud", 2.8f },
};

static volatile int s_react_req;     /* set from any task; the renderer picks it up */

/* Muse's avatar.react command. false if there's no reaction by that name. */
bool muse_pixel_react(const char *name)
{
    for (int i = 1; i < R_COUNT; i++) {
        if (name && strcmp(name, REACTIONS[i].name) == 0) {
            s_react_req = i;
            return true;
        }
    }
    return false;
}

static void ln(int x0, int y0, int x1, int y1, uint8_t c)
{
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int n = dx > dy ? dx : dy;
    for (int i = 0; i <= n; i++) {
        px(x0 + (n ? (x1 - x0) * i / n : 0), y0 + (n ? (y1 - y0) * i / n : 0), c);
    }
}

/* 3x5 particles: 0 1 ? ! Z */
enum { G_0, G_1, G_Q, G_BANG, G_Z };
static const uint8_t GLY[5][5] = { { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 3, 0, 2 }, { 2, 2, 2, 0, 2 }, { 7, 1, 2, 4, 7 } };

static void glyph(int x, int y, int g, uint8_t c)
{
    for (int r = 0; r < 5; r++) {
        for (int b = 0; b < 3; b++) {
            if (GLY[g][r] & (4 >> b)) {
                px(x + b, y + r, c);
            }
        }
    }
}

/* Brows are part of the pose library now (shodan_poses.h); the old dark-bar helper is gone. */

/* A stipple of the state's glow on the skin around the eyes. */
static void eye_bloom(float k, uint8_t c)
{
    static const int cx[2] = { 24, 39 };
    for (int e = 0; e < 2; e++) {
        for (int dy = -5; dy <= 5; dy++) {
            for (int dx = -8; dx <= 8; dx++) {
                float d2 = (float)(dx * dx) / 2.6f + (float)(dy * dy);
                int x = cx[e] + dx, y = 33 + dy;
                if (d2 < 30.0f && ((x + y) & 1) == 0 && BAYER4[y & 3][x & 3] < (int)((1.0f - d2 / 30.0f) * 16.0f * k)) {
                    uint8_t cur = s_fb[y * W + x];
                    /* Guard the palette read: the light pass paints dynamic indices
                     * (>= SPR_N) into s_fb; SPR_PAL has only SPR_N entries. */
                    if (cur == IDX_SOCKET || cur == IDX_SKIN || (cur < SPR_N && SPR_PAL[cur][0] > 120)) {
                        s_fb[y * W + x] = c;
                    }
                }
            }
        }
    }
}

/* An energy field around the face: a dotted ellipse that pulses. */
static void halo(float t, float k, uint8_t c)
{
    float pulse = sinf(t * 7.0f) * 1.2f;
    for (int i = 0; i < 360; i += 3) {
        float a = (float)i * TAU / 360.0f;
        int x = iround(31.5f + cosf(a) * (17.0f + pulse)), y = iround(33.0f + sinf(a) * (23.0f + pulse));
        if (BAYER4[(i / 3) & 3][((i / 3) >> 2) & 3] < (int)(k * 16.0f)) {
            px(x, y, c);
        }
    }
}

/* A crackle of lightning from each eye, redrawn every few frames so it jumps. */
static void lightning(float t, uint8_t c1, uint8_t c2)
{
    int seed = (int)(t * 14.0f);
    for (int side = -1; side <= 1; side += 2) {
        int x = 31 + side * 8, y = 33;
        for (int seg = 0; seg < 4; seg++) {
            int nx = x + side * (3 + (int)(hash2(seed, seg + side) % 3u));
            int ny = y + (int)(hash2(seed + 7, seg * 3 + side) % 7u) - 3;
            ln(x, y, nx, ny, c1);
            px(nx, ny + 1, c2);
            x = nx;
            y = ny;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Effects
 * ------------------------------------------------------------------------- */

static void glitch(int bands, int max_shift, int y_lo, int y_hi)
{
    uint8_t tmp[W];
    for (int b = 0; b < bands; b++) {
        int y0 = y_lo + (int)(irand() % (uint32_t)(y_hi - y_lo));
        int h = 1 + (int)(irand() % 4);
        int sh = (1 + (int)(irand() % (uint32_t)max_shift)) * ((irand() & 1) ? 1 : -1);
        for (int y = y0; y < y0 + h && y < H; y++) {
            uint8_t *row = &s_fb[y * W];
            memset(tmp, IDX_BLACK, W);
            for (int x = 0; x < W; x++) {
                int nx = x + sh;
                if ((unsigned)nx < W) {
                    tmp[nx] = row[x];
                }
            }
            memcpy(row, tmp, W);
        }
    }
}

static void shift_rows(int dy)
{
    if (dy > 0) {
        memmove(&s_fb[dy * W], &s_fb[0], (size_t)(H - dy) * W);
        memset(&s_fb[0], IDX_BLACK, (size_t)dy * W);
    } else if (dy < 0) {
        memmove(&s_fb[0], &s_fb[-dy * W], (size_t)(H + dy) * W);
        memset(&s_fb[(H + dy) * W], IDX_BLACK, (size_t)(-dy) * W);
    }
}

static void shift_cols(int dx)
{
    uint8_t tmp[W];
    for (int y = 0; y < H; y++) {
        memset(tmp, IDX_BLACK, W);
        for (int x = 0; x < W; x++) {
            int nx = x + dx;
            if ((unsigned)nx < W) {
                tmp[nx] = s_fb[y * W + x];
            }
        }
        memcpy(&s_fb[y * W], tmp, W);
    }
}

/* The screen is round: fade the corners out so the square has no edges. */
static EXT_RAM_BSS_ATTR uint8_t s_vig[W * H];   /* PSRAM */

static void vignette(void)
{
    static bool init;
    if (!init) {
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                float dx = (float)x - 31.5f, dy = (float)y - 31.5f;
                float d = sqrtf(dx * dx + dy * dy);
                s_vig[y * W + x] = (uint8_t)(clampf((32.5f - d) / 3.5f, 0, 1) * 16.0f + 0.5f);
            }
        }
        init = true;
    }
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t k = s_vig[y * W + x];
            if (k < 16 && BAYER4[y & 3][x & 3] >= k) {
                s_fb[y * W + x] = IDX_BLACK;
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Scaling to the screen (as the SDK expects)
 * ------------------------------------------------------------------------- */

#define MAP_MAX 512
static EXT_RAM_BSS_ATTR uint8_t s_map[MAP_MAX];   /* PSRAM */
static int s_size;

void muse_pixel_set_size(int px_size)
{
    s_size = px_size < MAP_MAX ? px_size : MAP_MAX;
    bool grid = s_size >= 3 * W;
    for (int i = 0; i < s_size; i++) {
        int cell = i * W / s_size;
        bool edge = grid && (i + 1) * W / s_size != cell;
        s_map[i] = (uint8_t)(cell | (edge ? 0x80 : 0));
    }
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    const uint8_t *xmap = &s_map[x0];
    const uint16_t *prev = NULL;
    uint8_t prev_m = 0;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        uint8_t m = s_map[y];
        if (prev && m == prev_m) {
            memcpy(dst, prev, n * sizeof(uint16_t));
            continue;
        }
        const uint8_t *row = &s_fb[(m & 0x7f) * W];
        if (m & 0x80) {
            for (int i = 0; i < n; i++) {
                dst[i] = s_pal_dim[row[xmap[i] & 0x7f]];
            }
        } else {
            for (int i = 0; i < n; i++) {
                uint8_t xm = xmap[i];
                uint8_t c = row[xm & 0x7f];
                dst[i] = xm & 0x80 ? s_pal_dim[c] : s_pal[c];
            }
        }
        prev = dst;
        prev_m = m;
    }
}

/* ---------------------------------------------------------------------------
 * One frame
 * ------------------------------------------------------------------------- */


/* Agent-switch transition: tears the frame in sideways bands and sprinkles it with noise; amp 0..1. */
void muse_pixel_glitch(float amp)
{
    if (amp <= 0.01f) {
        return;
    }
    uint8_t row[W];
    for (int y = 0; y < H; y += 3) {
        int sh = (int)((frand() - 0.5f) * 2.0f * amp * 26.0f);
        if (!sh) {
            continue;
        }
        for (int j = y; j < y + 3 && j < H; j++) {
            uint8_t *r = &s_fb[j * W];
            for (int x = 0; x < W; x++) {
                row[x] = r[((x - sh) % W + W) % W];
            }
            memcpy(r, row, W);
        }
    }
    int n = (int)(amp * 0.10f * W * H);
    for (int i = 0; i < n; i++) {
        s_fb[(int)(frand() * (H - 1)) * W + (int)(frand() * (W - 1))] = (frand() < 0.5f) ? D_ACC : D_WHITE;
    }
    if (frand() < amp * 0.6f) {
        int y = (int)(frand() * (H - 3)), x0 = (int)(frand() * 40), len = 8 + (int)(frand() * 24);
        for (int j = 0; j < 2; j++) {
            for (int x = x0; x < x0 + len && x < W; x++) {
                s_fb[(y + j) * W + x] = D_ACC;
            }
        }
    }
}

/* --- The cross-mood blend (muse_blend.h) ------------------------------------------
 * The faces used to replace the previous mood in one frame. Now every live mood
 * acts on a copy of the mode's own values; the face-pose stack takes each live
 * mood's deltas with its own fading weight, the glow channels blend by weight, and
 * the discrete things (the tremor, the dy nudges, the glitch bursts, the palette's
 * mood override) read from the owning mood. One live mood reproduces the old path
 * exactly (its weight == rk). --- */
static muse_blend_t s_blend;

typedef struct {
    float bright, bloom;          /* glow channels */
    float hovr;                   /* the pleased lift (joins the pet response) */
    int   dy;                     /* the mood's discrete nudge, rows */
    int   shake;                  /* the mood's tremor */
    bool  sh_ovr;                 /* the tremor above is the mood's, not the mode's */
    int   nface;                  /* the mood's face poses (weighted deltas) */
    int   face[3];
    float facew[3];
} shodan_act_t;

#define SHN_FACE(a, id, w) do { (a)->face[(a)->nface] = (id); (a)->facew[(a)->nface] = (w); (a)->nface++; } while (0)

/* One mood's acting, over the mode's own values (the old per-reaction switches).
 * ren is the mood's render weight: rk for a lone mood, falling while it hands
 * over -- so the pose deltas and the acted bits dissolve with it. */
static void shodan_act_mood(shodan_act_t *a, int rid, float rt, float ren, float t)
{
    switch (rid) {
    case R_SMIRK:
        a->bright = 1.0f; a->bloom = 0.6f; SHN_FACE(a, P_SMIRK, ren);
        break;
    case R_SQUINT:
        a->bright = 0.5f; SHN_FACE(a, P_SQUINT, ren);
        break;
    case R_SURPRISED:
        a->bright = 1.0f; a->bloom = 0.8f; SHN_FACE(a, P_SURPRISED, ren);
        if (rt < 0.35f) {
            a->dy -= 2;
        }
        break;
    case R_PLEASED:
        a->hovr = ren; a->bloom = 0.6f; SHN_FACE(a, P_PLEASED, ren);
        break;
    case R_ANGRY:
        a->bright = 1.0f; a->bloom = 1.0f; SHN_FACE(a, P_ANGRY, ren);
        if (rt < 0.6f) { a->shake = ((int)(t * 30.0f)) & 1 ? 1 : -1; a->sh_ovr = true; }
        break;
    case R_EVIL:
        a->bright = 1.0f; a->bloom = 1.0f; SHN_FACE(a, P_EVIL, ren);
        if (rt < 0.6f) { a->shake = ((int)(t * 30.0f)) & 1 ? 1 : -1; a->sh_ovr = true; }
        break;
    case R_SCAN: {
        a->bright = 0.9f; a->bloom = 0.5f;
        float gy = -0.7f + 1.4f * (rt / REACTIONS[R_SCAN].dur);
        SHN_FACE(a, P_UP, clampf(-gy, 0, 1) * 0.9f * ren);
        SHN_FACE(a, P_DOWN, clampf(gy, 0, 1) * 0.9f * ren);
        break;
    }
    case R_GLITCH:
        if (((int)(t * 20.0f)) % 3 == 0) {
            SHN_FACE(a, P_CLOSED, 1.0f);   /* the eyelid flicker stays a deliberate hard cut */
        }
        break;
    case R_LAUGH:
        SHN_FACE(a, sinf(t * 26.0f) > -0.2f ? P_LAUGH : P_PLEASED, ren);
        a->dy += ((int)(t * 11.0f)) & 1 ? -1 : 0;
        break;
    case R_WINK: {
        a->bright = 1.0f; a->bloom = 0.7f;
        float ww = clampf(fminf((rt - 0.25f) / 0.08f, (0.95f - rt) / 0.08f), 0, 1);
        SHN_FACE(a, P_WINK, ww * ren);
        break;
    }
    case R_SLEEPY: {
        a->bright = 0.3f;
        SHN_FACE(a, P_SLEEPY, clampf(rt / 1.2f, 0, 1) * ren);
        float wp = clampf((rt - 1.8f) / 1.2f, 0, 0.85f);
        SHN_FACE(a, P_CLOSED, wp * ((ren >= 1.0f || rt < 1.0f) ? 1.0f : ren));
        if (rt > 0.8f) {
            a->dy += 1 + (rt > 2.0f ? 1 : 0);
        }
        break;
    }
    case R_CONFUSED:
        SHN_FACE(a, ((int)(t * 2.6f) & 1) ? P_CONFUSED_L : P_CONFUSED_R, ren);
        break;
    case R_BORED:
        a->bright = 0.4f; SHN_FACE(a, P_BORED, ren);
        a->dy += 1;
        break;
    case R_DISGUST:
        a->bright = 0.7f; SHN_FACE(a, P_DISGUST, ren);
        break;
    case R_NERVOUS:
        SHN_FACE(a, ((int)(t * 7.0f) & 1) ? P_NERVOUS_L : P_NERVOUS_R, ren);
        a->shake = ((int)(t * 40.0f)) & 1 ? 1 : 0; a->sh_ovr = true;
        break;
    case R_PROUD:
        a->bright = 1.0f; a->bloom = 0.5f; SHN_FACE(a, P_PROUD, ren);
        a->dy -= 1;
        break;
    case R_POWERUP:
        a->bright = 1.0f; a->bloom = clampf(rt * 1.2f, 0, 1);
        SHN_FACE(a, P_TARGET, clampf(rt * 2.0f, 0, 1) * ren);
        break;
    case R_TARGET:
        a->bright = 1.0f; SHN_FACE(a, P_TARGET, ren);
        break;
    case R_ALERT:
        SHN_FACE(a, P_ALERT, ren);
        if (rt < 0.6f) { a->shake = ((int)(t * 30.0f)) & 1 ? 1 : -1; a->sh_ovr = true; }
        break;
    default:
        break;
    }
}

/* The palette's mood override: which scheme a mood pushes the face toward (the
 * mode's own, if the mood has no override). The old if/else chain, per mood. */
static const scheme_t *shodan_scheme_mood(int rid, float rt)
{
    if (rid == R_ANGRY || rid == R_EVIL || (rid == R_ALERT && (((int)(rt * 4.0f)) & 1) == 0) ||
        (rid == R_TARGET && rt > 1.3f)) {
        return &SCHEMES[MUSE_MODE_ERROR];
    }
    if (rid == R_DISGUST) {
        return &SCHEMES[MUSE_MODE_SPEAKING];           /* a sickly yellow-green */
    }
    if (rid == R_SLEEPY) {
        return &SCHEMES[MUSE_MODE_OFF];                /* the light dims */
    }
    if (rid == R_HACKED && (((int)(rt * 9.0f)) % 5) == 0) {
        return &SCHEMES[MUSE_MODE_THINKING];           /* a violet flicker */
    }
    return NULL;
}

void muse_pixel_render(const muse_pose_t *p)
{
    if (!s_eyes_init) {
        init_eyes();
    }
    float dt = s_idle.last_t > 0 ? clampf(p->t - s_idle.last_t, 0, 0.2f) : 0.04f;
    s_idle.last_t = p->t;

    muse_mode_t mode = p->mode;
    float t = p->t, level = clampf(p->level, 0, 1);
    float happy = mode == MUSE_MODE_ERROR ? 0 : p->happy;
    int rq = s_react_req;
    if (rq) {
        s_react_req = 0;
        muse_blend_fire(&s_blend, rq, REACTIONS[rq].dur);
    }
    muse_blend_step(&s_blend, dt);
    /* The owning mood drives what cannot blend (the palette's mood override and the
     * discrete bursts); the weighted layers read every live slot. */
    int top = muse_blend_top(&s_blend);
    int rid = top >= 0 ? s_blend.s[top].id : R_NONE;
    /* The palette's mood override rides the blend too: every live mood's scheme (the
     * old if/else chain, shodan_scheme_mood) mixes in by weight, so the face's whole
     * look slides between two moods instead of stepping. One mood lands exactly on
     * its scheme; the palette's own ease below still smooths the remainder. */
    scheme_t st = SCHEMES[mode];
    {
        float ws = 0.0f, acc[5][3] = {{ 0 }}, f_hue = 0, f_sat = 0, f_val = 0, f_spread = 0;
        for (int si = 0; si < s_blend.n; si++) {
            const muse_blend_slot_t *sl = &s_blend.s[si];
            if (sl->w <= MUSE_BLEND_EPS) {
                continue;
            }
            const scheme_t *sc = shodan_scheme_mood(sl->id, sl->age);
            if (!sc) {
                sc = &SCHEMES[mode];
            }
            uint32_t cols[5] = { sc->f[0], sc->f[1], sc->f[2], sc->f[3], sc->acc };
            for (int k = 0; k < 5; k++) {
                acc[k][0] += sl->w * (float)((cols[k] >> 16) & 255);
                acc[k][1] += sl->w * (float)((cols[k] >> 8) & 255);
                acc[k][2] += sl->w * (float)(cols[k] & 255);
            }
            f_hue += sl->w * sc->hue;
            f_sat += sl->w * sc->sat;
            f_val += sl->w * sc->val;
            f_spread += sl->w * sc->spread;
            ws += sl->w;
        }
        if (ws > 0.0f) {
            float S = ws > 1.0f ? ws : 1.0f;
            uint32_t cols[5] = { st.f[0], st.f[1], st.f[2], st.f[3], st.acc };
            for (int k = 0; k < 5; k++) {
                int c[3];
                for (int ch = 0; ch < 3; ch++) {
                    float b = (float)((cols[k] >> (16 - ch * 8)) & 255);
                    float v = b + (acc[k][ch] - b * ws) / S;
                    c[ch] = v < 0.0f ? 0 : (v > 255.0f ? 255 : iround(v));
                }
                uint32_t nc = ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[2];
                if (k < 4) {
                    st.f[k] = nc;
                } else {
                    st.acc = nc;
                }
            }
            st.hue = st.hue + (f_hue - st.hue * ws) / S;
            st.sat = st.sat + (f_sat - st.sat * ws) / S;
            st.val = st.val + (f_val - st.val * ws) / S;
            st.spread = st.spread + (f_spread - st.spread * ws) / S;
        }
    }
    update_palette(&st, dt);
    /* T3.2: beats only start while no mood is in charge and no mode owns the face; a
     * REACT cancels any beat in flight (see muse_idle.h for the priority rules). A
     * mood that is fading out no longer cancels them -- its props are leaving. */
    bool allow_ev = !muse_blend_busy(&s_blend) &&
                    (mode == MUSE_MODE_IDLE || mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_THINKING);
    float close = look_update(p, dt, allow_ev);
    s_spk = muse_ease(s_spk, mode == MUSE_MODE_SPEAKING ? clampf(level * 1.3f, 0, 1) : 0.0f, dt, 0.14f);
    if (mode != MUSE_MODE_SPEAKING) {
        s_lip.state = 0;
        s_lip.hold = 0;
    }

    /* Per-mode parameters. */
    float open = 0.8f, bright = 0.6f, pulse = 0.35f, mouth = 0;
    bool xeye = false;
    int shake = 0;
    switch (mode) {
    case MUSE_MODE_LISTENING:
        open = 1.0f;
        bright = 0.9f;
        pulse = 0.55f + level * 0.8f;
        break;
    case MUSE_MODE_THINKING:
        open = 0.55f;
        pulse = 0.9f;
        bright = 0.7f;
        break;
    case MUSE_MODE_SPEAKING:
        open = 0.95f;
        bright = 1.0f;
        pulse = 0.7f + level * 1.2f;
        mouth = muse_lip(&s_lip, level, dt);   /* 3-state lip-sync: hysteresis + min-hold */
        break;
    case MUSE_MODE_ERROR:
        xeye = (((int)(t * 11.0f)) % 5) != 0;
        shake = p->mode_t < 0.6f ? (((int)(t * 30.0f)) & 1 ? 2 : -2) : 0;
        pulse = 1.4f;
        open = 0.9f;
        bright = 1.0f;
        break;
    default:
        break;
    }
    float bloom = 0;
    /* The cross-mood blend (muse_blend.h): every live mood acts on a copy of the
     * mode's own values; the glow channels blend by weight (w/sum(w) with the sum
     * held at >= 1, so one mood lands at full weight and the last one out ramps
     * back to the mode's own acting), the face-pose stack takes each live mood's
     * deltas with its own fading weight, and the discrete bits (the tremor, the dy
     * nudges) come from the owning mood. */
    shodan_act_t a0 = { .bright = bright, .bloom = 0.0f };
    float wsum = 0, c_bright = 0, c_bloom = 0, c_dy = 0;
    int face[6], nf = 0;
    float facew[6];
    for (int si = 0; si < s_blend.n; si++) {
        const muse_blend_slot_t *sl = &s_blend.s[si];
        if (sl->w <= MUSE_BLEND_EPS) {
            continue;
        }
        shodan_act_t a = a0;
        shodan_act_mood(&a, sl->id, sl->age, muse_blend_rend(sl), t);
        c_bright += sl->w * a.bright;
        c_bloom += sl->w * a.bloom;
        c_dy += sl->w * (float)a.dy;
        wsum += sl->w;
        for (int pi = 0; pi < a.nface && nf < 6; pi++) {
            face[nf] = a.face[pi];
            facew[nf] = a.facew[pi];
            nf++;
        }
    }
    shodan_act_t atop = a0;
    if (top >= 0) {
        shodan_act_mood(&atop, s_blend.s[top].id, s_blend.s[top].age, muse_blend_rend(&s_blend.s[top]), t);
    }
    float dy_mood = 0.0f;
    if (wsum > 0.0f) {
        float S = wsum > 1.0f ? wsum : 1.0f;
        bright = a0.bright + (c_bright - a0.bright * wsum) / S;
        bloom = a0.bloom + (c_bloom - a0.bloom * wsum) / S;
        dy_mood = (c_dy - (float)a0.dy * wsum) / S;
    }
    if (atop.hovr > happy) {
        happy = atop.hovr;     /* R_PLEASED: the mood's own lift joins the pet response */
    }
    if (happy > 0) {
        bright = 1.0f;
        pulse += happy;
    }
    float boot = mode == MUSE_MODE_BOOT ? clampf(p->mode_t / 1.5f, 0, 1) : 1.0f;
    float off = mode == MUSE_MODE_OFF ? clampf(p->mode_t / 1.3f, 0, 1) : 0.0f;
    if (mode == MUSE_MODE_BOOT) {
        open = clampf((p->mode_t - 0.9f) / 0.25f, 0, 1);
        bright = p->mode_t > 0.9f && p->mode_t < 1.25f ? 1.0f : bright;
    }
    if (mode == MUSE_MODE_OFF) {
        open = clampf(1.0f - p->mode_t / 0.45f, 0, 1);
    }

    /* T3.1 rig: the glow eases and the mouth presence dissolves in and out; the
     * reaction poses already ramp with rk. open stays raw (boot/off quantize the
     * lid already). */
    muse_rig_t rt_tgt = { .open = open, .widen = mode == MUSE_MODE_LISTENING ? 1.0f : 0.0f,
                          .bright = bright, .bloom = bloom,
                          .mouth = mouth > 0.22f ? 1.0f : 0.0f };
    if (!s_rig_init) {
        s_rig = rt_tgt;
        s_rig_init = true;
    }
    muse_rig_step(&s_rig, &rt_tgt, dt);
    bright = s_rig.bright;
    bloom = s_rig.bloom;

    /* The face poses come from the blend above (each live mood's deltas, weighted). */

    memcpy(s_fb, SPR, sizeof(s_fb));
    animate_light(t, 0.45f + pulse * 0.9f, level);
    if (bloom > 0.02f) {
        eye_bloom(bloom, D_G3);   /* the rig leaves a residue; below this nothing paints */
    }
    if (xeye) {
        draw_xeyes();
    } else if (nf == 0 && wsum == 0.0f) {
        /* Idle-style face: the scheduler owns the gaze, the blinks and the beats. */
        float evk = muse_idle_ev_k(&s_idle, t);
        float wdw = s_rig.widen;
        if (s_idle.ev == MUSE_IDLE_EV_WIDEN) {
            wdw = fmaxf(wdw, evk);             /* a brief "noticed something" */
        }
        if (wdw > 0.02f) {
            pose_apply(P_WIDE, wdw);           /* widens in, relaxes out (rig) */
        }
        if (mode == MUSE_MODE_THINKING) {
            pose_apply(P_HALF, 0.55f);
            pose_apply(P_UP, clampf(-s_idle.gy, 0, 1));
            pose_apply(s_idle.gx > 0 ? P_RIGHT : P_LEFT, clampf(fabsf(s_idle.gx) * 0.8f, 0, 1));
        } else if (mode != MUSE_MODE_LISTENING) {
            pose_apply(P_RIGHT, clampf(s_idle.gx, 0, 1) * 0.9f);
            pose_apply(P_LEFT, clampf(-s_idle.gx, 0, 1) * 0.9f);
            pose_apply(P_DOWN, clampf(s_idle.gy, 0, 1) * 0.9f);
            pose_apply(P_UP, clampf(-s_idle.gy, 0, 1) * 0.9f);
        }
        if (s_idle.ev == MUSE_IDLE_EV_SQUINT) {
            pose_apply(P_SQUINT, evk * 0.9f);
        } else if (s_idle.ev == MUSE_IDLE_EV_SMILE) {
            pose_apply(P_SMIRK, evk * 0.4f);    /* a faint smirk, not the full act */
        }
        float shut = close;
        if (mode == MUSE_MODE_SPEAKING) {
            shut = fmaxf(shut, s_spk * 0.45f);  /* squint-on-speech: lids ride the voice */
        }
        if (mode == MUSE_MODE_BOOT || mode == MUSE_MODE_OFF) {
            shut = fmaxf(close, 1.0f - open);   /* boot/off: open is eyes_open */
        }
        pose_apply(P_CLOSED, shut);
        if (s_idle.ev == MUSE_IDLE_EV_WINK) {
            pose_apply(P_WINK, evk);            /* an idle wink beat */
        }
        if (happy > 0) {
            pose_apply(P_PLEASED, happy);
        }
    } else {
        for (int i = 0; i < nf; i++) {
            pose_apply(face[i], facew[i]);
        }
    }
    if (mode == MUSE_MODE_SPEAKING) {
        pose_apply(mouth > 0.9f ? P_MOUTH_2 : P_MOUTH_1, s_rig.mouth);
    }
    if (!xeye) {
        pose_eye_glow(bright);
    }
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING) {
        draw_wave(t, level);
    }
    /* The mood decorations: every live mood draws its own bits, fading with the
     * blend (the cases that carry an rk weight take the mood's render weight). */
    int tick = (int)(t * 12.0f);
    for (int si = 0; si < s_blend.n; si++) {
        const muse_blend_slot_t *sl = &s_blend.s[si];
        if (sl->w <= MUSE_BLEND_EPS) {
            continue;
        }
        float rt = sl->age;
        float rk = muse_blend_rend(sl);
        switch (sl->id) {
        case R_SMIRK:
            halo(t, 0.25f * rk, D_G3);
            break;
        case R_SCAN: {                                         /* a sweep down the face, mapping it as it goes */
            int y = 12 + (int)(rt / REACTIONS[R_SCAN].dur * 42.0f);
            for (int yy = 12; yy < y - 2; yy += 3) {
                for (int x = 17; x <= 46; x++) {
                    /* Guard the palette read: s_fb can hold a dynamic index (D_G2,
                     * written by this very sweep on an earlier frame), which is past
                     * the end of the SPR_N-entry SPR_PAL table. */
                    uint8_t u = s_fb[yy * W + x];
                    if (((x + yy) & 1) == 0 && u < SPR_N && SPR_PAL[u][0] > 120) {
                        s_fb[yy * W + x] = D_G2;               /* scanned rows keep a faint grid */
                    }
                }
            }
            for (int x = 12; x <= 52; x++) {
                px(x, y, D_WHITE);
                px(x, y - 1, D_G0);
                px(x, y - 2, D_G2);
            }
            if (rt > 0.3f && ((tick & 3) == 0)) {
                glyph(48 + (tick & 1) * 3, y - 6, (tick >> 1) & 1 ? G_0 : G_1, D_G1);
            }
            break;
        }
        case R_ALERT:
            halo(t, 0.9f, D_G1);
            if ((((int)(rt * 6.0f)) & 1) == 0) {
                glyph(48, 8, G_BANG, D_WHITE);
            }
            break;
        case R_GLITCH:
            for (int k = 0; k < 5; k++) {                      /* binary digits spilling through */
                glyph(4 + (int)(hash2(tick, k) % 54u), 4 + (int)(hash2(tick + 3, k) % 50u), hash2(k, tick) & 1, k & 1 ? D_G0 : D_G2);
            }
            break;
        case R_SQUINT:
            if (((int)(rt * 3.0f)) & 1) {
                glyph(49, 14, G_Q, D_G1);
            }
            break;
        case R_ANGRY:
            halo(t, 0.9f, D_G1);
            if (tick & 1) {
                lightning(t, D_WHITE, D_G0);
            }
            break;
        case R_EVIL:
            halo(t, 1.0f, D_G1);
            if (tick & 1) {
                lightning(t, D_G0, D_WHITE);
            }
            break;
        case R_SURPRISED:
            glyph(49, 9, G_BANG, D_WHITE);
            break;
        case R_PLEASED:
            halo(t, 0.5f * rk, D_G1);
            if (tick & 1) {                                    /* twinkles by the eyes */
                px(21, 30, D_WHITE); px(20, 31, D_G0); px(22, 31, D_G0); px(21, 32, D_G0);
                px(42, 30, D_WHITE); px(41, 31, D_G0); px(43, 31, D_G0); px(42, 32, D_G0);
            }
            break;
        case R_LAUGH:
            halo(t, (tick & 1) ? 0.5f : 0.1f, D_G2);
            break;
        case R_WINK:
            if (rt > 0.3f && rt < 0.9f) {                      /* a spark by the open eye */
                px(44, 30, D_WHITE); px(43, 30, D_G0); px(45, 30, D_G0); px(44, 29, D_G0); px(44, 31, D_G0);
            }
            break;
        case R_SLEEPY:
            for (int k = 0; k < 3; k++) {                      /* Zs drifting up */
                int y = 24 - (int)fmodf(rt * 6.0f + (float)k * 8.0f, 22.0f);
                if (y > 2) {
                    glyph(46 + k * 3 + (int)(sinf(rt * 2.0f + (float)k) * 1.5f), y, G_Z, k == 0 ? D_WHITE : D_G0);
                }
            }
            break;
        case R_CONFUSED:
            if (((int)(rt * 4.0f)) & 1) {
                glyph(30, 6, G_Q, D_G0);
            }
            break;
        case R_HACKED:
            for (int x = 1; x < 63; x += 3) {                  /* digits raining down */
                if (hash2(x, 5) % 3u) {
                    continue;
                }
                int head = ((int)(t * (20.0f + (float)(hash2(x, 9) % 20u)) + (float)(hash2(x, 2) % 70u))) % 76 - 8;
                glyph(x, head, hash2(x, tick) & 1, D_G0);
                glyph(x, head - 7, hash2(x, tick + 1) & 1, D_G2);
                glyph(x, head - 14, hash2(x, tick + 2) & 1, D_G3);
            }
            break;
        case R_POWERUP:
            for (int k = 0; k < 3; k++) {                      /* rings pouring outward */
                float r = fmodf(rt * 22.0f + (float)k * 9.0f, 30.0f);
                if (r > 8.0f) {
                    for (int i = 0; i < 64; i += 2) {
                        float a2 = (float)i * TAU / 64.0f;
                        px(iround(31.5f + cosf(a2) * r), iround(33.0f + sinf(a2) * r * 0.9f), r < 18.0f ? D_G0 : D_G2);
                    }
                }
            }
            halo(t, clampf(rt * 0.8f, 0, 1), D_G1);
            break;
        case R_BORED:
            for (int k = 0; k < 3; k++) {
                if (rt > 0.5f + (float)k * 0.6f) {
                    px(47 + k * 3, 14, D_G1);
                }
            }
            break;
        case R_DISGUST:
            break;
        case R_TARGET: {                                       /* brackets close in, then lock */
            float k = clampf(rt / 1.2f, 0, 1);
            int sz = 28 - (int)(k * 19.0f);
            int cx = 31, cy = 33;
            uint8_t c = rt > 1.3f ? D_G0 : D_WHITE;
            for (int sx = -1; sx <= 1; sx += 2) {
                for (int sy = -1; sy <= 1; sy += 2) {
                    int x = cx + sx * sz, y = cy + sy * sz;
                    ln(x, y, x - sx * 5, y, c);
                    ln(x, y, x, y - sy * 5, c);
                }
            }
            if (rt > 1.2f && (((int)(rt * 8.0f)) & 1) == 0) {
                ln(cx - 14, cy, cx - 6, cy, D_WHITE); ln(cx + 6, cy, cx + 14, cy, D_WHITE);
                ln(cx, cy - 14, cx, cy - 6, D_WHITE); ln(cx, cy + 6, cx, cy + 14, D_WHITE);
            }
            break;
        }
        case R_NERVOUS: {                                      /* a bead of sweat */
            int y = 22 + (int)fmodf(rt * 9.0f, 20.0f);
            px(45, y, D_G0); px(44, y + 1, D_G0); px(45, y + 1, D_WHITE); px(46, y + 1, D_G0); px(45, y + 2, D_G0);
            break;
        }
        case R_PROUD:
            halo(t * 0.5f, 0.35f + 0.3f * sinf(t * 3.0f), D_G1);
            break;
        default:
            break;
        }
        if (sl->id == R_GLITCH && (tick & 3) == 0) {           /* a split image: ghosts of the eyes */
            for (int e = 0; e < 2; e++) {
                for (int i = 0; i < s_eye[e].n; i++) {
                    px(s_eye[e].x[i] + 2, s_eye[e].y[i], D_G3);
                    px(s_eye[e].x[i] - 2, s_eye[e].y[i], D_WHITE);
                }
            }
        }
    }
    /* A slow breath (the scheduler's wandering bob), a hop when petted, a shake on
     * error. The mood's dy blends by weight and rounds: two moods trading opposite
     * nudges step through zero one pixel at a time instead of jumping. */
    float dyf = (float)iround(muse_idle_breath(&s_idle)) + dy_mood;
    if (happy > 0) {
        dyf -= fabsf(sinf(t * 9.0f)) * 2.5f * happy;
    }
    int dy = iround(dyf);
    if (atop.sh_ovr) {
        shake = atop.shake;    /* the owning mood's tremor replaces the mode's */
    }
    if (dy) {
        shift_rows(dy);
    }
    if (shake) {
        shift_cols(shake);
    }

    /* Signal trouble. */
    if (rid == R_HACKED) {
        glitch(4, 5, 4, 60);
    }
    if (rid == R_GLITCH) {
        glitch(8, 8, 4, 60);
        if (((int)(t * 25.0f)) % 4 == 0) {
            shift_cols(2);
        }
    } else if (rid == R_ANGRY) {
        glitch(2, 3, 6, 58);
    }
    if (mode == MUSE_MODE_ERROR) {
        /* Error is ONE ongoing event, not per-frame noise: each tear opens, strains and
         * knits back over seconds (muse_crack.h) instead of teleporting anew every frame. */
        if (!s_cracks_run) {
            muse_crack_init(&s_cracks, (uint32_t)(frand() * 100000.0f) + 3u,
                            5, 4.0f, 59.0f, 6.0f, 0.30f);
            s_cracks_run = true;
        }
        muse_crack_step(&s_cracks, dt);
        muse_crack_apply(&s_cracks, s_fb, W, H, IDX_BLACK);
    } else if (mode == MUSE_MODE_SPEAKING) {
        glitch(1 + (int)(level * 3.0f), 2 + (int)(level * 2.0f), 6, 58);
    } else if (mode == MUSE_MODE_BOOT) {
        glitch((int)((1.0f - boot) * 6.0f), 5, 4, 60);
    } else if (happy > 0.2f) {
        glitch(2, 3, 6, 58);
    } else if (fmodf(t, 6.3f) < 0.12f) {
        glitch(1, 2, 6, 58);   /* now and then, even at rest */
    }
    if (mode != MUSE_MODE_ERROR) {
        s_cracks_run = false;  /* leaving ERROR: the next entry starts calm */
    }

    if (mode == MUSE_MODE_BOOT && boot < 1.0f) {
        float r = boot * 38.0f;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                float dx = (float)x - 31.5f, dd = (float)y - 31.5f;
                float d = sqrtf(dx * dx + dd * dd);
                if (d > r + 1.0f) {
                    s_fb[y * W + x] = (irand() & 31) == 0 ? D_G2 : IDX_BLACK;
                } else if (d > r - 1.0f) {
                    s_fb[y * W + x] = D_G0;   /* the scan ring */
                }
            }
        }
    }

    if (mode == MUSE_MODE_OFF && off > 0.45f) {
        float k = clampf((off - 0.45f) / 0.55f, 0, 1);
        int vh = iround(32.0f * (1.0f - k) * (1.0f - k));
        for (int y = 0; y < H; y++) {
            if (abs(y - 32) > vh) {
                memset(&s_fb[y * W], IDX_BLACK, W);
            }
        }
        if (vh < 3) {
            int half = iround(32.0f * (1.0f - k * k * k));
            for (int x = 32 - half; x <= 32 + half; x++) {
                px(x, 32, D_G0);
            }
        }
    }
    vignette();
}
