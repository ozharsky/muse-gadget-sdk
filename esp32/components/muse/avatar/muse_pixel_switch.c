// Picks the avatar for the active agent: SHODAN for Muse, the stone for the default Hermes,
// the golem for the ALT Hermes slot (BEEPBOP). The three renderers are built with their
// muse_pixel_* names prefixed (shodan_/stock_/stock2_, see CMakeLists.txt).

#include "muse_pixel.h"
#include "muse_pixel_video.h"
#include "bench_videos.h"   /* the compiled clip table, for the player bench */

#include <math.h>
#include <stdio.h>
#include "esp_timer.h"
#include "sdkconfig.h"
#include "muse_settings.h"

uint32_t shodan_pixel_accent(muse_mode_t mode);
void shodan_pixel_set_size(int px);
void shodan_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);
void shodan_pixel_render(const muse_pose_t *p);
bool shodan_pixel_react(const char *name);

uint32_t stock_pixel_accent(muse_mode_t mode);
void stock_pixel_set_size(int px);
void stock_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);
void stock_pixel_render(const muse_pose_t *p);
bool stock_pixel_react(const char *name);

uint32_t stock2_pixel_accent(muse_mode_t mode);
void stock2_pixel_set_size(int px);
void stock2_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);
void stock2_pixel_render(const muse_pose_t *p);
bool stock2_pixel_react(const char *name);

void shodan_pixel_glitch(float amp);
void stock_pixel_glitch(float amp);
void stock2_pixel_glitch(float amp);

/* The face each agent slot draws. Muse -> SHODAN; Hermes default -> stone; Hermes ALT -> golem.
 * Falls back to SHODAN if the relay is unconfigured, exactly as before. */
static int face_for_agent(void)
{
    if (!CONFIG_MUSE_HERMES_URL[0]) {
        return 0;
    }
    switch (muse_settings_agent()) {
    case MUSE_AGENT_HERMES_ALT:
        return 2;
    case MUSE_AGENT_HERMES:
        return 1;
    default:
        return 0;
    }
}

/*
 * Switching agents plays a glitch: for 1.5 s the picture tears in sideways bands and fills with
 * noise, strongest in the middle, where the face changes from the old agent's to the new one's.
 * Generalized over the face index so three agents work; the transition still uses a bool on the
 * agent change (muse_ui_agent_switch), and this file re-reads which face is current each frame.
 */
#define TRANSITION_US 1500000
static int64_t s_tr_start;   /* 0: none */
static int s_tr_from;        /* the face shown until the middle (0..2) */
static int s_show;           /* the face the last render drew, for scale and accent */
static bool s_show_init;

static int64_t last_agent_seen;   /* to notice the agent changing without a call */

static void set_size_all(int px)
{
    shodan_pixel_set_size(px);
    stock_pixel_set_size(px);
    stock2_pixel_set_size(px);
}

void muse_pixel_transition(bool to_hermes)
{
    /* Kept for the existing caller: the direction hint no longer matters (three faces), so
     * this just starts a transition from whatever face is on screen. */
    (void)to_hermes;
    s_tr_from = s_show;
    s_tr_start = esp_timer_get_time();
    last_agent_seen = -1;   /* force the render path to re-read the agent */
}

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    switch (s_show) {
    case 2: return stock2_pixel_accent(mode);
    case 1: return stock_pixel_accent(mode);
    default: return shodan_pixel_accent(mode);
    }
}

void muse_pixel_set_size(int px)
{
    set_size_all(px);
    muse_pixel_video_set_size(px);
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    if (muse_pixel_video_active()) {
        muse_pixel_video_scale(dst, stride_px, x0, x1, y0, y1);
        return;
    }
    switch (s_show) {
    case 2: stock2_pixel_scale(dst, stride_px, x0, x1, y0, y1); break;
    case 1: stock_pixel_scale(dst, stride_px, x0, x1, y0, y1); break;
    default: shodan_pixel_scale(dst, stride_px, x0, x1, y0, y1); break;
    }
}

void muse_pixel_render(const muse_pose_t *p);

static void render_frame(const muse_pose_t *p)
{
    muse_pixel_video_render();   /* applies a pending start/stop; no-op when idle */
    if (muse_pixel_video_active()) {
        return;
    }
    int target = face_for_agent();
    if (!s_show_init) {
        s_show = target;
        s_show_init = true;
    }
    float amp = 0;
    if (s_tr_start) {
        float t = (float)(esp_timer_get_time() - s_tr_start) / TRANSITION_US;
        if (t >= 1.0f) {
            s_tr_start = 0;
        } else {
            /* first half shows the OLD face, second half the new one (the tear hides the swap) */
            s_show = t < 0.5f ? s_tr_from : target;
            amp = sinf(t * 3.14159265f);
        }
        last_agent_seen = target;
    } else if (target != last_agent_seen) {
        /* The agent changed without a transition call (boot, direct settings write): snap. */
        last_agent_seen = target;
        s_show = target;
    }
    switch (s_show) {
    case 2:
        stock2_pixel_render(p);
        stock2_pixel_glitch(amp);
        break;
    case 1:
        stock_pixel_render(p);
        stock_pixel_glitch(amp);
        break;
    default:
        shodan_pixel_render(p);
        shodan_pixel_glitch(amp);
        break;
    }
}

/* ---- Device-side frame-cost bench (>render.bench[=N]) ------------------------------
 * Measures the real per-frame cost of the two canvas paths on this chip in this build:
 * phase 1 renders N plain face frames, phase 2 plays the first compiled clip for N
 * frames (the player path). Both phases time the whole render call; the scale/blit to
 * the LCD happens later, per strip, and is not counted -- the same boundary as the host
 * harnesses (dump_anim.c for the face, video_harness.c for the player). All state moves
 * on the LVGL task; the console only sets the request. */
static volatile int s_bench_req;         /* frames per phase; 0 = none pending */
static int s_bench_phase;               /* 0 idle, 1 face, 2 player */
static int s_bench_n, s_bench_count;
static int64_t s_bench_sum, s_bench_min, s_bench_max;        /* face path, us */
static int64_t s_bench_vsum, s_bench_vmin, s_bench_vmax;     /* player path, us */
static int64_t s_bench_dsum;           /* player path, only the frames that decoded */
static int s_bench_dn;
static int64_t s_bench_prev, s_bench_psum;
static int s_bench_pn;
static const char *s_bench_clip = "";

void muse_pixel_bench(int frames)
{
    if (frames < 20) {
        frames = 20;
    }
    if (frames > 2000) {
        frames = 2000;
    }
    s_bench_req = frames;
}

static void bench_report(void)
{
    long fa = s_bench_n ? (long)(s_bench_sum / s_bench_n) : 0;
    long va = s_bench_n ? (long)(s_bench_vsum / s_bench_n) : 0;
    long da = s_bench_dn ? (long)(s_bench_dsum / s_bench_dn) : 0;
    long fp = s_bench_pn ? (long)(s_bench_psum / s_bench_pn) : 0;
    printf("@render.bench face n=%d avg=%ldus min=%ld max=%ld | player(%s) n=%d avg=%ldus min=%ld max=%ld decode avg=%ldus x%d | frame avg=%ldus %ld.%ldfps\n",
           s_bench_n, fa, (long)(s_bench_min == INT64_MAX ? 0 : s_bench_min), (long)s_bench_max,
           s_bench_clip, s_bench_n, va, (long)(s_bench_vmin == INT64_MAX ? 0 : s_bench_vmin), (long)s_bench_vmax,
           da, s_bench_dn,
           fp, fp ? 10000000 / fp / 10 : 0, fp ? (10000000 / fp) % 10 : 0);
    fflush(stdout);
    s_bench_phase = 0;
}

void muse_pixel_render(const muse_pose_t *p)
{
    if (s_bench_req && !s_bench_phase) {
        s_bench_n = s_bench_req;
        s_bench_req = 0;
        s_bench_phase = 1;
        s_bench_count = s_bench_dn = s_bench_pn = 0;
        s_bench_sum = s_bench_vsum = s_bench_dsum = s_bench_psum = 0;
        s_bench_min = s_bench_vmin = INT64_MAX;
        s_bench_max = s_bench_vmax = 0;
        s_bench_prev = 0;
        muse_pixel_video("stop");   /* phase 1 measures the face path */
        printf("@render.bench start face n=%d, then the player (stay on the face, keep it awake)\n", s_bench_n);
        fflush(stdout);
    }
    if (!s_bench_phase) {
        render_frame(p);
        return;
    }
    int64_t t0 = esp_timer_get_time();
    int d0 = s_bench_phase == 2 ? muse_pixel_video_decodes(false) : 0;
    render_frame(p);
    int64_t t1 = esp_timer_get_time();
    int d1 = s_bench_phase == 2 ? muse_pixel_video_decodes(false) : 0;
    int64_t us = t1 - t0;
    if (s_bench_phase == 1) {
        if (muse_pixel_video_active()) {
            s_bench_prev = 0;        /* a clip is in the way: wait, don't count */
            return;
        }
        if (s_bench_prev) {
            s_bench_psum += t0 - s_bench_prev;
            s_bench_pn++;
        }
        s_bench_prev = t0;
        s_bench_sum += us;
        if (us < s_bench_min) {
            s_bench_min = us;
        }
        if (us > s_bench_max) {
            s_bench_max = us;
        }
        s_bench_count++;
        if (s_bench_count >= s_bench_n) {
            s_bench_phase = 2;       /* face done: play a clip and measure the player */
            s_bench_count = 0;
            s_bench_prev = 0;
            (void)muse_pixel_video_decodes(true);   /* zero the decode counter */
            if (BENCH_VIDEO_COUNT > 0) {
                s_bench_clip = BENCH_VIDEOS[0].name;
                muse_pixel_video(s_bench_clip);
            } else {
                s_bench_clip = "(none compiled)";
                bench_report();
            }
        }
    } else if (s_bench_phase == 2) {
        if (!muse_pixel_video_active()) {
            return;                  /* nothing playing: wait for the pending start */
        }
        s_bench_vsum += us;
        if (us < s_bench_vmin) {
            s_bench_vmin = us;
        }
        if (us > s_bench_vmax) {
            s_bench_vmax = us;
        }
        if (d1 > d0) {
            s_bench_dsum += us;      /* this call decoded a clip frame */
            s_bench_dn++;
        }
        s_bench_count++;
        if (s_bench_count >= s_bench_n) {
            muse_pixel_video("stop");
            bench_report();
        }
    }
}

bool muse_pixel_react(const char *name)
{
    switch (face_for_agent()) {
    case 2:
        if (stock2_pixel_react(name)) {
            return true;
        }
        break;
    case 1:
        if (stock_pixel_react(name)) {
            return true;
        }
        break;
    default:
        if (shodan_pixel_react(name)) {
            return true;
        }
        break;
    }
    /* No face draws this mood: the clip table gets the last word (themed reaction clips,
     * e.g. the Halloween set, play full-face for one loop). */
    return muse_pixel_video_play(name, 1);
}
