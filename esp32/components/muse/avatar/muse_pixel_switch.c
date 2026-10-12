// Picks the avatar for the active agent: SHODAN for Muse, the stone for the default Hermes,
// the golem for the ALT Hermes slot (BEEPBOP). The three renderers are built with their
// muse_pixel_* names prefixed (shodan_/stock_/stock2_, see CMakeLists.txt).

#include "muse_pixel.h"
#include "muse_pixel_video.h"

#include <math.h>
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

void muse_pixel_render(const muse_pose_t *p)
{
    if (muse_pixel_video_active()) {
        muse_pixel_video_render();
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
