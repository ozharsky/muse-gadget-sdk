// Portrait faces: one AI-rendered portrait per agent, brought to life on the glass.
//   Photo faces (Muse, Hermes): patches cut from AI-edited variants of the portrait -- blink,
//     glances, wide/half/angry eyes, a raised brow, a wink, glowing eyes, smile, smirk, laugh, sad,
//     three mouth openings (tools/muse/portrait_pack.py) -- blended by weight through soft masks.
//   Screen faces (BEEPBOP): the robot's face is drawn here, glowing on its blank screen -- ring,
//     ^^, heart, star, X and sleepy eyes; smile, grin, O, wavy and frown mouths; brows, blush,
//     tears, sweat and sparkles; it floats and bounces inside its screen.
// An expression controller drives both from the mode, the voice (muse_lip_agc), idle habits and
// the relay's reactions (RECIPES). Each frame reports exactly what changed (muse_portrait_dirty),
// so the UI redraws only that.
// The packs live in the assets partition (entries pt_<name>): patches are read in place (mmap),
// the portrait is decoded once into PSRAM. Everything runs on the LVGL task; the draw units only read.

#include "muse_portrait.h"

#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "muse_assets.h"
#include "muse_lip.h"

static const char *TAG = "portrait";
static const char *const NAMES[3] = { "pt_muse", "pt_hermes", "pt_beepbop" };

/* Variant tags: the order of VARIANTS in portrait_pack.py, which is also the order patches apply. */
enum {
    V_EYES_HALF, V_EYES_LEFT, V_EYES_RIGHT, V_EYES_UP, V_EYES_DOWN, V_EYES_WIDE, V_EYES_ANGRY, V_BROW_RAISE,
    V_WINK, V_EYES_GLOW, V_EYES_CLOSED, V_SAD, V_LAUGH, V_SMILE, V_SMIRK, V_MOUTH_1, V_MOUTH_2, V_MOUTH_3,
    V_COUNT, V_NONE = 0xff
};
#define MAX_PATCHES 20

/* The pack header, as portrait_pack.py writes it (HDR there). */
typedef struct __attribute__((packed)) {
    uint8_t tag, pad;
    uint16_t x, y, w, h;          /* the patch, in portrait px */
    uint32_t mask_off;            /* w*h soft mask, 0..255 */
    uint32_t var_off;             /* w*h RGB565 */
} pack_patch_t;

typedef struct __attribute__((packed)) {
    char magic[4];                /* "MPT1" */
    uint16_t size;                /* the portrait is size x size */
    uint8_t kind, npatch;         /* kind 0 = photo face, 1 = screen face */
    uint32_t jpg_off, jpg_len;
    int16_t eye_l[2], eye_r[2], eye_rad, brow_dy, mouth[2], mouth_w;   /* screen face */
    uint32_t glow;
    int16_t clip[4];              /* x0, y0, x1, y1: where the screen face may glow */
    pack_patch_t p[MAX_PATCHES];
    uint32_t light_off, phase_off;   /* photo faces: where the portrait glows (size^2), how far from the eyes ((size/2)^2) */
    uint32_t light_col;              /* and its colour */
} pack_hdr_t;
_Static_assert(sizeof(pack_hdr_t) == 418, "pack header: keep in step with HDR in tools/muse/portrait_pack.py");

/* ---- Reactions ------------------------------------------------------------------------ */

enum { L_NONE, L_SEARCH, L_DOWN, L_UP, L_SIDE, L_SHAKE, L_NOD, L_READ, L_SCAN };   /* where the eyes go */
enum { FX_NONE, FX_GLITCH, FX_PULSE, FX_BOUNCE, FX_HUE };                   /* extra motion */
enum { E_RING, E_WIDE, E_SQUINT, E_WINK, E_HAPPY, E_HEART, E_STAR, E_CROSS, E_LINE };   /* screen eyes */
enum { M_SMILE, M_BIG, M_FLAT, M_FROWN, M_SMIRK, M_O, M_GRIN, M_WAVY, M_DOTS };         /* screen mouths */
enum { X_BLUSH = 1, X_TEAR = 2, X_SWEAT = 4, X_SPARKLE = 8, X_BROW1 = 16, X_BROWS = 32 };

typedef struct {
    const char *name;
    float dur;                    /* s */
    uint8_t v[3], w[3];           /* photo: variants and weights (tenths) */
    uint8_t mo;                   /* photo: mouth open, tenths */
    uint8_t look, fx;
    uint8_t eyes, mouth, extra;   /* screen */
    int8_t tilt;                  /* screen brows, tenths: + angry, - sad */
    uint32_t col;                 /* screen glow, 0 = its own */
} recipe_t;

#define V3(a, b, c) { V_##a, V_##b, V_##c }
#define V_ V_NONE
static const recipe_t RECIPES[] = {
    { "smirk", 2.8f, V3(SMIRK, , ), { 10 }, 0, L_NONE, FX_NONE, E_RING, M_SMIRK, X_BROW1, 0, 0 },
    { "scan", 2.6f, V3(EYES_GLOW, , ), { 8 }, 0, L_SCAN, FX_NONE, E_SQUINT, M_FLAT, 0, 0, 0x3ee8ff },
    { "alert", 2.2f, V3(EYES_WIDE, EYES_GLOW, ), { 10, 8 }, 0, L_NONE, FX_NONE, E_WIDE, M_O, X_BROWS, 0, 0xffd23e },
    { "glitch", 1.8f, V3(EYES_GLOW, , ), { 5 }, 0, L_SHAKE, FX_GLITCH, E_CROSS, M_WAVY, 0, 0, 0xff3a3a },
    { "squint", 2.6f, V3(EYES_HALF, EYES_ANGRY, ), { 8, 3 }, 0, L_NONE, FX_NONE, E_SQUINT, M_FLAT, 0, 5, 0 },
    { "angry", 2.6f, V3(EYES_ANGRY, EYES_GLOW, ), { 10, 6 }, 0, L_NONE, FX_NONE, E_RING, M_FROWN, 0, 10, 0xff3a3a },
    { "surprised", 1.8f, V3(EYES_WIDE, , ), { 10 }, 8, L_NONE, FX_NONE, E_WIDE, M_O, X_BROWS, 0, 0 },
    { "pleased", 2.6f, V3(SMILE, EYES_HALF, ), { 10, 3 }, 0, L_NONE, FX_NONE, E_HAPPY, M_SMILE, X_BLUSH, 0, 0 },
    { "laugh", 2.4f, V3(LAUGH, , ), { 10 }, 0, L_NONE, FX_BOUNCE, E_HAPPY, M_GRIN, X_BLUSH, 0, 0 },
    { "wink", 1.6f, V3(WINK, SMIRK, ), { 10, 6 }, 0, L_NONE, FX_NONE, E_WINK, M_SMILE, 0, 0, 0 },
    { "sleepy", 3.4f, V3(EYES_HALF, , ), { 10 }, 0, L_DOWN, FX_NONE, E_LINE, M_FLAT, 0, 0, 0x7a9a6a },
    { "confused", 2.6f, V3(BROW_RAISE, , ), { 10 }, 0, L_SEARCH, FX_NONE, E_RING, M_WAVY, X_BROW1, 0, 0 },
    { "evil", 3.0f, V3(SMIRK, EYES_ANGRY, EYES_GLOW), { 10, 6, 10 }, 0, L_NONE, FX_NONE, E_SQUINT, M_SMIRK, 0, 8, 0xff3a3a },
    { "hacked", 2.4f, V3(EYES_GLOW, , ), { 10 }, 0, L_SHAKE, FX_GLITCH, E_CROSS, M_WAVY, 0, 0, 0xff3a3a },
    { "powerup", 2.4f, V3(EYES_GLOW, EYES_WIDE, ), { 10, 5 }, 0, L_NONE, FX_PULSE, E_STAR, M_BIG, X_SPARKLE, 0, 0xffe14d },
    { "bored", 3.2f, V3(EYES_HALF, , ), { 8 }, 0, L_SIDE, FX_NONE, E_SQUINT, M_FLAT, 0, 0, 0x8a9a8a },
    { "disgust", 2.4f, V3(EYES_ANGRY, SMIRK, EYES_HALF), { 6, 5, 3 }, 0, L_NONE, FX_NONE, E_SQUINT, M_WAVY, 0, 5, 0xb5c93a },
    { "target", 2.6f, V3(EYES_HALF, EYES_GLOW, ), { 6, 10 }, 0, L_NONE, FX_PULSE, E_SQUINT, M_FLAT, 0, 3, 0xff3a3a },
    { "nervous", 2.6f, V3(EYES_WIDE, BROW_RAISE, ), { 6, 5 }, 0, L_SEARCH, FX_NONE, E_RING, M_WAVY, X_SWEAT, -5, 0x9fe8ff },
    { "proud", 2.8f, V3(SMILE, EYES_HALF, EYES_UP), { 8, 4, 3 }, 0, L_NONE, FX_NONE, E_HAPPY, M_BIG, X_SPARKLE, 0, 0xffc94d },
    { "excited", 2.8f, V3(LAUGH, , ), { 10 }, 0, L_NONE, FX_BOUNCE, E_STAR, M_GRIN, X_SPARKLE, 0, 0 },
    { "confetti", 3.4f, V3(LAUGH, EYES_GLOW, ), { 10, 6 }, 0, L_NONE, FX_HUE, E_HAPPY, M_GRIN, X_SPARKLE, 0, 0 },
    { "sunny", 3.0f, V3(SMILE, EYES_HALF, ), { 8, 3 }, 0, L_UP, FX_NONE, E_HAPPY, M_SMILE, X_SPARKLE, 0, 0xffd23e },
    { "rain", 3.2f, V3(SAD, , ), { 10 }, 0, L_UP, FX_NONE, E_RING, M_FROWN, X_TEAR, -6, 0x5aa8ff },
    { "storm", 3.2f, V3(EYES_ANGRY, EYES_WIDE, ), { 5, 4 }, 0, L_SHAKE, FX_PULSE, E_WIDE, M_WAVY, X_SWEAT, 4, 0xa064ff },
    { "cold", 3.0f, V3(EYES_HALF, EYES_ANGRY, ), { 5, 2 }, 0, L_SHAKE, FX_NONE, E_SQUINT, M_WAVY, X_BLUSH, -3, 0x9fe8ff },
    { "rainbow", 3.2f, V3(SMILE, EYES_WIDE, ), { 10, 4 }, 0, L_UP, FX_HUE, E_STAR, M_BIG, X_SPARKLE, 0, 0 },
    { "searching", 2.6f, V3(, , ), { 0 }, 0, L_SEARCH, FX_NONE, E_RING, M_FLAT, 0, 0, 0 },
    { "browsing", 3.0f, V3(EYES_DOWN, , ), { 4 }, 0, L_SCAN, FX_NONE, E_RING, M_SMILE, 0, 0, 0 },
    { "reading", 3.0f, V3(EYES_DOWN, , ), { 10 }, 0, L_READ, FX_NONE, E_SQUINT, M_FLAT, 0, 0, 0 },
    { "writing", 3.0f, V3(EYES_DOWN, EYES_HALF, ), { 10, 3 }, 0, L_DOWN, FX_NONE, E_SQUINT, M_SMIRK, X_BROW1, 0, 0 },
    { "checking", 2.4f, V3(EYES_HALF, SMILE, ), { 3, 4 }, 0, L_NOD, FX_NONE, E_RING, M_SMILE, 0, 0, 0 },
    { "calculating", 3.0f, V3(EYES_UP, EYES_GLOW, ), { 8, 5 }, 0, L_UP, FX_PULSE, E_RING, M_DOTS, 0, 0, 0xa064ff },
    { "calendar", 3.0f, V3(EYES_UP, EYES_GLOW, ), { 6, 4 }, 0, L_UP, FX_NONE, E_RING, M_DOTS, X_BROWS, 0, 0 },
    { "monitoring", 2.8f, V3(EYES_HALF, EYES_GLOW, ), { 5, 7 }, 0, L_SCAN, FX_NONE, E_SQUINT, M_FLAT, 0, 0, 0x3ee8ff },
    { "generating", 3.0f, V3(EYES_UP, EYES_GLOW, ), { 5, 6 }, 0, L_UP, FX_PULSE, E_STAR, M_DOTS, 0, 0, 0xa064ff },
    { "running", 3.0f, V3(EYES_GLOW, EYES_WIDE, ), { 8, 3 }, 0, L_NONE, FX_PULSE, E_RING, M_FLAT, 0, 0, 0 },
    { "invoking", 2.8f, V3(EYES_GLOW, EYES_UP, ), { 10, 6 }, 0, L_UP, FX_PULSE, E_STAR, M_O, X_SPARKLE, 0, 0xa064ff },
    { "delegating", 2.8f, V3(SMIRK, , ), { 4 }, 0, L_SIDE, FX_NONE, E_RING, M_SMIRK, 0, 0, 0 },
    { "remembering", 2.8f, V3(EYES_UP, EYES_HALF, ), { 10, 3 }, 0, L_UP, FX_NONE, E_RING, M_DOTS, 0, 0, 0 },
    { "shrug", 2.6f, V3(BROW_RAISE, SMIRK, ), { 10, 5 }, 0, L_NONE, FX_NONE, E_RING, M_WAVY, X_BROWS, 0, 0 },
    { "embarrassed", 2.8f, V3(SMILE, EYES_DOWN, ), { 6, 6 }, 0, L_DOWN, FX_NONE, E_HAPPY, M_SMILE, X_BLUSH | X_SWEAT, 0, 0 },
    { "scared", 2.6f, V3(EYES_WIDE, BROW_RAISE, ), { 10, 6 }, 3, L_SHAKE, FX_NONE, E_WIDE, M_WAVY, X_SWEAT, -6, 0 },
    { "cool", 2.8f, V3(SMIRK, EYES_HALF, ), { 8, 6 }, 0, L_NONE, FX_NONE, E_LINE, M_SMIRK, 0, 0, 0x3ee8ff },
    { "mind_blown", 2.8f, V3(EYES_WIDE, EYES_GLOW, ), { 10, 10 }, 9, L_NONE, FX_GLITCH, E_STAR, M_O, X_SPARKLE, 0, 0 },
    { "crying_laughing", 2.8f, V3(LAUGH, , ), { 10 }, 0, L_NONE, FX_BOUNCE, E_HAPPY, M_GRIN, X_TEAR | X_BLUSH, 0, 0 },
    { "sad", 3.0f, V3(SAD, , ), { 10 }, 0, L_DOWN, FX_NONE, E_RING, M_FROWN, X_TEAR, -8, 0x5aa8ff },
    { "nod", 2.4f, V3(SMILE, , ), { 5 }, 0, L_NOD, FX_NONE, E_HAPPY, M_SMILE, 0, 0, 0 },
    { "cloudy", 3.0f, V3(EYES_HALF, EYES_UP, ), { 4, 4 }, 0, L_UP, FX_NONE, E_RING, M_FLAT, 0, 0, 0xa8b4c8 },
    { "windy", 3.0f, V3(EYES_HALF, , ), { 5 }, 0, L_SIDE, FX_NONE, E_SQUINT, M_WAVY, 0, 0, 0 },
    { "hot", 3.0f, V3(EYES_HALF, , ), { 7 }, 0, L_NONE, FX_NONE, E_SQUINT, M_WAVY, X_SWEAT | X_BLUSH, 0, 0xff8a3e },
    { "happy", 2.6f, V3(SMILE, EYES_HALF, ), { 10, 2 }, 0, L_NONE, FX_BOUNCE, E_HAPPY, M_BIG, X_BLUSH, 0, 0 },
    { "love", 2.8f, V3(SMILE, EYES_GLOW, ), { 10, 6 }, 0, L_NONE, FX_BOUNCE, E_HEART, M_BIG, X_BLUSH | X_SPARKLE, 0, 0xff6aa8 },
    { "snow", 3.4f, V3(EYES_WIDE, SMILE, ), { 5, 5 }, 0, L_UP, FX_NONE, E_STAR, M_O, X_SPARKLE, 0, 0xdff4ff },
};
#undef V_
#define NRECIPES ((int)(sizeof(RECIPES) / sizeof(RECIPES[0])))

/* ---- State ---------------------------------------------------------------------------- */

typedef struct {
    int tag, x, y, w, h;
    const uint8_t *mask;
    const uint16_t *var;
} patch_t;

/* Screen face, this frame (portrait px). Styles switch; the numbers ease. */
typedef struct {
    int eyes, mouth;
    float lookx, looky, open_l, open_r, sx, sy, brow_l, brow_r, tilt, talk, curve, bob;
    float blush, tear, sweat, sparkle, dots;
} screen_t;

static volatile bool s_enabled = true;
static volatile int s_react_req = -1;
static bool s_active;
static int s_face = -1, s_failed = -1;
static float s_retry_at;
static muse_asset_t *s_asset;     /* over 1 KB: PSRAM, allocated once */
static esp_partition_mmap_handle_t s_map;
static bool s_mapped;
static const uint8_t *s_pack;
static uint16_t *s_base;
static uint16_t *s_frame;         /* the face as last composed: redraws (swipes, labels over it) only copy */
static pack_hdr_t s_h;
static patch_t s_p[MAX_PATCHES];
static int s_np;
static bool s_has[V_COUNT];       /* the loaded pack has this variant */
static int s_size = 384;          /* the loaded portrait's size */
static int s_px = 384;            /* the canvas */
static int16_t s_xmap[512];       /* canvas x -> portrait x, when the canvas is shrunk */

static float s_w[V_COUNT];        /* weights, 0..1 */
static int s_wq[V_COUNT];         /* this frame's, 0..256 */
static int s_drawn[MAX_PATCHES];  /* each patch's weight as last reported dirty */
static bool s_full_dirty = true, s_was_glitch;
static int s_drawn_px;
static float s_last_t, s_now, s_next_blink, s_blink_t0 = -1.0f;
static float s_glitch, s_fx_glitch;
static int8_t s_shift[64];        /* glitch: per 8-row band */
static muse_lip_agc_t s_agc;
static muse_mode_t s_prev_mode = MUSE_MODE_COUNT;

static int s_react = -1;          /* the reaction playing, or -1 */
static float s_react_t0;

/* idle habits */
static int s_glance = -1;         /* a V_EYES_* glance, or -1 */
static float s_glance_until, s_next_glance, s_next_mood, s_mood_until, s_reply_end = -10.0f, s_kick;
static int s_mood = -1;

static screen_t s_sf = { .open_l = 1, .open_r = 1, .sx = 1, .sy = 1, .curve = 1 };
static float s_tlookx, s_tlooky, s_next_look, s_wink_until, s_next_wink, s_curious_until, s_next_curious;
static float s_joy_until, s_next_joy;
static int s_wink_eye;
static float s_gr, s_gg, s_gb;    /* the light's colour, eased */
static float s_cr, s_cg, s_cb;    /* and as drawn this frame (a pulse dims it) */
/* The screen face is drawn once a frame, at half size, into these (0..255, 1.0 = 200). */
static uint8_t *s_lg, *s_lw, *s_lp;
static int s_lw_n, s_lh_n;        /* half-size grid */
static uint32_t s_light_key;
static bool s_light_dirty;
/* Photo faces' own light (glowing eyes, cables, seams): waves of brightness travel out from the
 * eyes. Built per frame as a table, used per pixel. */
static const uint8_t *s_lmap, *s_pmap;
static int s_ebox[4];             /* the eyes' box (always animating) */
static int s_lbands[4][4];        /* where the rest of the light is, in four bands of rows (-1: none) */
static int s_lframe;
static struct {
    float base, amp, waves, flicker, phase;
} s_lt;
static uint8_t s_wave[256];       /* how bright the light is, by distance from the eyes */
static uint16_t s_hot565;         /* where it brightens to: its own colour, lighter */
#define BELL_N 128                /* the light's falloff table, over q^2 in [0, 32) */
static float s_bell[BELL_N + 2];
static void bell_init(void);

static float rnd(void)
{
    return (float)(esp_random() & 0xffffff) / 16777216.0f;
}

static float approach(float v, float target, float rate, float dt)
{
    float k = rate * dt;
    return v + (target - v) * (k > 1 ? 1 : k);
}

static float clamp01(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

void muse_portrait_enable(bool on)
{
    s_enabled = on;
    s_failed = -1;
}

bool muse_portrait_on(void)
{
    return s_active;
}

static void map_x(void)
{
    for (int i = 0; i < s_px && i < 512; i++) {
        s_xmap[i] = (int16_t)(i * s_size / s_px);
    }
}

void muse_portrait_set_size(int px)
{
    s_px = px < 1 ? 1 : px > 512 ? 512 : px;
    map_x();
}

void muse_portrait_redraw_all(void)
{
    s_full_dirty = true;
}

bool muse_portrait_react(const char *name)
{
    for (int i = 0; name && i < NRECIPES; i++) {
        if (!strcmp(name, RECIPES[i].name)) {
            s_react_req = i;
            return true;
        }
    }
    return false;
}

static inline uint16_t rgb565f(float r, float g, float b)
{
    int ri = (int)r, gi = (int)g, bi = (int)b;
    ri = ri < 0 ? 0 : ri > 255 ? 255 : ri;
    gi = gi < 0 ? 0 : gi > 255 ? 255 : gi;
    bi = bi < 0 ? 0 : bi > 255 ? 255 : bi;
    return (uint16_t)(((ri >> 3) << 11) | ((gi >> 2) << 5) | (bi >> 3));
}

/* ---- Loading -------------------------------------------------------------------------- */

static void unload(void)
{
    heap_caps_free(s_frame);
    s_frame = NULL;
    heap_caps_free(s_base);
    heap_caps_free(s_lg);
    s_base = NULL;
    s_lg = s_lw = s_lp = NULL;
    if (s_mapped) {
        esp_partition_munmap(s_map);
        s_mapped = false;
    }
    s_pack = NULL;
    s_face = -1;
    s_np = 0;
    memset(s_has, 0, sizeof(s_has));
}

static bool decode(const uint8_t *in, uint32_t len, uint16_t *out, int size)
{
    jpeg_dec_config_t cfg = { .output_type = JPEG_PIXEL_FORMAT_RGB565_LE };
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return false;
    }
    /* parse and process on the same io (a fresh one runs out of data) */
    jpeg_dec_io_t io = { .inbuf = (uint8_t *)in, .inbuf_len = (int)len, .outbuf = (uint8_t *)out };
    jpeg_dec_header_info_t info = { 0 };
    jpeg_error_t err = jpeg_dec_parse_header(dec, &io, &info);
    if (err == JPEG_ERR_OK && (info.width != size || info.height != size)) {
        err = JPEG_ERR_INVALID_PARAM;
    }
    if (err == JPEG_ERR_OK) {
        err = jpeg_dec_process(dec, &io);
    }
    jpeg_dec_close(dec);
    if (err != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "portrait decode failed (%d)", err);
    }
    return err == JPEG_ERR_OK;
}

static bool load(int face)
{
    unload();
    if (s_bell[0] == 0) {
        bell_init();
    }
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x42, "assets");
    if (!s_asset) {
        s_asset = heap_caps_malloc(sizeof(*s_asset), MALLOC_CAP_SPIRAM);
    }
    if (!part || !s_asset || !muse_assets_open(NAMES[face], s_asset) || s_asset->fmt != 3) {
        ESP_LOGW(TAG, "no %s in the asset pack", NAMES[face]);
        return false;
    }
    uint32_t at = s_asset->data_off + s_asset->offs[0];
    size_t len = s_asset->offs[1] - s_asset->offs[0];
    const void *map;
    if (len < sizeof(s_h) || esp_partition_mmap(part, at, len, ESP_PARTITION_MMAP_DATA, &map, &s_map) != ESP_OK) {
        return false;
    }
    s_mapped = true;
    s_pack = map;
    memcpy(&s_h, s_pack, sizeof(s_h));
    if (((uintptr_t)s_pack & 3) || memcmp(s_h.magic, "MPT1", 4) || !s_h.size || s_h.size > 512 ||
        (size_t)s_h.jpg_off + s_h.jpg_len > len) {
        unload();
        return false;
    }
    int size = s_h.size;
    /* The JPEG is decoded from RAM: decoding straight from the mapped flash failed now and then (-3). */
    uint8_t *jpg = heap_caps_malloc(s_h.jpg_len, MALLOC_CAP_SPIRAM);
    s_base = heap_caps_aligned_alloc(16, (size_t)size * size * sizeof(uint16_t), MALLOC_CAP_SPIRAM);   /* the decoder wants 16 */
    bool ok = jpg && s_base && esp_partition_read(part, at + s_h.jpg_off, jpg, s_h.jpg_len) == ESP_OK &&
              decode(jpg, s_h.jpg_len, s_base, size);
    heap_caps_free(jpg);
    if (!ok) {
        unload();
        return false;
    }
    s_frame = heap_caps_calloc((size_t)size * size, sizeof(uint16_t), MALLOC_CAP_SPIRAM);   /* NULL: compose per strip */
    for (int i = 0; i < s_h.npatch && i < MAX_PATCHES; i++) {
        const pack_patch_t *pp = &s_h.p[i];
        size_t area = (size_t)pp->w * pp->h;
        if (pp->tag >= V_COUNT || pp->x + pp->w > size || pp->y + pp->h > size || pp->mask_off + area > len ||
            pp->var_off + area * 2 > len || (pp->var_off & 1)) {
            continue;
        }
        s_has[pp->tag] = true;
        s_p[s_np++] = (patch_t){ .tag = pp->tag, .x = pp->x, .y = pp->y, .w = pp->w, .h = pp->h,
                                 .mask = s_pack + pp->mask_off, .var = (const uint16_t *)(s_pack + pp->var_off) };
    }
    s_lmap = s_pmap = NULL;
    if (s_h.kind == 0 && s_h.light_off && (size_t)s_h.light_off + (size_t)size * size <= len &&
        (size_t)s_h.phase_off + (size_t)(size / 2) * (size / 2) <= len) {
        s_lmap = s_pack + s_h.light_off;
        s_pmap = s_pack + s_h.phase_off;
        /* the redraw areas: the eyes' box, and the lit cells of four bands of rows */
        s_ebox[0] = size;
        s_ebox[1] = size;
        s_ebox[2] = s_ebox[3] = -1;
        for (int i = 0; i < s_np; i++) {
            if (s_p[i].tag <= V_EYES_CLOSED) {
                const patch_t *pt = &s_p[i];
                s_ebox[0] = pt->x < s_ebox[0] ? pt->x : s_ebox[0];
                s_ebox[1] = pt->y < s_ebox[1] ? pt->y : s_ebox[1];
                s_ebox[2] = pt->x + pt->w - 1 > s_ebox[2] ? pt->x + pt->w - 1 : s_ebox[2];
                s_ebox[3] = pt->y + pt->h - 1 > s_ebox[3] ? pt->y + pt->h - 1 : s_ebox[3];
            }
        }
        for (int bnd = 0; bnd < 4; bnd++) {
            int *r = s_lbands[bnd];
            r[0] = size;
            r[1] = size;
            r[2] = r[3] = -1;
            for (int y = bnd * size / 4; y < (bnd + 1) * size / 4; y++) {
                for (int x = 0; x < size; x++) {
                    if (s_lmap[y * size + x] > 24) {
                        r[0] = x < r[0] ? x : r[0];
                        r[1] = y < r[1] ? y : r[1];
                        r[2] = x > r[2] ? x : r[2];
                        r[3] = y > r[3] ? y : r[3];
                    }
                }
            }
        }
        uint32_t c = s_h.light_col;
        s_hot565 = rgb565f(((c >> 16) & 255) * 0.5f + 128, ((c >> 8) & 255) * 0.5f + 128, (c & 255) * 0.5f + 128);
    }
    if (s_h.kind == 1) {
        s_lw_n = (s_h.clip[2] - s_h.clip[0]) / 2 + 2;
        s_lh_n = (s_h.clip[3] - s_h.clip[1]) / 2 + 2;
        s_lg = heap_caps_calloc(3, (size_t)s_lw_n * s_lh_n, MALLOC_CAP_SPIRAM);
        if (!s_lg) {
            unload();
            return false;
        }
        s_lw = s_lg + s_lw_n * s_lh_n;
        s_lp = s_lw + s_lw_n * s_lh_n;
    }
    s_size = size;
    s_face = face;
    map_x();
    return true;
}

/* ---- Expressions ---------------------------------------------------------------------- */

static const uint32_t IDLE_ACCENT[3] = { 0x4dff3c, 0x8f7bff, 0xb8ff2a };

uint32_t muse_portrait_accent(int face, muse_mode_t mode)
{
    switch (mode) {
    case MUSE_MODE_BOOT: return 0x7dffb0;
    case MUSE_MODE_LISTENING: return 0x3ee8ff;
    case MUSE_MODE_THINKING: return 0xa064ff;
    case MUSE_MODE_ERROR: return 0xff3a3a;
    case MUSE_MODE_OFF: return 0x2a5a2a;
    default: return IDLE_ACCENT[face >= 0 && face < 3 ? face : 0];
    }
}

/* 0..1 while a blink closes and opens the eyes; now and then a quick double blink. */
static float blink(float now)
{
    if (now >= s_next_blink) {
        s_blink_t0 = now;
        s_next_blink = now + (rnd() < 0.2f ? 0.36f : 2.2f + rnd() * 4.0f);
    }
    float ph = (now - s_blink_t0) / 0.22f;
    if (ph < 0 || ph >= 1.0f) {
        return 0;
    }
    return ph < 0.4f ? ph / 0.4f : 1.0f - (ph - 0.4f) / 0.6f;   /* shut fast, open a little slower */
}

/* The playing reaction's strength: eases in, holds, eases out. 0 when none. */
static float react_env(float now)
{
    if (s_react < 0) {
        return 0;
    }
    float t = now - s_react_t0, d = RECIPES[s_react].dur;
    if (t >= d) {
        s_react = -1;
        return 0;
    }
    float in = t / 0.15f, out = (d - t) / 0.35f;
    return clamp01(in < out ? in : out);
}

/* Where a reaction sends the eyes: x, y in -1..1 (right, down). */
static void react_look(int look, float now, float *x, float *y)
{
    float t = now - s_react_t0;
    *x = *y = 0;
    switch (look) {
    case L_SEARCH: *x = sinf(t * 5.0f) > 0 ? 1.0f : -1.0f; break;
    case L_DOWN: *y = 1; *x = 0.3f * sinf(t * 2.0f); break;
    case L_UP: *y = -1; *x = 0.3f * sinf(t * 1.3f); break;
    case L_SIDE: *x = -1; break;
    case L_SHAKE: *x = sinf(t * 38.0f) * 0.35f; break;
    case L_NOD: *y = sinf(t * 7.0f) > 0 ? 0.8f : -0.4f; break;
    case L_READ: {   /* line by line: a slow sweep right, a quick return */
        float ph = fmodf(t, 0.9f) / 0.9f;
        *x = ph < 0.8f ? -0.8f + 2.0f * ph : 0.8f - 8.0f * (ph - 0.8f);
        *y = 0.7f + 0.1f * floorf(fmodf(t / 0.9f, 3.0f));
        break;
    }
    case L_SCAN: *x = sinf(t * 2.2f); *y = 0.15f * sinf(t * 1.1f); break;
    default: break;
    }
}

/* Photo faces: target weights from the mode, the voice, a reaction and idle habits; then eased. */
static void photo_frame(const muse_pose_t *p, float now, float dt, float b, float open)
{
    float t[V_COUNT] = { 0 };
    muse_mode_t m = p->mode;
    float env = react_env(now);
    const recipe_t *rc = s_react >= 0 ? &RECIPES[s_react] : NULL;

    /* glances: often while idle, around while thinking, rarely while talking, never while listening */
    if (m != MUSE_MODE_LISTENING && m != MUSE_MODE_OFF && now >= s_next_glance) {
        float r = rnd();
        bool thinking = m == MUSE_MODE_THINKING, talking = m == MUSE_MODE_SPEAKING;
        s_glance = thinking ? (r < 0.5f ? V_EYES_UP : r < 0.75f ? V_EYES_LEFT : V_EYES_RIGHT)
                            : (r < 0.4f ? V_EYES_LEFT : r < 0.8f ? V_EYES_RIGHT : r < 0.9f ? V_EYES_UP : V_EYES_DOWN);
        if (!s_has[s_glance]) {
            s_glance = r < 0.5f ? V_EYES_LEFT : V_EYES_RIGHT;   /* not every face has every direction */
        }
        s_glance_until = now + (talking ? 0.3f + rnd() * 0.5f : thinking ? 0.7f + rnd() : 0.5f + rnd() * 0.9f);
        s_next_glance = s_glance_until + (talking ? 2.0f + rnd() * 2.5f : thinking ? 0.4f + rnd() * 0.8f : 1.0f + rnd() * 2.5f);
    }
    if (m == MUSE_MODE_LISTENING || now >= s_glance_until) {
        s_glance = -1;
    }
    if (s_glance >= 0 && env < 0.5f) {
        t[s_glance] = 1.0f;
    }

    s_kick *= expf(-dt * 5.0f);
    switch (m) {
    case MUSE_MODE_LISTENING:
        t[V_EYES_WIDE] = 0.6f;
        t[V_EYES_GLOW] = 0.35f + 0.1f * sinf(now * 3.0f);
        t[V_BROW_RAISE] = 0.25f;
        break;
    case MUSE_MODE_THINKING:
        t[V_EYES_HALF] = 0.35f;
        t[V_EYES_GLOW] = 0.2f + 0.2f * sinf(now * 4.0f);
        break;
    case MUSE_MODE_SPEAKING:
        t[V_EYES_GLOW] = 0.1f + 0.55f * open;   /* the eyes flare with the voice */
        if (open > 0.85f && s_kick < 0.1f && rnd() < 0.12f) {
            s_kick = 0.5f;                       /* a brow-lift on a strong syllable, now and then */
        }
        t[V_BROW_RAISE] = s_kick * 0.6f;
        break;
    case MUSE_MODE_ERROR:
        t[V_EYES_ANGRY] = 1.0f;
        break;
    case MUSE_MODE_BOOT:
        t[V_EYES_GLOW] = 0.5f + 0.5f * sinf(now * 6.0f);
        break;
    default:   /* idle: a slow glow, and boredom after a long quiet */
        t[V_EYES_GLOW] = 0.2f + 0.16f * sinf(now * 1.3f);   /* the eyes breathe */
        if (p->mode_t > 45.0f && s_glance < 0) {
            t[V_EYES_HALF] = 0.45f;
        }
        break;
    }
    t[V_EYES_WIDE] = t[V_EYES_WIDE] > s_kick ? t[V_EYES_WIDE] : s_kick;

    /* moods: a smile, smirk, wink or raised brow now and then while idle; one after each reply */
    if (s_prev_mode == MUSE_MODE_SPEAKING && m != MUSE_MODE_SPEAKING) {
        s_reply_end = now;
    }
    if (m == MUSE_MODE_IDLE && now >= s_next_mood) {
        float r = rnd();
        s_mood = r < 0.35f ? V_SMIRK : r < 0.65f ? V_SMILE : r < 0.82f ? V_BROW_RAISE : V_WINK;
        s_mood_until = now + (s_mood == V_WINK ? 0.6f : 1.6f + rnd() * 1.2f);
        s_next_mood = s_mood_until + 5.0f + rnd() * 7.0f;
    }
    if (m == MUSE_MODE_IDLE && now < s_mood_until && s_mood >= 0 && env < 0.5f) {
        t[s_mood] = 0.9f;
    }
    if (now - s_reply_end < 1.8f) {
        t[s_face == 0 ? V_SMIRK : V_SMILE] = 0.7f;   /* SHODAN is pleased with herself */
    }
    if (p->happy > 0.3f) {
        t[V_SMILE] = 1.0f;
        t[V_EYES_HALF] = 0.25f;
    }

    /* a reaction takes the face over while it plays */
    float mouth = m == MUSE_MODE_SPEAKING ? open : 0;
    if (rc) {
        for (int i = 0; i < 3; i++) {
            if (rc->v[i] != V_NONE) {
                float w = rc->w[i] / 10.0f;
                if (rc->fx == FX_PULSE && rc->v[i] == V_EYES_GLOW) {
                    w *= 0.55f + 0.45f * sinf(now * 9.0f);
                }
                t[rc->v[i]] += (w - t[rc->v[i]]) * env;
            }
        }
        float lx, ly;
        react_look(rc->look, now, &lx, &ly);
        if (rc->look != L_NONE) {
            for (int v = V_EYES_LEFT; v <= V_EYES_DOWN; v++) {
                t[v] *= 1.0f - env;
            }
            t[V_EYES_RIGHT] += env * clamp01(lx);
            t[V_EYES_LEFT] += env * clamp01(-lx);
            t[V_EYES_DOWN] += env * clamp01(ly);
            if (s_has[V_EYES_UP]) {
                t[V_EYES_UP] += env * clamp01(-ly);
            } else {
                t[V_BROW_RAISE] += env * clamp01(-ly) * 0.6f;
                t[V_EYES_HALF] += env * clamp01(-ly) * 0.35f;
            }
        }
        if (m != MUSE_MODE_SPEAKING) {
            mouth = mouth > rc->mo / 10.0f * env ? mouth : rc->mo / 10.0f * env;
        }
        s_fx_glitch = rc->fx == FX_GLITCH && now - s_react_t0 < 0.7f ? 0.5f * env : 0;
    } else {
        s_fx_glitch = 0;
    }

    /* the mouth: three openings chained, the widest only on the loudest syllables */
    if (mouth > 0.01f) {
        float pos = mouth * 2.4f;
        t[V_MOUTH_1] = pos;
        t[V_MOUTH_2] = pos - 1;
        t[V_MOUTH_3] = pos - 2;
        t[V_SMILE] *= 0.3f;
        t[V_SMIRK] *= 0.3f;
        t[V_SAD] *= 0.5f;
    }
    t[V_EYES_CLOSED] = b;
    float face = clamp01(t[V_LAUGH] > t[V_SAD] ? t[V_LAUGH] : t[V_SAD]);
    for (int v = V_EYES_HALF; v <= V_EYES_CLOSED; v++) {
        t[v] *= 1.0f - face;
    }
    for (int v = V_MOUTH_1; v <= V_MOUTH_3; v++) {
        t[v] *= 1.0f - clamp01(t[V_LAUGH]);   /* the laugh has its own open mouth */
    }

    static const float RATE[V_COUNT] = {   /* how fast each weight follows (1/s) */
        [V_EYES_HALF] = 8, [V_EYES_LEFT] = 16, [V_EYES_RIGHT] = 16, [V_EYES_UP] = 14, [V_EYES_DOWN] = 14,
        [V_EYES_WIDE] = 14, [V_EYES_ANGRY] = 10, [V_BROW_RAISE] = 10, [V_WINK] = 18, [V_EYES_GLOW] = 12,
        [V_EYES_CLOSED] = 1000, [V_SAD] = 8, [V_LAUGH] = 10, [V_SMILE] = 9, [V_SMIRK] = 9,
        [V_MOUTH_1] = 1000, [V_MOUTH_2] = 1000, [V_MOUTH_3] = 1000,   /* the lip follower already eases */
    };
    for (int v = 0; v < V_COUNT; v++) {
        s_w[v] = approach(s_w[v], clamp01(t[v]), RATE[v], dt);
        s_wq[v] = s_w[v] < 0.01f ? 0 : (int)(s_w[v] * 256.0f);
    }
}

static uint32_t hue_rgb(float h);


/* Photo faces: how their light behaves -- by mode, the voice, and a reaction while it plays. The
 * light keeps the portrait's own colour (a face doesn't change colour); moods show in how it
 * moves: its level, the waves' strength, speed and direction, and flicker. */
static void light_frame(const muse_pose_t *p, float now, float dt, float open, float env)
{
    if (!s_lmap) {
        return;
    }
    (void)now;
    typedef struct { float base, amp, speed, waves, flicker; } look_t;
    look_t m = { 0.0f, 0.9f, 0.45f, 3.0f, 0 };    /* idle: bright packets running out along the cords */
    switch (p->mode) {
    case MUSE_MODE_LISTENING: m = (look_t){ 0.1f, 0.5f, -0.9f, 3.0f, 0 }; break;      /* drawn in */
    case MUSE_MODE_THINKING: m = (look_t){ 0.1f, 0.6f, 1.8f, 5.0f, 0.15f }; break;    /* racing data */
    case MUSE_MODE_SPEAKING: m = (look_t){ 0.25f * open, 0.15f + 0.75f * open, 1.0f, 3.0f, 0 }; break;
    case MUSE_MODE_ERROR: m = (look_t){ 0, 0.5f, 0.5f, 2.0f, 0.6f }; break;            /* unsteady */
    case MUSE_MODE_OFF: m = (look_t){ 0, 0.05f, 0.1f, 1.0f, 0 }; break;
    case MUSE_MODE_BOOT: m = (look_t){ 0.1f, 0.8f, 1.4f, 4.0f, 0 }; break;
    default:
        if (p->mode_t > 45.0f) {
            m = (look_t){ 0, 0.15f, 0.12f, 2.0f, 0 };   /* bored: the light settles low */
        }
        break;
    }
    if (p->happy > 0.3f) {
        m = (look_t){ 0.15f, 0.6f, 0.8f, 3.0f, 0 };
    }
    if (s_react >= 0 && env > 0.01f) {
        const recipe_t *rc = &RECIPES[s_react];
        look_t r = { 0.05f, 0.45f, 0.6f, 3.0f, 0 };
        switch (rc->fx) {
        case FX_PULSE: r = (look_t){ 0.3f, 0.7f, 1.5f, 0, 0 }; break;    /* a throb, all at once */
        case FX_HUE: r = (look_t){ 0.2f, 0.8f, 2.0f, 4.0f, 0 }; break;   /* celebrations: quick bright waves */
        case FX_GLITCH: r.flicker = 0.8f; break;
        case FX_BOUNCE: r = (look_t){ 0.15f, 0.75f, 1.6f, 4.0f, 0 }; break;
        default: break;
        }
        if (rc->extra & X_TEAR) {   /* sad: dim and slow */
            r = (look_t){ 0, 0.15f, 0.12f, 2.0f, 0 };
        }
        if (rc->eyes == E_LINE) {   /* sleepy, cool: nearly dark */
            r = (look_t){ 0, 0.1f, 0.08f, 1.0f, 0 };
        }
        if (rc->eyes == E_CROSS || rc->tilt > 5) {   /* glitched, angry: a hard flicker */
            r.flicker = r.flicker > 0.5f ? r.flicker : 0.5f;
            r.amp = 0.6f;
        }
        if (rc->extra & X_SPARKLE) {
            r.flicker = r.flicker > 0.2f ? r.flicker : 0.2f;
        }
        float e = env;
        m.base += (r.base - m.base) * e;
        m.amp += (r.amp - m.amp) * e;
        m.speed += (r.speed - m.speed) * e;
        m.waves += (r.waves - m.waves) * e;
        m.flicker += (r.flicker - m.flicker) * e;
    }
    s_lt.base = approach(s_lt.base, m.base, p->mode == MUSE_MODE_SPEAKING ? 20 : 6, dt);
    s_lt.amp = approach(s_lt.amp, m.amp, p->mode == MUSE_MODE_SPEAKING ? 20 : 6, dt);
    s_lt.waves = approach(s_lt.waves, m.waves, 4, dt);
    s_lt.flicker = approach(s_lt.flicker, m.flicker, 8, dt);
    s_lt.phase += m.speed * dt;

    float fl = s_lt.flicker > 0.01f && rnd() < 0.5f ? 1.0f - s_lt.flicker * rnd() : 1.0f;
    for (int i = 0; i < 256; i++) {
        float c = cosf(6.2832f * ((float)i / 255.0f * s_lt.waves - s_lt.phase));
        float v = 0.5f + 0.5f * c, v2 = v * v;
        float in = (s_lt.base + s_lt.amp * v2 * v2 * v2) * fl;   /* v^6: short, distinct pulses, dark between */
        s_wave[i] = (uint8_t)(in >= 1.0f ? 255 : in <= 0 ? 0 : in * 255.0f);
    }
}

static uint32_t hue_rgb(float h)   /* h in turns; full saturation and value */
{
    h -= floorf(h);
    float r = clamp01(fabsf(h * 6.0f - 3.0f) - 1.0f), g = clamp01(2.0f - fabsf(h * 6.0f - 2.0f)),
          b = clamp01(2.0f - fabsf(h * 6.0f - 4.0f));
    return ((uint32_t)(r * 255) << 16) | ((uint32_t)(g * 255) << 8) | (uint32_t)(b * 255);
}

static void build_light(void);

/* Screen face: the shapes, where the eyes look, how open, the brows, the light's colour. */
static void screen_frame(const muse_pose_t *p, float now, float dt, float b, float open)
{
    float r = s_h.eye_rad, env = react_env(now);
    muse_mode_t m = p->mode;
    const recipe_t *rc = s_react >= 0 && env > 0.02f ? &RECIPES[s_react] : NULL;
    screen_t t = { .eyes = E_RING, .mouth = M_SMILE, .open_l = 1, .open_r = 1, .sx = 1, .sy = 1, .curve = 1 };
    uint32_t col = s_h.glow;

    if (now >= s_next_look) {   /* glances: somewhere, then home */
        bool home = s_tlookx != 0 || s_tlooky != 0;
        s_tlookx = home ? 0 : (rnd() - 0.5f) * r * 1.2f;
        s_tlooky = home ? 0 : (rnd() - 0.5f) * r * 0.7f;
        s_next_look = now + (home ? 1.0f + rnd() * 2.0f : 0.4f + rnd() * 0.8f);
    }
    t.lookx = s_tlookx;
    t.looky = s_tlooky;
    t.bob = 1.5f * sinf(now * 1.4f);   /* floats in its screen */
    if (m == MUSE_MODE_IDLE && !rc) {   /* idle habits: a wink, a curious brow, a happy moment */
        if (now >= s_next_wink) {
            s_wink_eye = rnd() < 0.5f;
            s_wink_until = now + 0.5f;
            s_next_wink = now + 14.0f + rnd() * 14.0f;
        }
        if (now >= s_next_curious) {
            s_curious_until = now + 1.8f;
            s_next_curious = now + 11.0f + rnd() * 12.0f;
        }
        if (now >= s_next_joy) {
            s_joy_until = now + 1.6f;
            s_next_joy = now + 18.0f + rnd() * 16.0f;
        }
        if (now < s_wink_until) {
            *(s_wink_eye ? &t.open_r : &t.open_l) = 0.06f;
            t.curve = 1.4f;
        } else if (now < s_curious_until) {
            t.brow_r = r * 0.55f;
            t.lookx = r * 0.45f;
            t.mouth = M_O;
        } else if (now < s_joy_until) {
            t.eyes = E_HAPPY;
            t.curve = 1.5f;
            t.blush = 0.7f;
            t.bob += 3.0f * fabsf(sinf((now - s_joy_until) * 9.0f));
        }
        if (p->mode_t > 45.0f) {
            t.eyes = E_SQUINT;   /* bored */
        }
    }
    switch (m) {
    case MUSE_MODE_LISTENING:
        t.lookx = t.looky = 0;
        t.eyes = E_WIDE;
        t.brow_l = t.brow_r = r * 0.3f;
        t.mouth = M_O;
        col = 0x3ee8ff;
        break;
    case MUSE_MODE_THINKING:
        t.lookx = -r * 0.45f + sinf(now * 1.7f) * r * 0.2f;
        t.looky = -r * 0.4f;
        t.sx = t.sy = 0.92f;
        t.brow_l = r * 0.25f;
        t.mouth = M_DOTS;
        t.dots = 1;
        col = 0xa064ff;
        break;
    case MUSE_MODE_SPEAKING:
        t.lookx *= 0.4f;
        t.looky *= 0.4f;
        t.talk = open;
        t.sy = 1.0f + 0.12f * open;            /* the eyes bounce with the syllables */
        t.brow_l = t.brow_r = r * 0.3f * open;
        t.bob += 3.0f * open;
        break;
    case MUSE_MODE_ERROR:
        t.eyes = E_CROSS;
        t.tilt = 1;
        t.mouth = M_FROWN;
        t.curve = -1;
        col = 0xff3a3a;
        break;
    case MUSE_MODE_BOOT:
        t.eyes = E_STAR;
        t.sparkle = 1;
        break;
    default:
        break;
    }
    if (p->happy > 0.3f) {   /* petted: heart eyes */
        t.eyes = E_HEART;
        t.mouth = M_BIG;
        t.curve = 1.5f;
        t.blush = 1;
        t.bob += 2.5f * fabsf(sinf(now * 9.0f));
    }
    if (rc) {   /* a reaction takes the face over while it plays */
        t.eyes = rc->eyes;
        if (m != MUSE_MODE_SPEAKING) {
            t.mouth = rc->mouth;
            t.talk = 0;
        }
        t.curve = rc->mouth == M_BIG || rc->mouth == M_GRIN ? 1.5f : rc->mouth == M_FROWN ? -1.0f :
                  rc->mouth == M_FLAT || rc->mouth == M_WAVY ? 0 : 1.0f;
        t.dots = rc->mouth == M_DOTS;
        t.tilt = rc->tilt / 10.0f;
        if (rc->extra & X_BROWS) {
            t.brow_l = t.brow_r = r * 0.4f;
        }
        if (rc->extra & X_BROW1) {
            t.brow_r = r * 0.6f;
        }
        t.blush = (rc->extra & X_BLUSH) ? 1 : 0;
        t.tear = (rc->extra & X_TEAR) ? 1 : 0;
        t.sweat = (rc->extra & X_SWEAT) ? 1 : 0;
        t.sparkle = (rc->extra & X_SPARKLE) ? 1 : 0;
        float lx, ly;
        react_look(rc->look, now, &lx, &ly);
        if (rc->look != L_NONE) {
            t.lookx = lx * r * 0.9f;
            t.looky = ly * r * 0.6f;
        }
        if (rc->fx == FX_BOUNCE) {
            t.bob += 4.0f * fabsf(sinf((now - s_react_t0) * 10.0f));
        }
        if (rc->col) {
            col = rc->col;
        }
        if (rc->fx == FX_HUE) {
            col = hue_rgb(now * 0.6f);
        }
        s_fx_glitch = rc->fx == FX_GLITCH && now - s_react_t0 < 0.7f ? 0.5f * env : 0;
    } else {
        s_fx_glitch = 0;
    }
    switch (t.eyes) {
    case E_WIDE: t.sx *= 1.22f; t.sy *= 1.22f; break;
    case E_SQUINT: t.open_l *= 0.45f; t.open_r *= 0.45f; break;
    case E_WINK: t.open_r = 0.06f; break;
    default: break;
    }
    float shut = 1.0f - 0.94f * b;
    t.open_l *= shut;
    t.open_r *= shut;
    t.sx *= 1.0f + 0.12f * b;   /* squash into the blink */
    if (m == MUSE_MODE_OFF) {
        t.open_l = t.open_r = 0.06f;
    }

    screen_t *s = &s_sf;
    if (t.eyes != s->eyes) {
        s_blink_t0 = now;       /* a new eye shape pops in behind a blink */
        s->eyes = t.eyes;
    }
    s->mouth = t.mouth;
    s->lookx = approach(s->lookx, t.lookx, 16, dt);
    s->looky = approach(s->looky, t.looky, 16, dt);
    s->open_l = b > 0.01f || t.open_l < 0.1f ? t.open_l : approach(s->open_l, t.open_l, 16, dt);
    s->open_r = b > 0.01f || t.open_r < 0.1f ? t.open_r : approach(s->open_r, t.open_r, 16, dt);
    s->sx = approach(s->sx, t.sx, 16, dt);
    s->sy = approach(s->sy, t.sy, 20, dt);
    s->brow_l = approach(s->brow_l, t.brow_l, 14, dt);
    s->brow_r = approach(s->brow_r, t.brow_r, 14, dt);
    s->tilt = approach(s->tilt, t.tilt, 12, dt);
    s->talk = m == MUSE_MODE_SPEAKING ? t.talk : approach(s->talk, t.talk, 14, dt);
    s->curve = approach(s->curve, t.curve, 12, dt);
    s->bob = approach(s->bob, t.bob, 20, dt);
    s->blush = approach(s->blush, t.blush, 6, dt);
    s->tear = approach(s->tear, t.tear, 6, dt);
    s->sweat = approach(s->sweat, t.sweat, 6, dt);
    s->sparkle = approach(s->sparkle, t.sparkle, 6, dt);
    s->dots = approach(s->dots, t.dots, 10, dt);
    s_gr = approach(s_gr, (float)((col >> 16) & 255), 10, dt);
    s_gg = approach(s_gg, (float)((col >> 8) & 255), 10, dt);
    s_gb = approach(s_gb, (float)(col & 255), 10, dt);
    float pulse = rc && rc->fx == FX_PULSE ? 0.82f + 0.18f * sinf(now * 9.0f) : 1.0f;
    s_cr = s_gr * pulse;
    s_cg = s_gg * pulse;
    s_cb = s_gb * pulse;

    /* redraw the light only when something visible moved */
    uint32_t key = 2166136261u;
    float q[] = { s->lookx, s->looky, s->open_l, s->open_r, s->sx * 8, s->sy * 8, s->brow_l, s->brow_r,
                  s->tilt * 8, s->talk * 16, s->curve * 8, s->bob, s->blush * 8, s->tear * 8, s->sweat * 8,
                  s->sparkle * 8, s->dots * 8, s_cr / 4, s_cg / 4, s_cb / 4,
                  (s->dots + s->tear + s->sweat + s->sparkle) > 0.02f ? now * 20 : 0 };
    for (size_t i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
        key = (key ^ (uint32_t)(int32_t)lroundf(q[i] * 16.0f)) * 16777619u;   /* 1/16 px: slow drifts move every frame */
    }
    key = (key ^ (uint32_t)(s->eyes * 16 + s->mouth)) * 16777619u;
    if (key != s_light_key) {
        s_light_key = key;
        build_light();
        s_light_dirty = true;
    }
}

bool muse_portrait_render(const muse_pose_t *p, int face, float glitch)
{
    if (!s_enabled || face < 0 || face > 2) {
        if (s_base && !s_enabled) {
            unload();   /* turned off: give the PSRAM back */
        }
        s_active = false;
        return false;
    }
    float now = p->t;
    float dt = now - s_last_t;
    dt = dt < 0 ? 0 : dt > 0.1f ? 0.1f : dt;
    if (face != s_face) {   /* a cut: a fade redraws the whole face every frame, which this panel can't do quickly */
        if (face == s_failed && now < s_retry_at) {
            s_active = false;
            return false;
        }
        int64_t t0 = esp_timer_get_time();
        if (!load(face)) {   /* on black: the decode's hitch doesn't show */
            ESP_LOGW(TAG, "face %d: no portrait, pixel face for now (retry in 2 s)", face);
            s_failed = face;
            s_retry_at = now + 2.0f;
            s_active = false;
            return false;
        }
        s_failed = -1;
        ESP_LOGI(TAG, "face %d: %d patches, loaded in %d ms", face, s_np, (int)((esp_timer_get_time() - t0) / 1000));
        memset(s_w, 0, sizeof(s_w));
        memset(s_wq, 0, sizeof(s_wq));
        s_react = -1;
        s_light_key = 0;
        s_full_dirty = true;
        s_gr = (float)((s_h.glow >> 16) & 255);
        s_gg = (float)((s_h.glow >> 8) & 255);
        s_gb = (float)(s_h.glow & 255);
    }
    s_last_t = s_now = now;

    int req = s_react_req;
    if (req >= 0) {
        s_react_req = -1;
        s_react = req;
        s_react_t0 = now;
    }
    /* the raw per-chunk level: the UI's smoothed one blurs the syllables away */
    float open = muse_lip_agc(&s_agc, p->mode == MUSE_MODE_SPEAKING ? muse_state_level() : 0, dt);
    float b = p->mode == MUSE_MODE_OFF ? 1.0f : blink(now);
    if (s_h.kind == 1) {
        screen_frame(p, now, dt, b, open);
    } else {
        photo_frame(p, now, dt, b, open);
        light_frame(p, now, dt, open, react_env(now));
    }
    s_prev_mode = p->mode;

    s_glitch = glitch > s_fx_glitch ? glitch : s_fx_glitch;
    if (s_glitch > 0.01f) {
        for (int i = 0; i < 64; i++) {
            s_shift[i] = rnd() < s_glitch * 0.6f ? (int8_t)((rnd() - 0.5f) * 2.0f * s_glitch * 40.0f) : 0;
        }
    }
    s_active = true;
    return true;
}

/* ---- What changed --------------------------------------------------------------------- */

static void row_direct(uint16_t *out, int sy, int x0, int x1);

static int add_rect(int (*r)[4], int n, int max, int x0, int y0, int x1, int y1)
{
    if (s_frame) {   /* bring the cached face up to date where it changed */
        int cx0 = x0 < 0 ? 0 : x0, cx1 = x1 >= s_size ? s_size - 1 : x1;
        for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < s_size; y++) {
            row_direct(s_frame + y * s_size + cx0, y, cx0, cx1);
        }
    }
    /* portrait px -> canvas px, rounded outwards */
    x0 = x0 * s_px / s_size;
    y0 = y0 * s_px / s_size;
    x1 = ((x1 + 1) * s_px + s_size - 1) / s_size - 1;
    y1 = ((y1 + 1) * s_px + s_size - 1) / s_size - 1;
    x1 = x1 >= s_px ? s_px - 1 : x1;
    y1 = y1 >= s_px ? s_px - 1 : y1;
    if (n == max) {   /* out of rects: grow the last one */
        int *l = r[n - 1];
        l[0] = x0 < l[0] ? x0 : l[0];
        l[1] = y0 < l[1] ? y0 : l[1];
        l[2] = x1 > l[2] ? x1 : l[2];
        l[3] = y1 > l[3] ? y1 : l[3];
        return n;
    }
    r[n][0] = x0;
    r[n][1] = y0;
    r[n][2] = x1;
    r[n][3] = y1;
    return n + 1;
}

int muse_portrait_dirty(int (*r)[4], int max)
{
    if (!s_base || max < 1) {
        return -1;
    }
    bool glitch = s_glitch > 0.01f;
    /* The canvas resizing (Muse making room for a reply) redraws it whole, but the cached face
     * is in portrait px and only its changed parts need composing, as on any frame. */
    bool resized = s_drawn_px != s_px;
    s_drawn_px = s_px;
    if (s_full_dirty || glitch || s_was_glitch) {
        s_full_dirty = false;
        s_was_glitch = glitch;
        s_light_dirty = false;
        for (int i = 0; i < s_np; i++) {
            s_drawn[i] = s_wq[s_p[i].tag];
        }
        return add_rect(r, 0, max, 0, 0, s_size - 1, s_size - 1);
    }
    int n = 0;
    if (s_h.kind == 0 && s_lmap) {
        if (s_ebox[2] >= 0) {
            n = add_rect(r, n, max, s_ebox[0], s_ebox[1], s_ebox[2], s_ebox[3]);
        }
        int b = s_lframe++ & 3;   /* the cables: one band a frame, in turn -- every frame moves, each stays small */
        if (s_lbands[b][2] >= 0) {
            n = add_rect(r, n, max, s_lbands[b][0], s_lbands[b][1], s_lbands[b][2], s_lbands[b][3]);
        }
    }
    if (s_h.kind == 1) {
        if (s_light_dirty) {
            s_light_dirty = false;
            n = add_rect(r, n, max, s_h.clip[0], s_h.clip[1], s_h.clip[2], s_h.clip[3]);
        }
    } else {
        for (int i = 0; i < s_np; i++) {
            int wq = s_wq[s_p[i].tag];
            if (wq != s_drawn[i]) {
                s_drawn[i] = wq;
                n = add_rect(r, n, max, s_p[i].x, s_p[i].y, s_p[i].x + s_p[i].w - 1, s_p[i].y + s_p[i].h - 1);
            }
        }
    }
    if (resized) {   /* composed above; the whole canvas shows it at its new size */
        r[0][0] = r[0][1] = 0;
        r[0][2] = r[0][3] = s_px - 1;
        return 1;
    }
    return n;
}

/* ---- The screen face's light ---------------------------------------------------------- */
/* This chip has no hardware float divide or square root, so the light avoids both per cell:
 * shapes are measured in eye radii (or mouth widths) with reciprocals taken once a frame, the
 * soft falloff comes from a table over the squared distance, and distances use a fast 1/sqrt. */

static void bell_init(void)
{
    for (int i = 0; i <= BELL_N + 1; i++) {
        float q2 = (float)i / 4.0f;
        s_bell[i] = 1.0f / ((1.0f + q2) * (1.0f + q2));
    }
}

static inline float bellq2(float q2)   /* 1 / (1 + q^2)^2 */
{
    if (q2 >= 32.0f) {
        return 0;
    }
    float f = q2 * 4.0f;
    int i = (int)f;
    return s_bell[i] + (s_bell[i + 1] - s_bell[i]) * (f - (float)i);
}

static inline float bl(float q)   /* 1 at q = 0, soft falloff over about q = 1 */
{
    return bellq2(q * q);
}

static inline float rsqrt(float x)   /* 1/sqrt(x), one Newton step (about 0.2% off) */
{
    union {
        float f;
        uint32_t i;
    } u = { x };
    u.i = 0x5f3759dfu - (u.i >> 1);
    return u.f * (1.5f - 0.5f * x * u.f * u.f);
}

static inline float fsqrt(float x)
{
    return x > 1e-6f ? x * rsqrt(x) : 0;
}

static inline float maxf(float a, float b)
{
    return a > b ? a : b;
}

/* Per-frame constants of the light (radius units unless noted). */
static struct {
    float isx, isy, io[2];        /* 1/scale, 1/openness per eye */
    float irx, iry, ry;           /* the talking oval, in mouth widths */
    float dot_ph[3];
} s_lk;

/* An eye's glow at (x, y) eye radii from its centre; *wh gets its shine. */
static float eye_light(const screen_t *s, int e, float x, float y, float *wh)
{
    float ex = x * s_lk.isx, ey = y * s_lk.isy;
    switch (s->eyes) {
    case E_HAPPY: {   /* ^ ^ : the top of a ring */
        float d = fsqrt(ex * ex + 1.7f * ey * ey);
        return ey < 0.1f ? bl((d - 0.7f) * 7.7f) : 0;
    }
    case E_HEART: {
        float u = ex * 1.176f, v = -ey * 1.176f + 0.2f, a = u * u + v * v - 1.0f;
        float f = a * a * a - u * u * v * v * v, hx = ex + 0.38f, hy = ey + 0.3f;
        *wh = maxf(*wh, bellq2((hx * hx + hy * hy) * 80.0f) * 0.8f);
        return f < 0 ? 0.9f : bl(f * 20.0f) * 0.9f;
    }
    case E_STAR: {   /* five points; cos(5a) from cos(a) (Chebyshev), a measured from straight up */
        float d2 = ex * ex + ey * ey + 1e-6f, inv = rsqrt(d2), d = d2 * inv, ca = -ey * inv, c2 = ca * ca;
        float c = 0.5f + 0.5f * ca * (16.0f * c2 * c2 - 20.0f * c2 + 5.0f);
        float rs = 0.38f + 0.5f * c * c * c, hx = ex + 0.18f, hy = ey + 0.22f;
        *wh = maxf(*wh, bellq2((hx * hx + hy * hy) * 160.0f) * 0.7f);
        return d < rs ? 0.9f : bl((d - rs) * 10.0f) * 0.9f;
    }
    case E_CROSS: {
        if (fabsf(ex) > 0.75f || fabsf(ey) > 0.75f) {
            return 0;
        }
        return bl(fminf(fabsf(ex - ey), fabsf(ex + ey)) * 0.7071f * 8.3f);
    }
    case E_LINE:
        return fabsf(ex) < 0.85f ? bl((ey - 0.1f) * 9.0f) : 0;
    default: {   /* a ring with a soft iris and a shine; open squashes it shut */
        float o = e ? s->open_r : s->open_l, yo = ey * s_lk.io[e];
        float d = fsqrt(ex * ex + yo * yo);
        float eye = bl((d - 0.78f) * (o < 0.3f ? 5.0f : 6.67f)) * (0.7f + 0.3f * o);   /* shut: a bright line */
        if (d < 0.6f) {
            eye += 0.22f * o;
        }
        float hx = ex + 0.32f, hy = yo + 0.34f;   /* the shine, top left */
        *wh = maxf(*wh, bellq2((hx * hx + hy * hy) * 69.4f) * o);
        return eye;
    }
    }
}

static inline float twinkle(float dx, float dy, float iw_thin, float iw_long)   /* a four-point sparkle */
{
    float ax = dx * dx, ay = dy * dy;
    return maxf(bellq2(ax * iw_thin) * bellq2(ay * iw_long), bellq2(ay * iw_thin) * bellq2(ax * iw_long));
}

/* The light is built feature by feature: each one fills only the half-size cells in its own box,
 * keeping the brightest value (most of the glass is dark). */
typedef struct {
    int i0, j0, i1, j1;
} box_t;

static box_t grid_box(float cx, float cy, float hw, float hh)
{
    box_t b = { (int)((cx - hw - s_h.clip[0]) / 2), (int)((cy - hh - s_h.clip[1]) / 2),
                (int)((cx + hw - s_h.clip[0]) / 2) + 1, (int)((cy + hh - s_h.clip[1]) / 2) + 1 };
    b.i0 = b.i0 < 0 ? 0 : b.i0;
    b.j0 = b.j0 < 0 ? 0 : b.j0;
    b.i1 = b.i1 >= s_lw_n ? s_lw_n - 1 : b.i1;
    b.j1 = b.j1 >= s_lh_n ? s_lh_n - 1 : b.j1;
    return b;
}

static inline void put(uint8_t *buf, int k, float v)
{
    if (v > 0.005f) {
        int q = v >= 1.27f ? 255 : (int)(v * 200.0f);
        buf[k] = q > buf[k] ? (uint8_t)q : buf[k];
    }
}

/* Each cell of a box, with its centre (x, y) in units of 1/unit from (cx, cy). */
#define EACH_CELL(box, cx, cy, unit, x, y, k)                                                       \
    for (int j_ = (box).j0; j_ <= (box).j1; j_++)                                                   \
        for (int i_ = (box).i0, k = j_ * s_lw_n + i_; i_ <= (box).i1; i_++, k++)                    \
            for (float x = ((float)(s_h.clip[0] + 2 * i_) - (cx)) * (unit),                         \
                       y = ((float)(s_h.clip[1] + 2 * j_) - (cy)) * (unit), o_ = 0; o_ < 1; o_ = 1)

static void build_light(void)
{
    if (!s_lg) {
        return;
    }
    const screen_t *s = &s_sf;
    memset(s_lg, 0, (size_t)s_lw_n * s_lh_n * 3);
    float r = s_h.eye_rad, ir = 1.0f / r;
    s_lk.isx = 1.0f / s->sx;
    s_lk.isy = 1.0f / s->sy;
    s_lk.io[0] = 1.0f / s->open_l;
    s_lk.io[1] = 1.0f / s->open_r;
    for (int e = 0; e < 2; e++) {
        float cx = (e ? s_h.eye_r[0] : s_h.eye_l[0]) + s->lookx;
        float cy = (e ? s_h.eye_r[1] : s_h.eye_l[1]) + s->looky + s->bob;
        float ext = 1.3f * r * (s->sx > s->sy ? s->sx : s->sy);   /* past this the glow is under 1% */
        box_t b = grid_box(cx, cy, ext, ext);
        EACH_CELL(b, cx, cy, ir, x, y, k) {
            float wh = 0;
            put(s_lg, k, eye_light(s, e, x, y, &wh));
            put(s_lw, k, wh);
        }
        /* the brow: a short arc, raised by the mood; tilt pulls the inner ends down */
        float bcy = cy - s_h.brow_dy - (e ? s->brow_r : s->brow_l);
        b = grid_box(cx, bcy, r, r * 0.9f);
        EACH_CELL(b, cx, bcy, ir, x, y, k) {
            float bx = x * 1.053f;
            if (bx > -1.0f && bx < 1.0f) {
                float inner = e ? -bx : bx, by = bx * bx * 0.22f + s->tilt * inner * 0.35f;
                put(s_lg, k, bl((y - by) * 10.0f) * (1.0f - bx * bx) * 0.9f);
            }
        }
        if (s->blush > 0.01f) {   /* cheeks, under and outside the eyes */
            float px = cx + (e ? r * 0.5f : -r * 0.5f), py = cy + r * 1.45f;
            b = grid_box(px, py, r * 1.4f, r * 0.9f);
            EACH_CELL(b, px, py, ir, x, y, k) {
                put(s_lp, k, bellq2((x * x + 2.2f * y * y) * 4.94f) * s->blush);
            }
        }
        if (s->tear > 0.01f && !e) {   /* a tear runs down from the left eye */
            float px = cx - r * 0.25f, py = cy + r * 1.0f + fmodf(s_now * 0.6f, 1.0f) * r * 1.4f;
            b = grid_box(px, py, r * 0.9f, r * 1.1f);
            EACH_CELL(b, px, py, ir, x, y, k) {
                put(s_lw, k, bellq2((x * x + (y < 0 ? 2.5f : 1.0f) * y * y) * 13.0f) * 0.95f * s->tear);
            }
        }
        if (s->sweat > 0.01f && e) {   /* a drop slides by the right brow */
            float px = cx + r * 1.55f, py = cy - r * 1.2f + fmodf(s_now * 0.45f, 1.0f) * r * 0.9f;
            b = grid_box(px, py, r * 0.9f, r * 1.1f);
            EACH_CELL(b, px, py, ir, x, y, k) {
                put(s_lw, k, bellq2((x * x + (y < 0 ? 2.5f : 1.0f) * y * y) * 15.0f) * 0.9f * s->sweat);
            }
        }
        if (s->sparkle > 0.01f) {   /* twinkles off the eyes' outer corners */
            float ph = 0.5f + 0.5f * sinf(s_now * 7.0f + (float)e * 2.1f), len = 0.35f * ph + 0.05f;
            float px = cx + (e ? r * 1.5f : -r * 1.5f), py = cy - r * 1.1f, thin = 1.4f * ir;
            float a = s->sparkle * ph, it = 1.0f / (thin * thin), il = 1.0f / (len * len);
            b = grid_box(px, py, r * 0.8f, r * 0.8f);
            EACH_CELL(b, px, py, ir, x, y, k) {
                put(s_lw, k, twinkle(x, y, it, il) * a);
            }
        }
    }
    /* the mouth: smile, frown, smirk, wavy, grin, O, dots; it opens into a glowing oval to talk */
    float mw = s_h.mouth_w, imw = 1.0f / mw;
    float mx = s_h.mouth[0] + s->lookx * 0.5f, my = s_h.mouth[1] + s->looky * 0.5f + s->bob;
    float tm = clamp01(s->talk * 4.0f), line_k = (1.0f - tm) * (1.0f - s->dots);
    float rx = 0.36f - 0.06f * s->talk, ry = 0.06f + 0.3f * s->talk, irx = 1.0f / rx, iry = 1.0f / ry;
    for (int i = 0; i < 3; i++) {
        s_lk.dot_ph[i] = 0.35f + 0.65f * maxf(sinf(s_now * 5.0f - (float)i * 0.9f), 0);
    }
    box_t b = grid_box(mx, my, mw * 0.8f, mw * 0.6f);
    EACH_CELL(b, mx, my, imw, x, y, k) {   /* x, y in mouth widths */
        float v = 0;
        float u = x * 1.818f;
        if (line_k > 0.01f && s->mouth != M_O && s->mouth != M_GRIN && s->mouth != M_DOTS && u > -1.0f && u < 1.0f) {
            float yc;
            if (s->mouth == M_WAVY) {
                yc = 0.07f * sinf(u * 6.2832f);
            } else if (s->mouth == M_SMIRK) {
                float up = clamp01(u);   /* flat on the left, pulled up on the right */
                yc = 0.04f * (1.0f - u * u) - 0.2f * up * up;
            } else {
                yc = s->curve * 0.12f * (1.0f - u * u);   /* lowest in the middle when smiling */
            }
            v = bl((y - yc + s->curve * 0.06f) * 14.3f) * (1.0f - u * u * u * u) * line_k;
        }
        if (tm > 0) {   /* the voice opens a glowing oval */
            float qy = (y - ry * 0.3f) * iry, qx = x * irx, q = fsqrt(qx * qx + qy * qy);
            v = maxf(v, (q < 1.0f ? 0.85f : bl((q - 1.0f) * 5.6f) * 0.85f) * tm);
        }
        if (s->mouth == M_O && tm < 0.5f) {   /* o: a hollow ring */
            float qx = x * 4.2f, qy = (y - 0.04f) * 3.8f, q = fsqrt(qx * qx + qy * qy);
            v = maxf(v, bl((q - 1.0f) * 3.2f) * 0.95f);
        }
        if (s->mouth == M_GRIN && tm < 0.5f) {   /* D: a big open grin, flat on top */
            float qx = x * 2.3f, qy = (y + 0.02f) * 3.2f, q = fsqrt(qx * qx + (qy < 0 ? qy * 6.0f : qy) * (qy < 0 ? qy * 6.0f : qy));
            v = maxf(v, q < 1.0f ? 0.8f + 0.2f * (1.0f - q) : bl((q - 1.0f) * 6.0f) * 0.9f);
        }
        if (s->dots > 0.02f) {
            for (int i = 0; i < 3; i++) {
                float ddx = x - (float)(i - 1) * 0.35f;
                v = maxf(v, bellq2((ddx * ddx + y * y) * 156.0f) * s_lk.dot_ph[i] * s->dots);
            }
        }
        put(s_lg, k, v);
    }
}

/* ---- Pixels --------------------------------------------------------------------------- */

static inline uint16_t mix565(uint16_t a, uint16_t b, int t)   /* t 0..256 */
{
    if (t <= 0) {
        return a;
    }
    if (t >= 256) {
        return b;
    }
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)(((ar + (((br - ar) * t) >> 8)) << 11) | ((ag + (((bg - ag) * t) >> 8)) << 5) |
                      (ab + (((bb - ab) * t) >> 8)));
}

static inline uint16_t patch_px(const patch_t *pt, int wq, int sx, int sy, uint16_t c)
{
    int i = (sy - pt->y) * pt->w + (sx - pt->x);
    int m = pt->mask[i];
    return m ? mix565(c, pt->var[i], (m * wq) >> 8) : c;
}

static inline int light_at(const uint8_t *buf, int fx, int fy)   /* bilinear from the half-size grid */
{
    int i = fx >> 1, j = fy >> 1, k = j * s_lw_n + i;
    int a = buf[k];
    if (fx & 1) {
        a = (a + buf[k + 1]) >> 1;
    }
    if (fy & 1) {
        int b = buf[k + s_lw_n];
        if (fx & 1) {
            b = (b + buf[k + s_lw_n + 1]) >> 1;
        }
        a = (a + b) >> 1;
    }
    return a;
}

static inline uint16_t glow_px(uint16_t c, int sx, int sy)
{
    int fx = sx - s_h.clip[0], fy = sy - s_h.clip[1];
    int g = light_at(s_lg, fx, fy), w = light_at(s_lw, fx, fy), pk = light_at(s_lp, fx, fy);
    if (!(g | w | pk)) {
        return c;
    }
    int r = ((c >> 11) & 31) << 3, gg = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
    r += ((int)s_cr * g + 255 * w + 150 * pk) / 200;
    gg += ((int)s_cg * g + 255 * w + 40 * pk) / 200;
    b += ((int)s_cb * g + 255 * w + 90 * pk) / 200;
    r = r > 255 ? 255 : r;
    gg = gg > 255 ? 255 : gg;
    b = b > 255 ? 255 : b;
    return (uint16_t)(((r >> 3) << 11) | ((gg >> 2) << 5) | (b >> 3));
}

/* A row at full size: the portrait, then only the patches (or the screen face) that cross it. */
static void row_direct(uint16_t *out, int sy, int x0, int x1)
{
    memcpy(out, &s_base[sy * s_size + x0], (size_t)(x1 - x0 + 1) * sizeof(uint16_t));
    if (s_h.kind == 1) {
        if (!s_lg || sy < s_h.clip[1] || sy > s_h.clip[3]) {
            return;
        }
        int a = x0 > s_h.clip[0] ? x0 : s_h.clip[0], b = x1 < s_h.clip[2] ? x1 : s_h.clip[2];
        for (int x = a; x <= b; x++) {
            out[x - x0] = glow_px(out[x - x0], x, sy);
        }
        return;
    }
    for (int i = 0; i < s_np; i++) {
        const patch_t *pt = &s_p[i];
        int wq = s_wq[pt->tag];
        if (!wq || sy < pt->y || sy >= pt->y + pt->h) {
            continue;
        }
        int a = x0 > pt->x ? x0 : pt->x, b = x1 < pt->x + pt->w - 1 ? x1 : pt->x + pt->w - 1;
        for (int x = a; x <= b; x++) {
            out[x - x0] = patch_px(pt, wq, x, sy, out[x - x0]);
        }
    }
    if (s_lmap) {
        const uint8_t *lr = s_lmap + sy * s_size, *pr = s_pmap + (sy >> 1) * (s_size >> 1);
        for (int x = x0; x <= x1; x++) {
            int l = lr[x];
            if (l) {   /* the cords rest a little darker, so a passing pulse reads as light moving */
                int p = pr[x >> 1];
                uint16_t c = p > 28 ? mix565(out[x - x0], 0, (l * 90) >> 8) : out[x - x0];   /* not the eyes */
                int a = (l * s_wave[p]) >> 8;
                out[x - x0] = a ? mix565(c, s_hot565, a) : c;
            }
        }
    }
}

void muse_portrait_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    if (!s_base) {
        for (int y = y0; y <= y1; y++, dst += stride_px) {
            memset(dst, 0, (size_t)n * sizeof(uint16_t));
        }
        return;
    }
    bool glitch = s_glitch > 0.01f;
    bool direct = s_px == s_size && !glitch && x1 < s_size && y1 < s_size;
    uint16_t row[512];   /* no cached frame: one portrait row, built the fast way, then sampled */
    int row_y = -1;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        int sy = y * s_size / s_px;
        sy = sy < s_size ? sy : s_size - 1;
        if (direct) {
            if (s_frame) {
                memcpy(dst, s_frame + sy * s_size + x0, (size_t)n * sizeof(uint16_t));
            } else {
                row_direct(dst, sy, x0, x1);
            }
            continue;
        }
        const uint16_t *src = s_frame ? s_frame + sy * s_size : row;
        if (!s_frame && sy != row_y) {
            row_direct(row, sy, 0, s_size - 1);
            row_y = sy;
        }
        int sh = glitch ? s_shift[(sy >> 3) & 63] : 0;
        for (int i = 0; i < n; i++) {
            int x = x0 + i, sx = (x < 512 ? s_xmap[x] : s_size) + sh;
            dst[i] = sx >= 0 && sx < s_size ? src[sx] : 0;
        }
    }
}

