// Picks the avatar for the active agent: SHODAN for Muse, the stock one for Hermes.
// The two renderers are built with their muse_pixel_* names prefixed (see CMakeLists.txt).

#include "muse_pixel.h"

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
void shodan_pixel_glitch(float amp);
void stock_pixel_glitch(float amp);

static bool hermes(void)
{
    return CONFIG_MUSE_HERMES_URL[0] && muse_settings_agent_hermes();
}

/*
 * Switching agents plays a glitch: for 1.5 s the picture tears in sideways bands and fills with
 * noise, strongest in the middle, where the face changes from the old agent's to the new one's.
 */
#define TRANSITION_US 1500000
static int64_t s_tr_start;   /* 0: none */
static bool s_tr_from;       /* the agent shown until the middle */
static bool s_show_h;        /* the avatar the last render drew, for scale and accent */

void muse_pixel_transition(bool to_hermes)
{
    s_tr_from = !to_hermes;
    s_tr_start = esp_timer_get_time();
}

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    return s_show_h ? stock_pixel_accent(mode) : shodan_pixel_accent(mode);
}

void muse_pixel_set_size(int px)
{
    shodan_pixel_set_size(px);
    stock_pixel_set_size(px);
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    if (s_show_h) {
        stock_pixel_scale(dst, stride_px, x0, x1, y0, y1);
    } else {
        shodan_pixel_scale(dst, stride_px, x0, x1, y0, y1);
    }
}

void muse_pixel_render(const muse_pose_t *p)
{
    bool show = hermes();
    float amp = 0;
    if (s_tr_start) {
        float t = (float)(esp_timer_get_time() - s_tr_start) / TRANSITION_US;
        if (t >= 1.0f) {
            s_tr_start = 0;
        } else {
            show = t < 0.5f ? s_tr_from : show;
            amp = sinf(t * 3.14159265f);
        }
    }
    s_show_h = show;
    if (show) {
        stock_pixel_render(p);
        stock_pixel_glitch(amp);
    } else {
        shodan_pixel_render(p);
        shodan_pixel_glitch(amp);
    }
}

bool muse_pixel_react(const char *name)
{
    return hermes() ? stock_pixel_react(name) : shodan_pixel_react(name);
}
