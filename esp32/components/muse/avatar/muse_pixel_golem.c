// Hermes avatar: the Jade Golem (round-6 k4). A rounded stone-and-moss guardian -- jade dome
// head, two big glowing cream-white eyes in dark sockets, bolt-ear bosses, a chest emblem plate
// whose core breathes. Picture is fixed pixel art (sprite_golem.h); what moves is light: the
// body recolours with the state, the moss shimmers, the eyes blink/look/widen, the core beats,
// the mouth slot opens for speech. Picture-based, like SHODAN and the Construct.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "muse_pixel.h"
#include "sprite_golem.h"
#include "golem_moods.h"
#include "muse_rig.h"   /* T3.1 rig: eased expression channels */
#include "muse_idle.h"  /* T3.2 idle scheduler: blink cadence, saccades, micro beats, breath */
#include "muse_crack.h" /* the error state's persistent crack tears */
#include "muse_blend.h" /* the cross-mood blend: live moods cross-fade instead of cutting */
#include "golem_moods.h"   /* pre-baked mood frames (sparse diffs) */

#if defined(__has_include) && __has_include("esp_attr.h")
#include "esp_attr.h"   /* PSRAM placement for the frame buffers */
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR   /* host preview builds: ordinary memory */
#endif

#define W MUSE_PX_W
#define H MUSE_PX_H
#define TAU 6.2831853f

/* Colours past the sprite's own: the state's glow ramp, its accent, white, and the five
 * true-colour arcs the rainbow mood blooms (the only place the state ramp is overridden). */
enum { D_G0 = SPR_N, D_G1, D_G2, D_G3, D_ACC, D_WHITE, D_RB0, D_RB1, D_RB2, D_RB3, D_RB4, D_RB5, D_RB6, D_COOL, N_PAL };

typedef struct {
    uint32_t f[4];
    uint32_t acc;
    float sat, val;    /* how the sprite's live colours are pushed: saturation and value scales */
    float warm_val;    /* extra value on the amber eyes/accents (they should stay warmer/brighter) */
    float hue_k;       /* how strongly the stone's hue rotates toward the state's own hue
                          (0 = the baked gold stays; 1 = fully the state's hue) */
} scheme_t;

/* Idle is living jade-green; listening cyan; thinking violet; speaking warm gold; error red; off dim.
 * The ramp recolours the golem's moss/jade family; the cream lens keeps a bright lift of its own. */
/* Gold-tuned: idle/speak keep the baked gold warm; listen cools it toward teal, think pushes
 * the stone toward amethyst, error heats it red, boot is a brief cool forge-light, off grey. */
static const scheme_t SCHEMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT]      = { { 0xfff0d0, 0xffc86a, 0xcf8a2a, 0x6a4a14 }, 0xffc050, 1.00f, 1.05f, 1.10f, 0.15f },
    [MUSE_MODE_IDLE]      = { { 0xfff0c0, 0xffd24a, 0xd8982a, 0x8a5f18 }, 0xffb840, 1.05f, 1.02f, 1.05f, 0.00f },
    [MUSE_MODE_LISTENING] = { { 0xd8f4f0, 0x6fd8d0, 0x2f9a98, 0x1a5f68 }, 0x4fdcd4, 1.08f, 1.06f, 0.85f, 0.60f },
    [MUSE_MODE_THINKING]  = { { 0xf0e6f4, 0xb08cf0, 0x7f4fc8, 0x402a78 }, 0x9a70e0, 1.08f, 1.02f, 0.70f, 0.80f },
    [MUSE_MODE_SPEAKING]  = { { 0xfff4d0, 0xffd24a, 0xd8982a, 0x8a5f18 }, 0xffb03c, 1.10f, 1.08f, 1.15f, 0.05f },
    [MUSE_MODE_ERROR]     = { { 0xf4dcd4, 0xd86f5a, 0xa83828, 0x5a1a14 }, 0xe05040, 1.28f, 1.02f, 0.30f, 0.90f },
    [MUSE_MODE_OFF]       = { { 0xa8b0ac, 0x6f7a74, 0x4a544e, 0x2a302c }, 0x5a6a60, 0.35f, 0.45f, 0.40f, 0.35f },
};

static float s_sat = 1.0f, s_val = 1.0f, s_warmv = 1.0f, s_hue_k = 0.0f;
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

static uint8_t pxat(int x, int y)
{
    if ((unsigned)x >= W || (unsigned)y >= H) {
        return IDX_BLACK;
    }
    return s_fb[y * W + x];
}

/* 3x5 particles: 0 1 ? ! Z */
enum { G_0, G_1, G_Q, G_BANG, G_Z };

/* A stipple of the state's glow on the visor around the eyes. */
static void eye_bloom(float k, uint8_t c)
{
    static const int cx[2] = { 20, 43 };
    for (int e = 0; e < 2; e++) {
        for (int dy = -5; dy <= 5; dy++) {
            for (int dx = -8; dx <= 8; dx++) {
                float d2 = (float)(dx * dx) / 2.6f + (float)(dy * dy);
                int x = cx[e] + dx, y = 23 + dy;
                if (d2 < 22.0f && ((x + y) & 1) == 0 && BAYER4[y & 3][x & 3] < (int)((1.0f - d2 / 22.0f) * 16.0f * k)) {
                    uint8_t cur = s_fb[y * W + x];
                    if (cur == IDX_SOCKET || cur == IDX_SKIN || (cur < SPR_N && SPR_PAL[cur][0] > 110)) {
                        s_fb[y * W + x] = c;   /* guard: dynamic idx (>= SPR_N) has no SPR_PAL row */
                    }
                }
            }
        }
    }
}

/* A ring that ripples with the voice, in place of the dotted rings. */
/* Sound: rings ripple out from the chest core (the golem's voice organ), fading with distance.
 * Listening: slow tight rings around the core. Speaking: faster, stronger rings driven by voice. */
static void draw_wave(float t, float level)
{
    float lv = level < 0.12f ? 0.12f : level;
    static const int cx = 31, cy = 54;
    for (int ring = 0; ring < 3; ring++) {
        float ph = fmodf(t * (2.2f + lv * 2.5f) + (float)ring / 3.0f, 1.0f);
        float r = 4.0f + ph * (lv * 16.0f + 10.0f);
        float fade = (1.0f - ph) * (0.35f + 0.65f * lv);
        for (int i = 0; i < 110; i++) {
            float a = (float)i * TAU / 110.0f;
            int x = iround(cx + cosf(a) * r), y = iround(cy + sinf(a) * r * 0.8f);
            if (x < 2 || x > 61 || y < 2 || y > 61) {
                continue;                                /* the rings stay on the disc */
            }
            uint8_t cur = pxat(x, y);
            uint8_t c = (i & 1) ? D_G1 : D_G0;
            /* Guard: a dynamic index (>= SPR_N) is glow already, not stone -- skip it. */
            if (cur < SPR_N && SPR_PAL[cur][1] > 60 && SPR_PAL[cur][1] >= SPR_PAL[cur][0]) {
                continue;                                /* don't paint over the golem itself */
            }
            if (BAYER4[y & 3][x & 3] < (int)(fade * 16.0f)) {
                px(x, y, c);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Reactions: short acted moods the relay can trigger (the [react:name] tag)
 * ------------------------------------------------------------------------- */

enum {
    R_NONE, R_SMIRK, R_SCAN, R_ALERT, R_GLITCH, R_SQUINT, R_ANGRY, R_SURPRISED, R_PLEASED,
    R_LAUGH, R_WINK, R_SLEEPY, R_CONFUSED, R_EVIL, R_HACKED, R_POWERUP, R_BORED, R_DISGUST, R_TARGET,
    R_NERVOUS, R_PROUD,
    /* the moods the relay lists beyond the 20 (Bloop had them too) */
    R_EXCITED, R_CONFETTI, R_SUNNY, R_RAIN, R_STORM, R_COLD, R_RAINBOW,
    /* the work family (13): tools and tasks the golem acts out */
    R_SEARCHING, R_BROWSING, R_READING, R_WRITING, R_CHECKING, R_CALCULATING, R_CALENDAR,
    R_MONITORING, R_GENERATING, R_RUNNING, R_INVOKING, R_DELEGATING, R_REMEMBERING,
    /* feelings (8) */
    R_SHRUG, R_EMBARRASSED, R_SCARED, R_COOL, R_MIND_BLOWN, R_CRYING_LAUGHING, R_SAD, R_NOD,
    /* weather (4) that suits the stone */
    R_CLOUDY, R_WINDY, R_HOT, R_SNOW,
    R_COUNT
};

static const struct { const char *name; float dur; } REACTIONS[R_COUNT] = {
    [R_SMIRK] = { "smirk", 2.8f },       [R_SCAN] = { "scan", 2.6f },         [R_ALERT] = { "alert", 2.2f },
    [R_GLITCH] = { "glitch", 1.8f },     [R_SQUINT] = { "squint", 2.6f },     [R_ANGRY] = { "angry", 2.6f },
    [R_SURPRISED] = { "surprised", 1.8f }, [R_PLEASED] = { "pleased", 2.6f }, [R_LAUGH] = { "laugh", 2.4f },
    [R_WINK] = { "wink", 1.6f },         [R_SLEEPY] = { "sleepy", 3.4f },     [R_CONFUSED] = { "confused", 2.6f },
    [R_EVIL] = { "evil", 3.0f },         [R_HACKED] = { "hacked", 2.4f },     [R_POWERUP] = { "powerup", 2.4f },
    [R_BORED] = { "bored", 3.2f },       [R_DISGUST] = { "disgust", 2.4f },   [R_TARGET] = { "target", 2.6f },
    [R_NERVOUS] = { "nervous", 2.6f },   [R_PROUD] = { "proud", 2.8f },
    [R_EXCITED] = { "excited", 2.8f },   [R_CONFETTI] = { "confetti", 3.4f }, [R_SUNNY] = { "sunny", 3.0f },
    [R_RAIN] = { "rain", 3.2f },         [R_STORM] = { "storm", 3.2f },       [R_COLD] = { "cold", 3.0f },
    [R_RAINBOW] = { "rainbow", 3.2f },
    [R_SEARCHING] = { "searching", 2.6f },   [R_BROWSING] = { "browsing", 3.0f },   [R_READING] = { "reading", 3.0f },
    [R_WRITING] = { "writing", 3.0f },       [R_CHECKING] = { "checking", 2.4f },   [R_CALCULATING] = { "calculating", 3.0f },
    [R_CALENDAR] = { "calendar", 3.0f },     [R_MONITORING] = { "monitoring", 2.8f }, [R_GENERATING] = { "generating", 3.0f },
    [R_RUNNING] = { "running", 3.0f },       [R_INVOKING] = { "invoking", 2.8f },   [R_DELEGATING] = { "delegating", 2.8f },
    [R_REMEMBERING] = { "remembering", 2.8f },
    [R_SHRUG] = { "shrug", 2.6f },           [R_EMBARRASSED] = { "embarrassed", 2.8f }, [R_SCARED] = { "scared", 2.6f },
    [R_COOL] = { "cool", 2.8f },             [R_MIND_BLOWN] = { "mind_blown", 2.8f },   [R_CRYING_LAUGHING] = { "crying_laughing", 2.8f },
    [R_SAD] = { "sad", 3.0f },               [R_NOD] = { "nod", 2.4f },
    [R_CLOUDY] = { "cloudy", 3.0f },         [R_WINDY] = { "windy", 3.0f },         [R_HOT] = { "hot", 3.0f },
    [R_SNOW] = { "snow", 3.4f },
};

static volatile int s_react_req;     /* set from any task; the renderer picks it up */

/* The relay's [react:name] (via avatar.react / the Hermes mood list). false if the name is unknown. */
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

/* The sprite is two-hue: live (saturated) pixels are pushed in sat/value; the amber family
 * keeps an extra warm lift so the eyes read warm even in cool states. The pale metal only
 * takes a faint catch of the glow. */
static bool is_warm(uint8_t i)
{
    float r = SPR_PAL[i][0] / 255.0f, g = SPR_PAL[i][1] / 255.0f, b = SPR_PAL[i][2] / 255.0f;
    float h, s, v;
    rgb_to_hsv(r, g, b, &h, &s, &v);
    /* the golem's equivalent of SHODAN's amber: the near-neutral bright lens (it must stay hot) */
    return v > 0.85f && s < 0.30f;
}

static void update_palette(const scheme_t *t, float dt)
{
    float k = s_pal_init ? 1.0f - expf(-dt * 7.0f) : 1.0f;
    s_sat += (t->sat - s_sat) * k;
    s_val += (t->val - s_val) * k;
    s_warmv += (t->warm_val - s_warmv) * k;
    s_hue_k += (t->hue_k - s_hue_k) * k;
    uint32_t tg[5] = { t->f[0], t->f[1], t->f[2], t->f[3], t->acc };
    for (int i = 0; i < 5; i++) {
        float tr = (float)((tg[i] >> 16) & 255), tgc = (float)((tg[i] >> 8) & 255), tb = (float)(tg[i] & 255);
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
            s = clampf(s * s_sat, 0, 1);
            v = clampf(v * s_val * (is_warm(i) ? s_warmv : 1.0f), 0, 1);
            /* rotate the stone's hue toward the state's own hue (the GLOW colour's) */
            float hr, hg, hb, htone_h, htone_s, htone_v;
            hr = (float)((s_ramp[2][0])) / 255.0f;   /* the deep ramp tone carries the hue */
            hg = (float)((s_ramp[2][1])) / 255.0f;
            hb = (float)((s_ramp[2][2])) / 255.0f;
            rgb_to_hsv(hr, hg, hb, &htone_h, &htone_s, &htone_v);
            float dh = htone_h - h;
            if (dh > 180.0f) {
                dh -= 360.0f;
            }
            if (dh < -180.0f) {
                dh += 360.0f;
            }
            /* keep warm hues warm: never drag a gold through violet the long way */
            if (dh < -180.0f + 0.1f) { }
            h = fmodf(h + dh * s_hue_k + 360.0f, 360.0f);
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
    static const uint32_t rb[7] = { 0xd82a2a, 0xff8a1a, 0xffd21a, 0x4ac41a, 0x1a9ad8, 0x3a3ac8, 0x8a2ad8 };
    for (int i = 0; i < 7; i++) {
        s_pal[D_RB0 + i] = to565f((float)((rb[i] >> 16) & 255), (float)((rb[i] >> 8) & 255), (float)(rb[i] & 255));
        s_pal_dim[D_RB0 + i] = to565f((float)((rb[i] >> 16) & 255) * 0.72f, (float)((rb[i] >> 8) & 255) * 0.72f, (float)(rb[i] & 255) * 0.72f);
    }
    s_pal[D_COOL] = to565f(95.0f, 184.0f, 255.0f);   /* the weather prop's own cool blue */
    s_pal_dim[D_COOL] = to565f(68.0f, 132.0f, 184.0f);
}

/* ---------------------------------------------------------------------------
 * Eyes: the two amber lenses, animated (open / close / look / blink / wink)
 * ------------------------------------------------------------------------- */

typedef struct {
    int n;
    uint8_t x[48], y[48], c[48];   /* the golem's lenses are 31 px each: 2x the old cap */
    int xmin, xmax, ymin, ymax;
} eye_t;

static eye_t s_eye[2];
static bool s_eyes_init;
static float s_eye_scale[2] = { 1.0f, 1.0f };   /* per eye: 0 shuts it (a wink) */

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
        s_eye[s].xmin = 99; s_eye[s].xmax = -1; s_eye[s].ymin = 99; s_eye[s].ymax = -1;
        for (int i = 0; i < s_eye[s].n; i++) {
            if (s_eye[s].x[i] < s_eye[s].xmin) s_eye[s].xmin = s_eye[s].x[i];
            if (s_eye[s].x[i] > s_eye[s].xmax) s_eye[s].xmax = s_eye[s].x[i];
            if (s_eye[s].y[i] < s_eye[s].ymin) s_eye[s].ymin = s_eye[s].y[i];
            if (s_eye[s].y[i] > s_eye[s].ymax) s_eye[s].ymax = s_eye[s].y[i];
        }
    }
    s_eyes_init = true;
}

/* The idle scheduler (T3.2): blink cadence (doubles, doze blinks), speed-classed
 * saccades, micro-expression beats and the breath -- the shared muse_idle.h,
 * host-tested in tools/muse/idle_test.c. The reaction poses still poke s_idle.tx/ty
 * directly; the scheduler treats a poke as the new target. */
static muse_idle_t s_idle;

/* The lens sparkle: the hot pixel inside each lens wanders on its own slow beat. */
static int s_sp_tx, s_sp_ty;

/* The T3.1 rig: eased expression channels. The targets are computed per frame in
 * muse_pixel_render; these hold what is actually drawn, so a mood or mode change
 * reads as motion, not a cut. First frame: settled (no fade-in from zero). */
static muse_rig_t s_rig;
static bool s_rig_init;
static float s_eye_cur[2] = { 1.0f, 1.0f };   /* eased per-eye wink scale (targets in s_eye_scale) */

/* Returns how closed the eyes are (0 open .. 1 shut) and steps the gaze/beats. */
static float look_update(const muse_pose_t *p, float dt, bool allow_ev)
{
    if (!s_idle.rng) {
        muse_idle_init(&s_idle, (uint32_t)(frand() * 100000.0f) + 11u);
    }
    int gm = p->mode == MUSE_MODE_LISTENING ? 1 : p->mode == MUSE_MODE_THINKING ? 2 : 0;
    muse_idle_step(&s_idle, p->t, dt, gm, allow_ev);
    s_sp_tx = iround(1.6f * sinf(p->t * 0.9f));
    s_sp_ty = iround(0.9f * sinf(p->t * 0.53f + 1.3f));
    return muse_idle_shut(&s_idle, p->t);
}

/* Edits only the eye's own pixels, so the visor around them stays as drawn. */
static void draw_eyes(float open, float close, bool xeye, float bright, float widen)
{
    for (int s = 0; s < 2; s++) {
        const eye_t *e = &s_eye[s];
        if (e->n == 0) {
            continue;
        }
        int cx = (e->xmin + e->xmax) / 2, cy = (e->ymin + e->ymax) / 2;
        if (xeye) {
            for (int i = 0; i < e->n; i++) {
                px(e->x[i], e->y[i], IDX_SOCKET);
            }
            for (int d = -2; d <= 2; d++) {
                px(cx + d, cy + d, D_G1);
                px(cx + d, cy - d, D_G1);
            }
            continue;
        }
        float o = open * (1.0f - close) * s_eye_cur[s];
        for (int i = 0; i < e->n; i++) {
            px(e->x[i], e->y[i], IDX_SOCKET);          /* wipe the lens back to the visor */
        }
        if (o < 0.3f) {
            for (int x = e->xmin; x <= e->xmax; x++) {
                px(x, cy, IDX_LID);                      /* a closed eye is a dark seam */
            }
            continue;
        }
        int sx = iround(s_idle.gx * 2.0f), sy = iround(s_idle.gy * 1.0f);
        int top = e->ymin;
        for (int i = 0; i < e->n; i++) {
            if (o < 0.62f && e->y[i] == top) {
                continue;                                  /* half-lidded: the top row is gone */
            }
            uint8_t c = e->c[i];
            int ox = 0, oy = 0;
            if (c == IDX_EYE_U) {                    /* the glint: wanders over the lens */
                ox = s_sp_tx;
                oy = s_sp_ty;
                if (bright > 0.8f) {
                    c = D_G0;
                }
            } else if (c == IDX_EYE_R && bright > 0.5f && ((e->x[i] + e->y[i]) & 1) == 0) {
                c = D_G1;
            }
            px(e->x[i] + sx + ox, e->y[i] + sy + oy, c);
        }
        if (widen > 0.5f) {                               /* wide open: a row more above and below */
            for (int i = 0; i < e->n; i++) {
                if (e->y[i] == top) px(e->x[i] + sx, e->y[i] - 1 + sy, IDX_EYE_R);
                if (e->y[i] == e->ymax) px(e->x[i] + sx, e->y[i] + 1 + sy, IDX_EYE_R);
            }
        }
    }
}

/* The mouth: a dark slot on the face plate, opening to a tall rounded O for speech.
 * Always repaints the whole slot region: the rows must CLOSE as well as open (the carved
 * sprite rows cannot be relied on as the closed shape). */
static void draw_mouth(float open)
{
    static const int x0 = 28, x1 = 35;         /* the carved slot (x 28..35, y 30..31) */
    for (int r = 0; r < 3; r++) {              /* wipe to face plate first (3 rows of slack) */
        for (int x = x0 + r; x <= x1 - r; x++) {
            px(x, 30 + r, IDX_SKIN);
        }
    }
    if (open < 0.2f) {                         /* closed: the flat carved slit */
        for (int x = x0; x <= x1; x++) {
            px(x, 30, IDX_LID);
        }
    } else if (open < 0.45f) {                 /* half: a small rounded O (2 rows) */
        for (int x = x0 + 1; x <= x1 - 1; x++) {
            px(x, 30, IDX_LID);
        }
        for (int x = x0 + 2; x <= x1 - 2; x++) {
            px(x, 31, IDX_BLACK);
        }
    } else {                                   /* open: a tall O — dark walls, hollow, a floor */
        for (int x = x0 + 2; x <= x1 - 2; x++) {
            px(x, 30, IDX_LID);
            px(x, 31, IDX_BLACK);
            px(x, 32, IDX_BLACK);
            if (open > 0.9f && x >= x0 + 3 && x <= x1 - 3) {
                px(x, 33, IDX_LID);
            }
        }
        px(x0 + 1, 31, IDX_LID); px(x1 - 1, 31, IDX_LID);
        px(x0 + 1, 32, IDX_LID); px(x1 - 1, 32, IDX_LID);
    }
}

/* ---------------------------------------------------------------------------
 * Light: pulses along the cyan circuit lines, shimmer in the mid-tone circuitry,
 * and a chest core that beats
 * ------------------------------------------------------------------------- */

/* The light and shimmer walk only their own pixels: the kinds are fixed at bake time, so
 * the member indexes are collected once and the per-frame loops touch ~700 cells instead of
 * scanning all 4096. */
static EXT_RAM_BSS_ATTR uint16_t s_k1_px[512];
static EXT_RAM_BSS_ATTR uint16_t s_k3_px[768];
static int s_k1_n, s_k3_n;
static uint8_t s_k3_dark[SPR_N];     /* one ramp step down (the "fall back into shade" move) */

static void light_lists_init(void)
{
    for (int i = 0; i < SPR_N; i++) {
        s_k3_dark[i] = (uint8_t)i;
        for (int b = 0; b < SPR_N; b++) {
            if (SPR_BRIGHT[b] == i) {
                s_k3_dark[i] = (uint8_t)b;
            }
        }
    }
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t kind = SPR_KIND[y * W + x];
            uint16_t p = (uint16_t)(y * W + x);
            if (kind == 1 && s_k1_n < (int)(sizeof(s_k1_px) / sizeof(s_k1_px[0]))) {
                s_k1_px[s_k1_n++] = p;
            } else if (kind == 3 && s_k3_n < (int)(sizeof(s_k3_px) / sizeof(s_k3_px[0]))) {
                s_k3_px[s_k3_n++] = p;
            }
        }
    }
}

static void animate_light(float t, float speed, float level, float bright)
{
    static bool init;
    if (!init) {
        light_lists_init();
        init = true;
    }
    int tick = (int)(t * 3.0f);
    for (int i = 0; i < s_k1_n; i++) {
        uint16_t p = s_k1_px[i];
        int x = p % W, y = p / W;
        float ph = fmodf((float)(x * 9 + y * 13) / 96.0f - t * speed, 1.0f);
        if (ph < 0) {
            ph += 1.0f;
        }
        if (ph < 0.10f && bright > 0.25f) {
            s_fb[p] = D_WHITE;
        } else if (ph < 0.22f) {
            s_fb[p] = D_G0;
        }
    }
    for (int i = 0; i < s_k3_n; i++) {
        uint16_t p = s_k3_px[i];
        int x = p % W, y = p / W;
        /* the moss breathes: some px catch light, others fall back into shade.
         * Guarded: a dynamic index (>= SPR_N) can be under these pixels during mood
         * props, and SPR_BRIGHT/s_k3_dark are SPR_N-entry tables. */
        uint8_t u = s_fb[p];
        uint32_t h = hash2(x, y);
        if (u < SPR_N && ((h + (uint32_t)tick) & 7u) == 0u) {
            s_fb[p] = SPR_BRIGHT[u];
        } else if (u < SPR_N && ((h / 7 + (uint32_t)tick / 3) & 7u) == 3u) {
            s_fb[p] = s_k3_dark[u];
        } else if (u < SPR_N && level > 0.3f && ((hash2(x, y + 7) + (uint32_t)(t * 12.0f)) & 15u) == 0u) {
            s_fb[p] = SPR_BRIGHT[SPR_BRIGHT[u]];
        }
    }
}

/* ---------------------------------------------------------------------------
 * Continuous ambient motion: a drifting glint field over the stone plus a soft
 * breath band. The old resting "breath" was a rigid integer whole-frame bob; at
 * 40 ms it only fired every ~9th frame (byte-identical in between: a dead frame)
 * and when it did fire it jumped ~845 px in one step (the burst). These two
 * layers move a little EVERY frame and never translate the image, so the face is
 * never a copy of its predecessor and there is no stutter. Integer/index math
 * only in the per-pixel loops (one sinf per frame for the band phase).
 * ------------------------------------------------------------------------- */

#define MOTE_N 1200

static uint32_t s_frame;                        /* render calls seen; +1 every frame */

static uint8_t  s_spr_rank[SPR_N];              /* 0 = darkest .. SPR_N-1 = brightest */
static uint8_t  s_shade_hi[SPR_N];              /* one ramp step brighter (same family) */
static uint8_t  s_shade_lo[SPR_N];              /* one ramp step darker */
static EXT_RAM_BSS_ATTR uint8_t  s_pix_k[W * H];      /* PSRAM: 0 = empty, else rank+1 */
static EXT_RAM_BSS_ATTR uint16_t s_mote_px[MOTE_N];   /* PSRAM: glint-field candidates */
static int      s_mote_n;
static bool     s_motes_init;

static void motes_init(void)
{
    uint8_t order[SPR_N];
    for (int i = 0; i < SPR_N; i++) {
        order[i] = (uint8_t)i;
    }
    /* rank the palette by luminance (20 entries, once) */
    for (int a = 0; a < SPR_N; a++) {
        for (int b = a + 1; b < SPR_N; b++) {
            float la = 0.30f * SPR_PAL[order[a]][0] + 0.70f * SPR_PAL[order[a]][1];
            float lb = 0.30f * SPR_PAL[order[b]][0] + 0.70f * SPR_PAL[order[b]][1];
            if (lb < la) {
                uint8_t tmp = order[a]; order[a] = order[b]; order[b] = tmp;
            }
        }
    }
    for (int r = 0; r < SPR_N; r++) {
        s_spr_rank[order[r]] = (uint8_t)r;
    }
    for (int i = 0; i < SPR_N; i++) {
        int r = s_spr_rank[i];
        int lo = r > 1 ? r - 1 : 1;             /* never step a live colour down to black */
        int hi = r < SPR_N - 1 ? r + 1 : r;
        s_shade_lo[i] = order[lo];
        s_shade_hi[i] = order[hi];
    }
    s_shade_lo[IDX_BLACK] = s_shade_hi[IDX_BLACK] = IDX_BLACK;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t c = SPR[y * W + x];
            s_pix_k[y * W + x] = c == IDX_BLACK ? 0 : (uint8_t)(s_spr_rank[c] + 1);
        }
    }
    /* the body pixels the glint field drifts over: a ~1/4 spatial subset of the
     * solid stone, never the lens (rank >= 18 is the cream eye) */
    for (int y = 3; y < H - 3; y++) {
        for (int x = 3; x < W - 3; x++) {
            uint8_t k = s_pix_k[y * W + x];
            if (k == 0 || k > 17 || ((x * 3 + y * 5) & 3) != 0) {
                continue;
            }
            if (s_mote_n < MOTE_N) {
                s_mote_px[s_mote_n++] = (uint16_t)(y * W + x);
            }
        }
    }
    s_motes_init = true;
}

/* A drifting field of glints: each stone pixel catches the light on its own slow
 * beat. Always some pixels toggling, never the whole image — the ambient layer.
 * Driven by a monotonic frame counter (a float (uint32_t)(t*25) can stall for a
 * frame on rounding and reintroduce a byte-identical frame). */
static void draw_motes(float t, float bright)
{
    (void)t;
    if (!s_motes_init) {
        motes_init();
    }
    if (bright < 0.10f) {
        return;
    }
    uint32_t fr = s_frame;
    for (int i = 0; i < s_mote_n; i++) {
        uint16_t p = s_mote_px[i];
        int x = p % W, y = p / W;
        uint32_t h = hash2(x, y);
        uint32_t rate = 1u + ((h >> 9) & 3u);    /* 1..4 steps per frame */
        uint32_t per = 7u + ((h >> 13) & 15u);   /* 7..22 step period */
        if (((h >> 17) + fr * rate) % per <= per / 3u) {   /* ~1/3 lit at any moment */
            uint8_t under = s_fb[p];
            uint8_t c = under < SPR_N ? s_shade_hi[under] : under;   /* guard: idx >= SPR_N is dynamic */
            if (c != under) {
                s_fb[p] = c;
            }
        }
    }
}

/* A soft glow band sweeping the stone with the breath (~0.7 s). Its dithered edge
 * moves every frame, so the body reads as breathing WITHOUT the image translating. */
static void breath_band(float t, float bright)
{
    if (!s_motes_init) {
        motes_init();
    }
    if (bright < 0.10f) {
        return;
    }
    float ph = 0.5f + 0.5f * sinf(t * 1.4f);
    int band = 8 + (int)(ph * 44.0f);
    for (int i = 0; i < s_mote_n; i++) {
        uint16_t p = s_mote_px[i];
        int x = p % W, y = p / W;
        int d = y > band ? y - band : band - y;
        if (d > 2) {
            continue;
        }
        if (BAYER4[y & 3][x & 3] < 3 - d) {
            uint8_t under = s_fb[p];
            uint8_t c = under < SPR_N ? s_shade_hi[under] : under;   /* guard: idx >= SPR_N is dynamic */
            if (c != under) {
                s_fb[p] = c;
            }
        }
    }
}

/* A single standby pip that breathes on its own, so the fully-dark OFF state is
 * not a frozen frame (the CRT collapse leaves one pixel; it was byte-identical).
 * The arms grow/shrink with the beat and a spark rotates round the cross — the
 * spark guarantees a fresh pixel every frame even when the integer arm is static. */
static void off_pip(float t)
{
    float s = 0.5f + 0.5f * sinf(t * 2.6f);
    int half = 1 + (int)(s * 3.0f);              /* arms grow 1..4 px and back */
    uint8_t c = s > 0.5f ? D_G1 : D_G0;
    for (int x = 32 - half; x <= 32 + half; x++) {
        px(x, 32, c);
    }
    for (int y = 32 - half; y <= 32 + half; y++) {
        px(32, y, c);
    }
    px(32, 32, s > 0.75f ? D_G0 : D_ACC);
    static const int spx[4] = { 0, 1, 0, -1 }, spy[4] = { -1, 0, 1, 0 };
    int ph = (int)(s_frame % 4);                 /* the spark walks the cross */
    px(32 + spx[ph], 32 + spy[ph], D_WHITE);
}

/* The chest emblem's core breathes; it flares with the voice. */
static void chest_core(float t, float level, float bright)
{
    /* The core GLOWS — synced to the breath beat and the VOICE level (level now drives the
     * swell, no longer voided). Dense hot centre, soft falloff, no sparse dither specks. */
    int cx = 31, cy = 49;                 /* the plate's true centre (y46..53 + keyhole) */
    float beat = 0.5f + 0.5f * sinf(t * 3.4f);
    float drive = 0.35f + 0.65f * level;                          /* the voice itself */
    drive = fmaxf(drive * bright, 0.25f * bright);
    float rr = (12.0f + 24.0f * (0.30f + 0.70f * beat)) * (0.85f + 0.55f * drive);
    int hot = 6 + (int)(4.0f * drive);                            /* the hot centre follows level */
    for (int dy = -8; dy <= 8; dy++) {
        for (int dx = -8; dx <= 8; dx++) {
            float d2 = (float)(dx * dx) / 1.4f + (float)(dy * dy);
            int x = cx + dx, y = cy + dy;
            if (d2 < rr) {
                uint8_t cur = pxat(x, y);
                /* Guard: dynamic indices (>= SPR_N) from the light pass have no SPR_PAL row;
                 * treat them as already glowing (paint the hot centre only). */
                float pv = cur < SPR_N ? fmaxf(SPR_PAL[cur][0], fmaxf(SPR_PAL[cur][1], SPR_PAL[cur][2])) / 255.0f : 0.0f;
                bool dark_teal = cur < SPR_N && pv < 0.12f && SPR_PAL[cur][2] >= SPR_PAL[cur][0];
                bool gold_fam = cur < SPR_N && SPR_PAL[cur][0] > 60 && SPR_PAL[cur][0] >= SPR_PAL[cur][2];
                if (dark_teal || gold_fam) {
                    if (d2 < (float)(hot * hot) * 0.45f) {
                        px(x, y, D_G0);                          /* the hot centre */
                    } else if (d2 < (float)(hot * hot) * 1.35f) {
                        px(x, y, D_G1);
                    } else {
                        float k = (1.0f - d2 / rr) * (0.55f + 0.45f * drive);
                        if (BAYER4[y & 3][x & 3] < (int)(k * 16.0f) + 7) {
                            px(x, y, D_G2);
                        }
                    }
                }
            }
        }
    }
    /* the voice pip: white-hot with the voice, warm at rest */
    px(cx, cy, level > 0.28f ? D_WHITE : D_G0);
}

/* Thinking: the core works. Three sparks circuit the chest core -- the golem's
 * "computing" read, in place of the old half-lid that collided with the sleepy row.
 * The dots ride the plate's rim (r ~ 10) so they never cross the face or the lens;
 * the tail dot trails the lead by a fixed angle, so the circuit has a direction. */
static float s_think_k;   /* eased 0..1: the sparks are motion, not a cut */
static void think_spin(float t, float k)
{
    for (int i = 0; i < 3; i++) {
        float a = t * 2.2f + (float)i * (TAU / 3.0f);
        int x = 31 + iround(cosf(a) * 10.5f), y = 49 + iround(sinf(a) * 9.0f);
        int tx = 31 + iround(cosf(a - 0.55f) * 10.5f), ty = 49 + iround(sinf(a - 0.55f) * 9.0f);
        if (BAYER4[y & 3][x & 3] < (int)(k * 16.0f) + 1) {
            px(x, y, D_WHITE);
        }
        if (BAYER4[ty & 3][tx & 3] < (int)(k * 9.0f)) {
            px(tx, ty, D_G1);
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

/* The screen is round: fade the corners out so the square has no edges. The threshold
 * only ever fires on the rim ring, so the candidates are baked once into a list (~700 px
 * instead of 4096 checks per frame). */
static EXT_RAM_BSS_ATTR uint8_t s_vig[W * H];          /* PSRAM */
static EXT_RAM_BSS_ATTR uint16_t s_vig_px[1500];       /* PSRAM: rim + corner candidates */
static int s_vig_n;

static void vignette(void)
{
    static bool init;
    if (!init) {
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                float dx = (float)x - 31.5f, dy = (float)y - 31.5f;
                float d = sqrtf(dx * dx + dy * dy);
                uint8_t k = (uint8_t)(clampf((32.5f - d) / 3.5f, 0, 1) * 16.0f + 0.5f);
                s_vig[y * W + x] = k;
                /* only pixels the BAYER test can ever blank are worth visiting: every
                 * k<16 pixel qualifies somewhere in the 4x4 dither cell */
                if (k < 16 && s_vig_n < (int)(sizeof(s_vig_px) / sizeof(s_vig_px[0]))) {
                    s_vig_px[s_vig_n++] = (uint16_t)(y * W + x);
                }
            }
        }
        init = true;
    }
    for (int i = 0; i < s_vig_n; i++) {
        uint16_t p = s_vig_px[i];
        int x = p % W, y = p / W;
        if (BAYER4[y & 3][x & 3] >= s_vig[p]) {
            s_fb[p] = IDX_BLACK;
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
 * acts on a copy of the mode's own values; the weighted layers (the props) draw
 * each mood with its own blend weight and the continuous channels blend by weight,
 * so a second mood arriving mid-flight hands over instead of cutting. The discrete
 * things (the tremor, the dy nudges, the glitch bursts, the palette's mood
 * override) read from the owning mood. One live mood reproduces the old path
 * exactly (its weight == rk). --- */
static muse_blend_t s_blend;

typedef struct {
    float open, widen, bright, bloom, mouth;
    float tx, ty;                 /* gaze target; applies only while ovr */
    bool  ovr;
    float eye[2];                 /* per-eye scale (the wink) */
    float hovr;                   /* the pleased lift (joins the pet response) */
    int   dy;                     /* the mood's discrete nudge, rows */
    int   shake;                  /* the mood's tremor */
    bool  sh_ovr;                 /* the tremor above is the mood's, not the mode's */
} golem_act_t;

/* One mood's acting, over the mode's own values (the old per-reaction switch). */
static void golem_act_mood(golem_act_t *a, int rid, float rt, float rk, float t)
{
    switch (rid) {
    case R_SMIRK:
        a->open = 0.62f; a->bright = 1.0f; a->bloom = 0.6f; a->tx = 0.9f; a->ovr = true;
        break;
    case R_SQUINT:
        a->open = 0.35f; a->bright = 0.5f; a->tx = -0.8f; a->ovr = true;
        break;
    case R_SURPRISED:
        a->open = 1.2f; a->widen = 1.0f; a->bright = 1.0f; a->mouth = 1.0f; a->bloom = 0.8f;
        if (rt < 0.35f) {
            a->dy -= 2;
        }
        break;
    case R_PLEASED:
        a->hovr = rk; a->bloom = 0.6f;
        break;
    case R_ANGRY:
        a->open = 0.9f; a->bright = 1.0f; a->bloom = 1.0f;
        if (rt < 0.6f) { a->shake = ((int)(t * 30.0f)) & 1 ? 1 : -1; a->sh_ovr = true; }
        break;
    case R_EVIL:
        a->open = 0.75f; a->bright = 1.0f; a->bloom = 1.0f;
        if (rt < 0.6f) { a->shake = ((int)(t * 30.0f)) & 1 ? 1 : -1; a->sh_ovr = true; }
        break;
    case R_SCAN:
        a->bright = 0.9f; a->bloom = 0.5f;
        a->ty = -0.7f + 1.4f * (rt / REACTIONS[R_SCAN].dur); a->ovr = true;
        break;
    case R_GLITCH:
        if (((int)(t * 20.0f)) % 3 == 0) {
            a->open = 0.1f;       /* the eyelid flicker stays a deliberate hard cut */
        }
        break;
    case R_LAUGH:
        a->open = 0.4f; a->mouth = fmodf(rt * 2.4f, 1.0f) < 0.45f ? 1.0f : 0.12f;   /* three slow ha-ha's */
        a->dy += ((int)(t * 11.0f)) & 1 ? -1 : 0;
        break;
    case R_WINK:
        a->open = 0.9f; a->bright = 1.0f; a->bloom = 0.7f;
        if (rt > 0.25f && rt < 0.95f) {
            a->eye[0] = 0.0f;
        }
        break;
    case R_SLEEPY:
        a->open = fmaxf(0.15f, 0.7f - rt * 0.22f); a->bright = 0.3f;
        if (rt > 0.8f) {
            a->dy += 1 + (rt > 2.0f ? 1 : 0);
        }
        break;
    case R_CONFUSED:
        a->open = 0.85f; a->tx = ((int)(t * 2.6f) & 1) ? 1.0f : -1.0f; a->ty = -0.2f; a->ovr = true;
        break;
    case R_BORED:
        a->open = 0.45f; a->bright = 0.4f;
        a->tx = cosf(rt * 2.2f); a->ty = 0.7f * sinf(rt * 2.2f); a->ovr = true;
        a->dy += 1;
        break;
    case R_DISGUST:
        a->open = 0.4f; a->bright = 0.7f;
        break;
    case R_NERVOUS:
        a->open = 1.0f; a->widen = 1.0f;
        a->tx = ((int)(t * 7.0f) & 1) ? 1.0f : -1.0f; a->ty = 0; a->ovr = true;
        a->shake = ((int)(t * 40.0f)) & 1 ? 1 : 0; a->sh_ovr = true;
        break;
    case R_PROUD:
        a->open = 0.8f; a->bright = 1.0f; a->bloom = 0.5f;
        a->dy -= 1;
        break;
    case R_POWERUP:
        a->bright = 1.0f; a->bloom = clampf(rt * 1.2f, 0, 1); a->open = 1.0f; a->widen = 1.0f;
        break;
    case R_TARGET:
        a->open = 0.7f; a->bright = 1.0f; a->tx = 0; a->ty = 0; a->ovr = true;
        break;
    case R_EXCITED:
        a->open = 1.0f; a->widen = 1.0f; a->bright = 1.0f; a->bloom = 0.7f;
        break;
    case R_SUNNY:
        a->open = 0.55f; a->bright = 1.0f; a->bloom = 0.6f;
        break;
    case R_CONFETTI:
    case R_RAINBOW:
        a->open = 0.9f; a->bright = 1.0f; a->bloom = 0.5f;
        break;
    case R_COLD:
        a->open = 0.45f; a->bright = 0.6f;
        break;
    case R_RAIN:
        a->open = 0.7f; a->bright = 0.7f;
        break;
    case R_STORM:
        a->open = 1.0f; a->widen = 1.0f; a->bright = 1.0f;
        break;
    default:
        break;
    }
}

/* The palette's mood override: which scheme a mood pushes the face toward (the
 * mode's own, if the mood has no override). The old if/else chain, per mood. */
static const scheme_t *golem_scheme_mood(int rid, float rt)
{
    if (rid == R_ANGRY || rid == R_EVIL || (rid == R_ALERT && (((int)(rt * 4.0f)) & 1) == 0) ||
        (rid == R_TARGET && rt > 1.3f)) {
        return &SCHEMES[MUSE_MODE_ERROR];
    }
    if (rid == R_DISGUST) {
        return &SCHEMES[MUSE_MODE_SPEAKING];           /* a sickly yellow */
    }
    if (rid == R_SLEEPY) {
        return &SCHEMES[MUSE_MODE_OFF];                /* the light dims */
    }
    if (rid == R_HACKED && (((int)(rt * 9.0f)) % 5) == 0) {
        return &SCHEMES[MUSE_MODE_THINKING];           /* a violet flicker */
    }
    if (rid == R_SUNNY || rid == R_CONFETTI || rid == R_RAINBOW) {
        return &SCHEMES[MUSE_MODE_SPEAKING];           /* warm and bright (gold stays gold) */
    }
    if (rid == R_RAIN || rid == R_COLD || rid == R_STORM) {
        return &SCHEMES[MUSE_MODE_SPEAKING];           /* gold stone: the weather lives in the props */
    }
    return NULL;
}

void muse_pixel_render(const muse_pose_t *p)
{
    if (!s_eyes_init) {
        init_eyes();
    }
    s_frame++;                        /* monotonic per-frame ticks for the ambient layers */
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
     * old if/else chain, golem_scheme_mood) mixes in by weight, so the face's whole
     * look slides between two moods instead of stepping. One mood lands exactly on
     * its scheme; the palette's own ease below still smooths the remainder. */
    scheme_t st = SCHEMES[mode];
    {
        float ws = 0.0f, acc[5][3] = {{ 0 }}, f_sat = 0, f_val = 0, f_warm = 0, f_hue = 0;
        for (int si = 0; si < s_blend.n; si++) {
            const muse_blend_slot_t *sl = &s_blend.s[si];
            if (sl->w <= MUSE_BLEND_EPS) {
                continue;
            }
            const scheme_t *sc = golem_scheme_mood(sl->id, sl->age);
            if (!sc) {
                sc = &SCHEMES[mode];
            }
            uint32_t cols[5] = { sc->f[0], sc->f[1], sc->f[2], sc->f[3], sc->acc };
            for (int k = 0; k < 5; k++) {
                acc[k][0] += sl->w * (float)((cols[k] >> 16) & 255);
                acc[k][1] += sl->w * (float)((cols[k] >> 8) & 255);
                acc[k][2] += sl->w * (float)(cols[k] & 255);
            }
            f_sat += sl->w * sc->sat;
            f_val += sl->w * sc->val;
            f_warm += sl->w * sc->warm_val;
            f_hue += sl->w * sc->hue_k;
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
            st.sat = st.sat + (f_sat - st.sat * ws) / S;
            st.val = st.val + (f_val - st.val * ws) / S;
            st.warm_val = st.warm_val + (f_warm - st.warm_val * ws) / S;
            st.hue_k = st.hue_k + (f_hue - st.hue_k * ws) / S;
        }
    }
    update_palette(&st, dt);
    /* T3.2: beats only start while no mood is in charge and no mode owns the face; a
     * REACT cancels any beat in flight (see muse_idle.h for the priority rules). A
     * mood that is fading out no longer cancels them -- its props are leaving. */
    bool allow_ev = !muse_blend_busy(&s_blend) &&
                    (mode == MUSE_MODE_IDLE || mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_THINKING);
    float close = look_update(p, dt, allow_ev);

    /* Per-mode parameters. */
    float open = 0.8f, bright = 0.6f, pulse = 0.35f, widen = 0, mouth = 0;
    bool xeye = false;
    int shake = 0;
    switch (mode) {
    case MUSE_MODE_LISTENING:
        open = 1.0f;
        widen = 1.0f;
        bright = 0.9f;
        pulse = 0.55f + level * 0.8f;
        break;
    case MUSE_MODE_THINKING:
        /* Thinking is the core WORKING, not the lids drooping: the eyes stay open
         * (the idle scheduler already ponders the gaze up into a corner) and the
         * think_spin sparks circuit the chest core below. The old half-lid here
         * read as a milder sleepy -- the droop is the sleepy row's signature. */
        open = 0.85f;
        pulse = 0.9f;
        bright = 0.75f;
        break;
    case MUSE_MODE_SPEAKING:
        open = 0.95f;
        bright = 1.0f;
        pulse = 0.7f + level * 1.2f;
        /* A golem doesn't flap a mouth — its voice is the core: the slot parts once at
         * speak-start (an inhale) and the chest glow carries the voice level. */
        mouth = p->mode_t < 0.9f ? clampf(1.0f - p->mode_t / 0.9f, 0, 1) * 1.1f : 0.15f;
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
    s_eye_scale[0] = s_eye_scale[1] = 1.0f;   /* targets; the blend refines below */
    float bloom = 0;

    /* The cross-mood blend (muse_blend.h): every live mood acts on a copy of the
     * mode's own values; the continuous channels blend by weight (w/sum(w) with the
     * sum held at >= 1, so one mood lands at full weight and the last one out ramps
     * back to the mode's own acting), while the discrete bits -- the tremor, the dy
     * nudges -- come from the owning mood. */
    golem_act_t a0 = { .open = open, .widen = widen, .bright = bright, .bloom = 0.0f, .mouth = mouth,
                       .tx = s_idle.tx, .ty = s_idle.ty, .eye = { 1.0f, 1.0f } };
    float wsum = 0, c_open = 0, c_widen = 0, c_bright = 0, c_bloom = 0, c_mouth = 0;
    float c_tx = 0, c_ty = 0, c_eye0 = 0, c_eye1 = 0, c_dy = 0;
    for (int si = 0; si < s_blend.n; si++) {
        const muse_blend_slot_t *sl = &s_blend.s[si];
        if (sl->w <= MUSE_BLEND_EPS) {
            continue;
        }
        golem_act_t a = a0;
        golem_act_mood(&a, sl->id, sl->age, muse_blend_rk(sl), t);
        c_open += sl->w * a.open;
        c_widen += sl->w * a.widen;
        c_bright += sl->w * a.bright;
        c_bloom += sl->w * a.bloom;
        c_mouth += sl->w * a.mouth;
        c_tx += sl->w * a.tx;
        c_ty += sl->w * a.ty;
        c_eye0 += sl->w * a.eye[0];
        c_eye1 += sl->w * a.eye[1];
        c_dy += sl->w * (float)a.dy;
        wsum += sl->w;
    }
    golem_act_t atop = a0;
    if (top >= 0) {
        golem_act_mood(&atop, s_blend.s[top].id, s_blend.s[top].age, muse_blend_rk(&s_blend.s[top]), t);
    }
    float dy_mood = 0.0f;
    if (wsum > 0.0f) {
        float S = wsum > 1.0f ? wsum : 1.0f;
        open = a0.open + (c_open - a0.open * wsum) / S;
        widen = a0.widen + (c_widen - a0.widen * wsum) / S;
        bright = a0.bright + (c_bright - a0.bright * wsum) / S;
        bloom = a0.bloom + (c_bloom - a0.bloom * wsum) / S;
        mouth = a0.mouth + (c_mouth - a0.mouth * wsum) / S;
        s_idle.tx = a0.tx + (c_tx - a0.tx * wsum) / S;
        s_idle.ty = a0.ty + (c_ty - a0.ty * wsum) / S;
        s_eye_scale[0] = a0.eye[0] + (c_eye0 - a0.eye[0] * wsum) / S;
        s_eye_scale[1] = a0.eye[1] + (c_eye1 - a0.eye[1] * wsum) / S;
        dy_mood = (c_dy - (float)a0.dy * wsum) / S;
    }
    if (atop.hovr > happy) {
        happy = atop.hovr;     /* R_PLEASED: the mood's own lift joins the pet response */
    }
    if (happy > 0) {
        bright = 1.0f;
        open = fmaxf(open, 0.95f);
        widen = 0.8f;
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

    /* Idle beats (T3.2): only while allowed; a reaction cancels them outright. The
     * golem's lids are thresholded (<0.3 seam, <0.62 half-lid), so the squint-on-
     * speech coupling that reads as a lid droop on the other faces would only
     * chatter a threshold here -- its voice stays the chest core. */
    float evk = muse_idle_ev_k(&s_idle, t);
    if (s_idle.ev == MUSE_IDLE_EV_WIDEN) {
        widen = fmaxf(widen, evk);
    } else if (s_idle.ev == MUSE_IDLE_EV_SMILE) {
        open = fmaxf(open, 0.9f);
        bright = fmaxf(bright, 0.7f + 0.3f * evk);   /* a warm flare */
        bloom = fmaxf(bloom, 0.5f * evk);
    } else if (s_idle.ev == MUSE_IDLE_EV_WINK) {
        s_eye_scale[s_idle.ev_side] = 0.0f;          /* eased by s_eye_cur below */
    }
    if (s_idle.ev == MUSE_IDLE_EV_SQUINT) {
        close = fmaxf(close, evk * 0.5f);            /* the beat reads as a half-lid dip */
    }

    /* T3.1 rig: from here down the eased channels replace the targets, so an
     * expression change is motion, not a cut. The glitch's eyelid flicker stays a
     * deliberate hard cut; the wink scale eases so the lid lands smooth. */
    muse_rig_t rt_tgt = { .open = open, .widen = widen, .bright = bright, .mouth = mouth, .bloom = bloom };
    if (!s_rig_init) {
        s_rig = rt_tgt;
        s_rig_init = true;
    }
    muse_rig_step(&s_rig, &rt_tgt, dt);
    if (rid != R_GLITCH) {
        open = s_rig.open;
    }
    widen = s_rig.widen;
    bright = s_rig.bright;
    mouth = s_rig.mouth;
    bloom = s_rig.bloom;
    for (int s = 0; s < 2; s++) {
        s_eye_cur[s] = muse_ease(s_eye_cur[s], s_eye_scale[s], dt, 0.05f);
    }

    memcpy(s_fb, SPR, sizeof(s_fb));
    animate_light(t, 0.45f + pulse * 0.9f, level, bright);
    chest_core(t, level, bright);
    s_think_k += ((mode == MUSE_MODE_THINKING ? 1.0f : 0.0f) - s_think_k) *
                 (dt > 0.0f ? 1.0f - expf(-dt / 0.2f) : 1.0f);
    if (s_think_k > 0.02f) {
        think_spin(t, s_think_k);
    }
    if (bloom > 0.02f) {
        eye_bloom(bloom, D_G3);   /* the rig leaves a residue; below this nothing paints */
    }
    draw_eyes(open, close, xeye, bright, widen);
    if (mouth > 0) {
        draw_mouth(mouth);
    } else if (mode != MUSE_MODE_OFF) {
        draw_mouth(0);
    }
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING) {
        draw_wave(t, level);
    }
    /* The props: every live mood draws its own baked frame (make_mood_frames2.py) as
     * sparse diffs, cross-fading by its blend weight; the face acting (eyes/mouth/gaze)
     * above stays in C. Paint rule: never over the lens (eye-boxes) and only over
     * dark/bronze under-pixels (the mood never rewrites the character's face). */
    bool props_live = false;
    for (int si = 0; si < s_blend.n; si++) {
        if (muse_blend_rend(&s_blend.s[si]) > MUSE_BLEND_EPS) {
            props_live = true;
            break;
        }
    }
    if (props_live) {
        /* Lens-coverage bitset (512 B; a 4 KB bool grid before -- internal RAM is
         * the scarce pool). Prop pixels must never land on the eyes. */
        static uint8_t over_eye[64 * 64 / 8];
        memset(over_eye, 0, sizeof over_eye);
        int esx = iround(s_idle.gx * 2.0f), esy = iround(s_idle.gy * 1.0f);
        for (int s2 = 0; s2 < 2; s2++) {
            const eye_t *e = &s_eye[s2];
            for (int i2 = 0; i2 < e->n; i2++) {
                for (int oy = -1; oy <= 1; oy++) {
                    for (int ox = -2; ox <= 2; ox++) {
                        int yy = e->y[i2] + esy + oy, xx = e->x[i2] + esx + ox;
                        if (xx >= 0 && xx < 64 && yy >= 0 && yy < 64) {
                            over_eye[(yy * 64 + xx) >> 3] |= (uint8_t)(1u << ((yy * 64 + xx) & 7));
                        }
                    }
                }
            }
        }
        for (int si = 0; si < s_blend.n; si++) {
            const muse_blend_slot_t *sl = &s_blend.s[si];
            float pw = muse_blend_rend(sl);
            if (pw <= MUSE_BLEND_EPS) {
                continue;
            }
            int fi = -1;
            for (unsigned g = 0; g < sizeof(MOODS) / sizeof(MOODS[0]); g++) {
                /* match by NAME against REACTIONS[]: a hand-typed id table drifted from the
                 * enum once (alert played scan, sunny played confetti...), the names cannot. */
                const char *row = (const char *)MOODS[g].name;
                if (sl->id < R_COUNT && strcmp(row, REACTIONS[sl->id].name) == 0) {
                    fi = MOODS[g].off +
                         (int)(sl->age * 1000.0f / (float)MOOD_FRAME_DT_MS) % MOODS[g].n;
                    break;
                }
            }
            if (fi < 0) {
                continue;
            }
            int po = 0;
            for (int g = 0; g < fi; g++) {
                po += MOOD_FRAME_COUNTS[g];
            }
            for (int k = 0; k < MOOD_FRAME_COUNTS[fi]; k++) {
                int idx = MOOD_FRAME_PX[po * 2 + k * 2];
                uint8_t c = (uint8_t)MOOD_FRAME_PX[po * 2 + k * 2 + 1];
                int x = idx % 64, y = idx / 64;
                /* The props dissolve in and out with the mood's blend weight (pw): a mood
                 * that pops in full and vanishes mid-frame reads as a cut. */
                if (x < 1 || x > 62 || y < 1 || y > 62 ||
                    (over_eye[(y * 64 + x) >> 3] >> ((y * 64 + x) & 7) & 1u) ||
                    BAYER4[y & 3][x & 3] >= (int)(pw * 16.0f + 0.5f)) {
                    continue;
                }
                uint8_t under = pxat(x, y);
                /* Guard: under can be a dynamic index (>= SPR_N) after the light pass. */
                bool goldish = under < SPR_N && SPR_PAL[under][0] > 60 && SPR_PAL[under][0] >= SPR_PAL[under][2];
                bool darkish = under < SPR_N && fmaxf(SPR_PAL[under][0],
                                     fmaxf(SPR_PAL[under][1], SPR_PAL[under][2])) < 0.14f * 255.0f;
                if (goldish || darkish) {
                    px(x, y, c);
                }
            }
        }
    }
    /* A hop when petted, a shake on error, and the acted dy offsets. The resting
     * breath is NO LONGER an integer whole-frame bob: a ±1 px translate either fires
     * (~845 px change) or does not (0 px), which is exactly the dead/burst pattern.
     * The breath now lives in the continuous ambient layers below. The mood's dy
     * blends by weight and rounds: two moods trading opposite nudges step through
     * zero one pixel at a time instead of jumping. */
    float dyf = dy_mood;
    if (happy > 0) {
        dyf -= fabsf(sinf(t * 9.0f)) * 3.5f * happy;
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
        glitch(2, 3, 4, 60);
    }
    if (rid == R_GLITCH) {
        glitch(6, 6, 4, 60);
        if (((int)(t * 25.0f)) % 4 == 0) {
            shift_cols(2);
        }
    } else if (rid == R_ANGRY) {
        /* a tremor: the whole head shifts, no tear */
        if (!shake) {
            shake = 0;
        }
    }
    /* Stone doesn't interfere: the golem never glitches in speech, joy, or rest. It cracks
     * (heated stone) on error and re-forms out of dust on boot — that is all. */
    if (mode == MUSE_MODE_ERROR) {
        /* Error is ONE ongoing event, not per-frame noise: each tear opens, strains and
         * knits back over seconds (muse_crack.h) instead of teleporting anew every frame. */
        if (!s_cracks_run) {
            muse_crack_init(&s_cracks, (uint32_t)(frand() * 100000.0f) + 3u,
                            3, 4.0f, 59.0f, 4.0f, 0.35f);
            s_cracks_run = true;
        }
        muse_crack_step(&s_cracks, dt);
        muse_crack_apply(&s_cracks, s_fb, W, H, IDX_BLACK);
    } else if (mode == MUSE_MODE_BOOT) {
        glitch((int)((1.0f - boot) * 5.0f), 4, 4, 60);
    }
    if (mode != MUSE_MODE_ERROR) {
        s_cracks_run = false;   /* leaving ERROR: the next entry starts calm */
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
            off_pip(t);                     /* the standby dot breathes, no dead frame */
        }
    }
    /* Continuous ambient motion, after every translation/glitch so nothing smears:
     * the glint field + the breath band touch only body pixels and never the face. */
    draw_motes(t, bright);
    breath_band(t, bright);
    vignette();
}
