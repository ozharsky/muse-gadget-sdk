// Hermes avatar: the Construct. A sturdy little worker robot -- a dark visor holding two
// big amber eyes, small pale wing vents at the temples, an antenna with a glowing tip, and
// a cyan chest light. The picture itself is fixed pixel art (sprite_construct.h); what moves
// is light: it recolours with the state, pulses along the cyan circuit lines, shimmers in the
// mid-tone circuitry, watches/blinks/flickers, its chest core beats, and it glitches when it
// talks. Picture-based, like SHODAN (muse_pixel.c), not procedural like Bloop.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "muse_pixel.h"
#include "sprite_construct.h"

#if defined(__has_include) && __has_include("esp_attr.h")
#include "esp_attr.h"   /* PSRAM placement for the frame buffers */
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR   /* host preview builds: ordinary memory */
#endif

#define W MUSE_PX_W
#define H MUSE_PX_H
#define TAU 6.2831853f

/* Colours past the sprite's own: the state's glow ramp, its accent, and white. */
enum { D_G0 = SPR_N, D_G1, D_G2, D_G3, D_ACC, D_WHITE, N_PAL };

typedef struct {
    uint32_t f[4];
    uint32_t acc;
    float sat, val;    /* how the sprite's live colours are pushed: saturation and value scales */
    float warm_val;    /* extra value on the amber eyes/accents (they should stay warmer/brighter) */
} scheme_t;

/* Idle is warm amber; listening cyan; thinking violet; speaking bright warm; error red; off dim. */
static const scheme_t SCHEMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT]      = { { 0xffffff, 0xa0d8ff, 0x2f6cff, 0x123a9a }, 0x63b0ff, 1.0f, 1.05f, 1.10f },
    [MUSE_MODE_IDLE]      = { { 0xfff0c8, 0xffc84a, 0xff8a24, 0xc24a08 }, 0xffa832, 1.05f, 1.05f, 1.05f },
    [MUSE_MODE_LISTENING] = { { 0xdffcff, 0x5fe8ff, 0x24b6d9, 0x166fa8 }, 0x3ee0ff, 1.10f, 1.10f, 0.85f },
    [MUSE_MODE_THINKING]  = { { 0xf0e6ff, 0xb48bff, 0x7a4dff, 0x3a2aa8 }, 0x9a64ff, 1.10f, 1.05f, 0.70f },
    [MUSE_MODE_SPEAKING]  = { { 0xfff4d8, 0xffd24a, 0xff9a24, 0xc84a08 }, 0xffb03c, 1.12f, 1.14f, 1.15f },
    [MUSE_MODE_ERROR]     = { { 0xffe0d8, 0xff5a4d, 0xc71f2e, 0x5a1020 }, 0xff3a3a, 1.30f, 1.05f, 0.30f },
    [MUSE_MODE_OFF]       = { { 0x9fb0b8, 0x4f6a7a, 0x2a3a4a, 0x141e22 }, 0x2a4a5a, 0.35f, 0.45f, 0.40f },
};

static float s_sat = 1.0f, s_val = 1.0f, s_warmv = 1.0f;
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

/* A line of pixels. */
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

/* Brows: dark bars above the visor. Left runs outer (x 20) to inner (x 27), right mirrors. */
static void brows(int lo, int li, int ro, int ri, int thick)
{
    for (int t = 0; t < thick; t++) {
        ln(20, lo + t, 27, li + t, IDX_LID);
        ln(43, ro + t, 36, ri + t, IDX_LID);
    }
}

/* A stipple of the state's glow on the visor around the eyes. */
static void eye_bloom(float k, uint8_t c)
{
    static const int cx[2] = { 26, 37 };
    for (int e = 0; e < 2; e++) {
        for (int dy = -5; dy <= 5; dy++) {
            for (int dx = -8; dx <= 8; dx++) {
                float d2 = (float)(dx * dx) / 2.6f + (float)(dy * dy);
                int x = cx[e] + dx, y = 28 + dy;
                if (d2 < 30.0f && ((x + y) & 1) == 0 && BAYER4[y & 3][x & 3] < (int)((1.0f - d2 / 30.0f) * 16.0f * k)) {
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
static void draw_wave(float t, float level)
{
    float lv = level < 0.12f ? 0.12f : level;
    for (int i = 0; i < 180; i++) {
        float a = (float)i * TAU / 180.0f;
        float w = lv * 2.4f * sinf(a * 7.0f + t * 9.0f) + lv * 1.0f * sinf(a * 13.0f - t * 6.0f);
        float r = 26.0f + w;
        px(iround(31.5f + cosf(a) * r), iround(31.0f + sinf(a) * r), (i & 1) ? D_G1 : D_G0);
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
    R_EXCITED, R_CONFETTI, R_SUNNY, R_RAIN, R_STORM, R_COLD, R_RAINBOW, R_COUNT
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
};

static volatile int s_react_req;     /* set from any task; the renderer picks it up */
static int s_react_id;
static float s_react_start;

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
    return s > 0.30f && h >= 20.0f && h <= 70.0f;
}

static void update_palette(const scheme_t *t, float dt)
{
    float k = s_pal_init ? 1.0f - expf(-dt * 7.0f) : 1.0f;
    s_sat += (t->sat - s_sat) * k;
    s_val += (t->val - s_val) * k;
    s_warmv += (t->warm_val - s_warmv) * k;
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
 * Eyes: the two amber lenses, animated (open / close / look / blink / flicker)
 * ------------------------------------------------------------------------- */

typedef struct {
    int n;
    uint8_t x[24], y[24], c[24];
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

static struct {
    float last_t, next_gaze, gx, gy, tx, ty, next_blink, blink_t, next_flick, flick_t;
} s_look = { .next_gaze = 1.0f, .next_blink = 2.5f, .blink_t = -10.0f, .next_flick = 4.0f, .flick_t = -10.0f };

/* Returns how closed the eyes are (0 open .. 1 shut) and updates the gaze. */
static float look_update(const muse_pose_t *p, float dt, float *flick)
{
    if (p->mode == MUSE_MODE_LISTENING) {
        s_look.tx = s_look.ty = 0;
    } else if (p->t >= s_look.next_gaze) {
        s_look.tx = frand() * 2.0f - 1.0f;
        s_look.ty = (frand() * 2.0f - 1.0f) * 0.6f;
        if (p->mode == MUSE_MODE_THINKING) {
            s_look.tx = s_look.tx < 0 ? -1.0f : 1.0f;
            s_look.ty = -0.7f;
        }
        s_look.next_gaze = p->t + 1.2f + frand() * 2.6f;
    }
    float k = 1.0f - expf(-dt * 14.0f);
    s_look.gx += (s_look.tx - s_look.gx) * k;
    s_look.gy += (s_look.ty - s_look.gy) * k;
    if (p->t >= s_look.next_blink) {
        s_look.blink_t = p->t;
        s_look.next_blink = p->t + 2.4f + frand() * 3.2f;
    }
    if (p->t >= s_look.next_flick) {
        s_look.flick_t = p->t;
        s_look.next_flick = p->t + 3.0f + frand() * 5.0f;
    }
    *flick = clampf(1.0f - (p->t - s_look.flick_t) / 0.18f, 0, 1);
    float b = (p->t - s_look.blink_t) / 0.2f;
    return b >= 0 && b < 1 ? sinf(b * 3.1416f) : 0.0f;
}

/* Edits only the eye's own pixels, so the visor around them stays as drawn. */
static void draw_eyes(float open, float close, float flick, bool xeye, float bright, float widen)
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
        float o = open * (1.0f - close) * (1.0f - 0.55f * flick) * s_eye_scale[s];
        for (int i = 0; i < e->n; i++) {
            px(e->x[i], e->y[i], IDX_SOCKET);          /* wipe the lens back to the visor */
        }
        if (o < 0.3f) {
            for (int x = e->xmin; x <= e->xmax; x++) {
                px(x, cy, IDX_LID);                      /* a closed eye is a dark seam */
            }
            continue;
        }
        int sx = iround(s_look.gx * 1.0f), sy = iround(s_look.gy * 0.6f);
        int top = e->ymin;
        for (int i = 0; i < e->n; i++) {
            if (o < 0.62f && e->y[i] == top) {
                continue;                                  /* half-lidded: the top row is gone */
            }
            uint8_t c = e->c[i];
            if (c == IDX_EYE_U && bright > 0.8f) {
                c = D_G0;
            } else if (c == IDX_EYE_R && bright > 0.5f && ((e->x[i] + e->y[i]) & 1) == 0) {
                c = D_G1;
            }
            px(e->x[i] + sx, e->y[i] + sy, c);
        }
        if (widen > 0.5f) {                               /* wide open: a row more above and below */
            for (int i = 0; i < e->n; i++) {
                if (e->y[i] == top) px(e->x[i] + sx, e->y[i] - 1 + sy, IDX_EYE_R);
                if (e->y[i] == e->ymax) px(e->x[i] + sx, e->y[i] + 1 + sy, IDX_EYE_R);
            }
        }
    }
}

/* The mouth: a grille of slats across the visor's lower edge, opening for speech. */
static void draw_mouth(float open)
{
    static const int x0 = 26, x1 = 37;         /* visor mouth band (x 26..37, y 33) */
    int rows = open > 0.55f ? 2 : open > 0.2f ? 1 : 0;
    for (int r = 0; r < rows; r++) {
        for (int x = x0 - r; x <= x1 + r; x++) {
            px(x, 33 + r, r == 0 ? IDX_LID : IDX_BLACK);
        }
    }
    if (open < 0.2f) {                          /* closed: a thin grille */
        for (int x = x0; x <= x1; x++) {
            if ((x & 1) == 0) {
                px(x, 33, IDX_LID);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Light: pulses along the cyan circuit lines, shimmer in the mid-tone circuitry,
 * and a chest core that beats
 * ------------------------------------------------------------------------- */

static void animate_light(float t, float speed, float level, float bright)
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
                if (ph < 0.10f && bright > 0.25f) {
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

/* The chest light breathes; it flares with the voice. */
static void chest_core(float t, float level, float bright)
{
    (void)level;
    int cx = 35, cy = 49;
    float beat = 0.5f + 0.5f * sinf(t * 3.4f);
    if (bright > 0.5f && ((hash2((int)(t * 30.0f), 3) & 3u) == 0u)) {
        beat = 1.0f;
    }
    /* a soft ring of the state glow around the drawn core */
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -5; dx <= 5; dx++) {
            float d2 = (float)(dx * dx) / 1.6f + (float)(dy * dy);
            int x = cx + dx, y = cy + dy;
            if (d2 < 16.0f && ((x + y) & 1) == 0) {
                uint8_t cur = pxat(x, y);
                if (cur == IDX_SOCKET || cur == IDX_DARK || (cur < SPR_N && SPR_PAL[cur][2] > 90)) {
                    float k = (1.0f - d2 / 16.0f) * (0.35f + 0.4f * beat) * bright;
                    if (BAYER4[y & 3][x & 3] < (int)(k * 16.0f)) {
                        px(x, y, D_G2);
                    }
                }
            }
        }
    }
    /* a bright pip at the core, always */
    px(cx, cy, beat > 0.6f ? D_G0 : D_G1);
}

/* An energy field around the head: a dotted ellipse that pulses (the Construct's "halo"). */
static void halo(float t, float k, uint8_t c)
{
    float pulse = sinf(t * 7.0f) * 1.2f;
    for (int i = 0; i < 360; i += 3) {
        float a = (float)i * TAU / 360.0f;
        int x = iround(31.5f + cosf(a) * (21.0f + pulse)), y = iround(31.0f + sinf(a) * (25.0f + pulse));
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
        int x = 31 + side * 9, y = 28;
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

void muse_pixel_render(const muse_pose_t *p)
{
    if (!s_eyes_init) {
        init_eyes();
    }
    float dt = s_look.last_t > 0 ? clampf(p->t - s_look.last_t, 0, 0.2f) : 0.04f;
    s_look.last_t = p->t;

    muse_mode_t mode = p->mode;
    float t = p->t, level = clampf(p->level, 0, 1);
    float happy = mode == MUSE_MODE_ERROR ? 0 : p->happy;
    int rq = s_react_req;
    if (rq) {
        s_react_req = 0;
        s_react_id = rq;
        s_react_start = t;
    }
    int rid = R_NONE;
    float rt = 0, rk = 0;
    if (s_react_id != R_NONE) {
        rt = t - s_react_start;
        float dur = REACTIONS[s_react_id].dur;
        if (rt >= dur) {
            s_react_id = R_NONE;
        } else {
            rid = s_react_id;
            rk = clampf(fminf(rt / 0.15f, (dur - rt) / 0.4f), 0, 1);
        }
    }
    const scheme_t *tgt = &SCHEMES[mode];
    if (rid == R_ANGRY || rid == R_EVIL || (rid == R_ALERT && (((int)(rt * 4.0f)) & 1) == 0) ||
        (rid == R_TARGET && rt > 1.3f)) {
        tgt = &SCHEMES[MUSE_MODE_ERROR];
    } else if (rid == R_DISGUST) {
        tgt = &SCHEMES[MUSE_MODE_SPEAKING];           /* a sickly yellow */
    } else if (rid == R_SLEEPY) {
        tgt = &SCHEMES[MUSE_MODE_OFF];                /* the light dims */
    } else if (rid == R_HACKED && (((int)(rt * 9.0f)) % 5) == 0) {
        tgt = &SCHEMES[MUSE_MODE_THINKING];           /* a violet flicker */
    } else if (rid == R_SUNNY || rid == R_CONFETTI) {
        tgt = &SCHEMES[MUSE_MODE_SPEAKING];           /* warm and bright */
    } else if (rid == R_RAIN || rid == R_COLD) {
        tgt = &SCHEMES[MUSE_MODE_LISTENING];          /* cool cyan */
    } else if (rid == R_STORM) {
        tgt = &SCHEMES[MUSE_MODE_THINKING];           /* a dark violet squall */
    } else if (rid == R_RAINBOW) {
        tgt = &SCHEMES[MUSE_MODE_SPEAKING];
    }
    update_palette(tgt, dt);
    float flick;
    float close = look_update(p, dt, &flick);

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
        open = 0.55f;
        pulse = 0.9f;
        bright = 0.7f;
        break;
    case MUSE_MODE_SPEAKING:
        open = 0.95f;
        bright = 1.0f;
        pulse = 0.7f + level * 1.2f;
        mouth = level * 1.1f + 0.15f * fabsf(sinf(t * 29.0f));
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
    s_eye_scale[0] = s_eye_scale[1] = 1.0f;
    float bloom = 0;
    switch (rid) {
    case R_SMIRK:
        open = 0.62f; bright = 1.0f; bloom = 0.6f; s_look.tx = 0.9f;
        break;
    case R_SQUINT:
        open = 0.35f; bright = 0.5f; s_look.tx = -0.8f;
        break;
    case R_SURPRISED:
        open = 1.2f; widen = 1.0f; bright = 1.0f; mouth = 1.0f; bloom = 0.8f;
        break;
    case R_PLEASED:
        happy = fmaxf(happy, rk); bloom = 0.6f;
        break;
    case R_ANGRY:
        open = 0.9f; bright = 1.0f; bloom = 1.0f;
        break;
    case R_EVIL:
        open = 0.75f; bright = 1.0f; bloom = 1.0f;
        break;
    case R_SCAN:
        bright = 0.9f; bloom = 0.5f; s_look.ty = -0.7f + 1.4f * (rt / REACTIONS[R_SCAN].dur);
        break;
    case R_GLITCH:
        open = (((int)(t * 20.0f)) % 3) ? open : 0.1f;
        break;
    case R_LAUGH:
        open = 0.4f; mouth = 0.55f + 0.45f * sinf(t * 26.0f);
        break;
    case R_WINK:
        open = 0.9f; bright = 1.0f; bloom = 0.7f;
        if (rt > 0.25f && rt < 0.95f) {
            s_eye_scale[0] = 0.0f;
        }
        break;
    case R_SLEEPY:
        open = fmaxf(0.15f, 0.7f - rt * 0.22f); bright = 0.3f;
        break;
    case R_CONFUSED:
        open = 0.85f; s_look.tx = ((int)(t * 2.6f) & 1) ? 1.0f : -1.0f; s_look.ty = -0.2f;
        break;
    case R_BORED:
        open = 0.45f; bright = 0.4f; s_look.tx = cosf(rt * 2.2f); s_look.ty = 0.7f * sinf(rt * 2.2f);
        break;
    case R_DISGUST:
        open = 0.4f; bright = 0.7f;
        break;
    case R_NERVOUS:
        open = 1.0f; widen = 1.0f; s_look.tx = ((int)(t * 7.0f) & 1) ? 1.0f : -1.0f; s_look.ty = 0;
        break;
    case R_PROUD:
        open = 0.8f; bright = 1.0f; bloom = 0.5f;
        break;
    case R_POWERUP:
        bright = 1.0f; bloom = clampf(rt * 1.2f, 0, 1); open = 1.0f; widen = 1.0f;
        break;
    case R_TARGET:
        open = 0.7f; bright = 1.0f; s_look.tx = 0; s_look.ty = 0;
        break;
    case R_EXCITED:
        open = 1.0f; widen = 1.0f; bright = 1.0f; bloom = 0.7f;
        break;
    case R_SUNNY:
        open = 0.55f; bright = 1.0f; bloom = 0.6f;
        break;
    case R_CONFETTI:
    case R_RAINBOW:
        open = 0.9f; bright = 1.0f; bloom = 0.5f;
        break;
    case R_COLD:
        open = 0.45f; bright = 0.6f;
        break;
    case R_RAIN:
        open = 0.7f; bright = 0.7f;
        break;
    case R_STORM:
        open = 1.0f; widen = 1.0f; bright = 1.0f;
        break;
    default:
        break;
    }
    if ((rid == R_ALERT || rid == R_ANGRY || rid == R_EVIL) && rt < 0.6f) {
        shake = ((int)(t * 30.0f)) & 1 ? 1 : -1;
    }
    if (rid == R_NERVOUS) {
        shake = ((int)(t * 40.0f)) & 1 ? 1 : 0;
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

    memcpy(s_fb, SPR, sizeof(s_fb));
    animate_light(t, 0.45f + pulse * 0.9f, level, bright);
    chest_core(t, level, bright);
    if (bloom > 0) {
        eye_bloom(bloom, D_G3);
    }
    draw_eyes(open, close, flick, xeye, bright, widen);
    if (mouth > 0) {
        draw_mouth(mouth);
    } else if (mode != MUSE_MODE_OFF) {
        draw_mouth(0);
    }
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING) {
        draw_wave(t, level);
    }
    if (rid != R_NONE) {
        int tick = (int)(t * 12.0f);
        switch (rid) {
        case R_SMIRK:
            brows(25, 26, 27, 27, 1);
            halo(t, 0.25f * rk, D_G3);
            break;
        case R_SCAN: {
            int y = 10 + (int)(rt / REACTIONS[R_SCAN].dur * 44.0f);
            for (int yy = 10; yy < y - 2; yy += 3) {
                for (int x = 20; x <= 43; x++) {
                    /* Guard the palette read: s_fb can hold a dynamic index (D_G2,
                     * written by this very sweep on an earlier frame), which is past
                     * the end of the SPR_N-entry SPR_PAL table. */
                    uint8_t u = s_fb[yy * W + x];
                    if (((x + yy) & 1) == 0 && u < SPR_N && SPR_PAL[u][0] > 120) {
                        s_fb[yy * W + x] = D_G2;
                    }
                }
            }
            for (int x = 14; x <= 50; x++) {
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
            brows(25, 28, 25, 28, 2);
            halo(t, 0.9f, D_G1);
            if ((((int)(rt * 6.0f)) & 1) == 0) {
                glyph(48, 8, G_BANG, D_WHITE);
            }
            break;
        case R_GLITCH:
            for (int k = 0; k < 5; k++) {
                glyph(4 + (int)(hash2(tick, k) % 54u), 4 + (int)(hash2(tick + 3, k) % 50u), hash2(k, tick) & 1, k & 1 ? D_G0 : D_G2);
            }
            break;
        case R_SQUINT:
            brows(27, 27, 25, 24, 1);
            if (((int)(rt * 3.0f)) & 1) {
                glyph(49, 14, G_Q, D_G1);
            }
            break;
        case R_ANGRY:
            brows(24, 28, 24, 28, 2);
            halo(t, 0.9f, D_G1);
            if (tick & 1) {
                lightning(t, D_WHITE, D_G0);
            }
            break;
        case R_EVIL:
            brows(23, 29, 23, 29, 2);
            halo(t, 1.0f, D_G1);
            if (tick & 1) {
                lightning(t, D_G0, D_WHITE);
            }
            break;
        case R_SURPRISED:
            brows(22, 22, 22, 22, 1);
            glyph(49, 9, G_BANG, D_WHITE);
            break;
        case R_PLEASED:
            brows(25, 24, 25, 24, 1);
            halo(t, 0.5f * rk, D_G1);
            if (tick & 1) {
                px(22, 28, D_WHITE); px(21, 29, D_G0); px(23, 29, D_G0); px(22, 30, D_G0);
                px(41, 28, D_WHITE); px(40, 29, D_G0); px(42, 29, D_G0); px(41, 30, D_G0);
            }
            break;
        case R_LAUGH:
            brows(25, 24, 25, 24, 1);
            halo(t, (tick & 1) ? 0.5f : 0.1f, D_G2);
            break;
        case R_WINK:
            brows(25, 26, 23, 24, 1);
            if (rt > 0.3f && rt < 0.9f) {
                px(44, 28, D_WHITE); px(43, 28, D_G0); px(45, 28, D_G0); px(44, 27, D_G0); px(44, 29, D_G0);
            }
            break;
        case R_SLEEPY:
            for (int k = 0; k < 3; k++) {
                int y = 22 - (int)fmodf(rt * 6.0f + (float)k * 8.0f, 22.0f);
                if (y > 2) {
                    glyph(46 + k * 3 + (int)(sinf(rt * 2.0f + (float)k) * 1.5f), y, G_Z, k == 0 ? D_WHITE : D_G0);
                }
            }
            break;
        case R_CONFUSED:
            brows(24, 23, 26, 27, 1);
            if (((int)(rt * 4.0f)) & 1) {
                glyph(30, 6, G_Q, D_G0);
            }
            break;
        case R_HACKED:
            for (int x = 1; x < 63; x += 3) {
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
            for (int k = 0; k < 3; k++) {
                float r = fmodf(rt * 22.0f + (float)k * 9.0f, 30.0f);
                if (r > 8.0f) {
                    for (int i = 0; i < 64; i += 2) {
                        float a2 = (float)i * TAU / 64.0f;
                        px(iround(31.5f + cosf(a2) * r), iround(31.5f + sinf(a2) * r * 0.9f), r < 18.0f ? D_G0 : D_G2);
                    }
                }
            }
            halo(t, clampf(rt * 0.8f, 0, 1), D_G1);
            break;
        case R_BORED:
            brows(26, 26, 26, 26, 1);
            for (int k = 0; k < 3; k++) {
                if (rt > 0.5f + (float)k * 0.6f) {
                    px(47 + k * 3, 14, D_G1);
                }
            }
            break;
        case R_DISGUST:
            brows(26, 28, 26, 28, 1);
            for (int x = 28; x <= 37; x++) {
                px(x, 33 + ((x >> 1) & 1), IDX_LID);
            }
            break;
        case R_TARGET: {
            float k = clampf(rt / 1.2f, 0, 1);
            int sz = 28 - (int)(k * 19.0f);
            int cx = 31, cy = 30;
            uint8_t c = rt > 1.3f ? D_G0 : D_WHITE;
            for (int sx = -1; sx <= 1; sx += 2) {
                for (int sy = -1; sy <= 1; sy += 2) {
                    int x = cx + sx * sz, y = cy + sy * sz;
                    ln(x, y, x - sx * 5, y, c);
                    ln(x, y, x, y - sy * 5, c);
                }
            }
            if (rt > 1.2f) {                                   /* locked: the cross holds and pulses */
                uint8_t cc = (((int)(rt * 8.0f)) & 1) == 0 ? D_WHITE : D_G0;
                ln(cx - 14, cy, cx - 6, cy, cc); ln(cx + 6, cy, cx + 14, cy, cc);
                ln(cx, cy - 14, cx, cy - 6, cc); ln(cx, cy + 6, cx, cy + 14, cc);
                for (int sx = -1; sx <= 1; sx += 2) {
                    for (int sy = -1; sy <= 1; sy += 2) {
                        ln(cx + sx * sz, cy + sy * sz, cx + sx * (sz - 5), cy + sy * sz, cc);
                        ln(cx + sx * sz, cy + sy * sz, cx + sx * sz, cy + sy * (sz - 5), cc);
                    }
                }
            }
            break;
        }
        case R_NERVOUS: {
            int y = 20 + (int)fmodf(rt * 9.0f, 20.0f);
            px(45, y, D_G0); px(44, y + 1, D_G0); px(45, y + 1, D_WHITE); px(46, y + 1, D_G0); px(45, y + 2, D_G0);
            brows(24, 26, 24, 26, 1);
            break;
        }
        case R_PROUD:
            brows(25, 24, 25, 24, 1);
            halo(t * 0.5f, 0.35f + 0.3f * sinf(t * 3.0f), D_G1);
            break;
        case R_EXCITED:                                        /* sparkles orbiting the head */
            for (int k = 0; k < 6; k++) {
                float a = (float)k * TAU / 6.0f + t * 3.0f;
                px(iround(31.5f + cosf(a) * 24.0f), iround(30.0f + sinf(a) * 24.0f), k & 1 ? D_WHITE : D_G0);
            }
            halo(t, 0.4f + 0.4f * fabsf(sinf(t * 6.0f)), D_G1);
            break;
        case R_SUNNY: {                                        /* a rotating-ray sun, top left */
            int sx = 13, sy = 13;
            for (int i = 0; i < 12; i++) {
                float a = (float)i * TAU / 12.0f + t * 1.4f;
                for (int r = 3; r <= 7; r++) {
                    px(iround(sx + cosf(a) * r), iround(sy + sinf(a) * r * 0.9f), r < 6 ? D_G0 : D_G2);
                }
            }
            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    if (dx * dx + dy * dy <= 4 && ((dx + dy) & 1) == 0) {
                        px(sx + dx, sy + dy, D_WHITE);
                    }
                }
            }
            break;
        }
        case R_RAIN:                                           /* falling drops */
            for (int i = 0; i < 22; i++) {
                int x = 4 + (int)(hash2(i, 1) % 56u);
                int y = (int)fmodf(t * 40.0f + (float)(hash2(i, 2) % 40u), 46.0f) + 8;
                px(x, y, D_G0); px(x, y - 1, D_G1);
            }
            break;
        case R_STORM:                                          /* a bolt plus driving drops */
            lightning(t, D_WHITE, D_G0);
            for (int i = 0; i < 12; i++) {
                int x = 4 + (int)(hash2(i, 5) % 56u);
                int y = (int)fmodf(t * 46.0f + (float)(hash2(i, 6) % 40u), 46.0f) + 8;
                px(x, y, D_G2);
            }
            break;
        case R_COLD:                                           /* drifting snow */
            for (int i = 0; i < 20; i++) {
                int x = 4 + (int)(hash2(i, 7) % 56u);
                int y = (int)fmodf(t * 18.0f + (float)(hash2(i, 8) % 40u), 46.0f) + 8;
                px(x, y, D_G0);
            }
            px(28, 34, D_G0); px(34, 34, D_G0);                /* a puff at the grille */
            break;
        case R_RAINBOW:                                        /* five arcs behind the head */
            for (int band = 0; band < 5; band++) {
                uint8_t c = band == 4 ? D_ACC : (band & 1 ? D_G0 : D_G2);
                float r = 20.0f + (float)band * 2.0f;
                for (int i = 0; i < 120; i++) {
                    float a = (float)i * TAU / 120.0f;
                    if (sinf(a) > 0.25f) {
                        continue;                              /* the upper arc only */
                    }
                    px(iround(31.5f + cosf(a) * r), iround(44.0f + sinf(a) * r), c);
                }
            }
            break;
        case R_CONFETTI:                                       /* falling specks */
            for (int i = 0; i < 26; i++) {
                int x = (int)fmodf(hash2(i, 9) * 60.0f + sinf(t * 2.0f + (float)i) * 3.0f + 60.0f, 60.0f) + 2;
                int y = (int)fmodf(t * 28.0f + (float)(hash2(i, 10) % 50u), 56.0f) + 4;
                px(x, y, (i % 3 == 0) ? D_WHITE : (i & 1 ? D_G0 : D_G2));
            }
            break;
        default:
            break;
        }
        if (rid == R_GLITCH && (tick & 3) == 0) {
            for (int e = 0; e < 2; e++) {
                for (int i = 0; i < s_eye[e].n; i++) {
                    px(s_eye[e].x[i] + 2, s_eye[e].y[i], D_G3);
                    px(s_eye[e].x[i] - 2, s_eye[e].y[i], D_WHITE);
                }
            }
        }
    }
    /* A slow breath, a hop when petted, a shake on error. */
    int dy = iround(sinf(t * 1.4f) * 0.6f) - (happy > 0 ? (int)(fabsf(sinf(t * 9.0f)) * 2.5f * happy) : 0);
    if (rid == R_SURPRISED && rt < 0.35f) {
        dy -= 2;
    }
    if (rid == R_LAUGH) {
        dy += ((int)(t * 11.0f)) & 1 ? -1 : 0;
    } else if (rid == R_SLEEPY && rt > 0.8f) {
        dy += 1 + (rt > 2.0f ? 1 : 0);
    } else if (rid == R_PROUD) {
        dy -= 1;
    } else if (rid == R_BORED) {
        dy += 1;
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
        glitch(5, 6, 4, 60);
    } else if (mode == MUSE_MODE_SPEAKING) {
        glitch(1 + (int)(level * 3.0f), 2 + (int)(level * 2.0f), 6, 58);
    } else if (mode == MUSE_MODE_BOOT) {
        glitch((int)((1.0f - boot) * 6.0f), 5, 4, 60);
    } else if (happy > 0.2f) {
        glitch(2, 3, 6, 58);
    } else if (fmodf(t, 6.3f) < 0.12f) {
        glitch(1, 2, 6, 58);   /* now and then, even at rest */
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
