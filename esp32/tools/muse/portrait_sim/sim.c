/* Host simulator for avatar/muse_portrait.c: the real renderer, frame by frame, on the Mac.
 *
 *   cc -O2 -I tools/muse/portrait_sim -I components/muse -I components/muse/avatar \
 *      tools/muse/portrait_sim/sim.c components/muse/avatar/muse_portrait.c -lm -o /tmp/psim
 *   /tmp/psim PACK.bin BASE.rgb565 FACE SCRIPT EVERY > frames.raw
 *
 * PACK.bin is a portrait pack (portraits/portrait_<name>.bin); BASE.rgb565 its portrait decoded
 * (portrait_review.py does that: the host has no esp_new_jpeg). FACE 0 Muse, 1 Hermes, 2 BEEPBOP.
 * SCRIPT: idle | modes | speak | pet | bored | react:<name>. Runs at 25 fps and writes every
 * EVERY-th frame as size x size RGB565 to stdout. Exits 1 if a reaction name is unknown.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_jpeg_dec.h"
#include "esp_partition.h"
#include "muse_assets.h"
#include "muse_portrait.h"

static uint8_t *g_pack;
static size_t g_pack_len;
static uint8_t *g_raw;
static size_t g_raw_len;
static int64_t g_us;
static float g_level;
static uint32_t g_rng = 12345;

uint32_t esp_random(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
int64_t esp_timer_get_time(void) { return g_us; }
float muse_state_level(void) { return g_level; }

static const esp_partition_t PART;
const esp_partition_t *esp_partition_find_first(int t, int s, const char *l) { (void)t; (void)s; (void)l; return &PART; }
esp_err_t esp_partition_mmap(const esp_partition_t *p, size_t off, size_t len, int m, const void **out,
                             esp_partition_mmap_handle_t *h)
{
    (void)p; (void)len; (void)m; *h = 1; *out = g_pack + off; return ESP_OK;
}
void esp_partition_munmap(esp_partition_mmap_handle_t h) { (void)h; }
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t len)
{
    (void)p; memcpy(dst, g_pack + off, len); return ESP_OK;
}
bool muse_assets_open(const char *name, muse_asset_t *out)
{
    (void)name;
    memset(out, 0, sizeof(*out));
    out->fmt = 3;
    out->frames = 1;
    out->offs[1] = (uint32_t)g_pack_len;
    return true;
}
jpeg_error_t jpeg_dec_open(jpeg_dec_config_t *c, jpeg_dec_handle_t *d) { (void)c; *d = (void *)1; return JPEG_ERR_OK; }
jpeg_error_t jpeg_dec_parse_header(jpeg_dec_handle_t d, jpeg_dec_io_t *io, jpeg_dec_header_info_t *info)
{
    (void)d; (void)io;
    info->width = info->height = (int)sqrt((double)(g_raw_len / 2));
    return JPEG_ERR_OK;
}
jpeg_error_t jpeg_dec_process(jpeg_dec_handle_t d, jpeg_dec_io_t *io) { (void)d; memcpy(io->outbuf, g_raw, g_raw_len); return JPEG_ERR_OK; }
void jpeg_dec_close(jpeg_dec_handle_t d) { (void)d; }

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len);
    if (fread(b, 1, *len, f) != *len) { exit(2); }
    fclose(f);
    return b;
}

/* Speech: syllables at about 4.5 a second, uneven, with a breath every couple of seconds. */
static float speech(float t)
{
    if (fmodf(t, 2.1f) > 1.8f) {
        return 0;
    }
    float syl = 0.5f + 0.5f * sinf(t * 6.2832f * 4.5f + sinf(t * 1.7f) * 2.0f);
    return 0.42f + 0.45f * syl * (0.8f + 0.2f * sinf(t * 2.3f));
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: psim PACK BASE FACE SCRIPT EVERY\n");
        return 2;
    }
    g_pack = slurp(argv[1], &g_pack_len);
    g_raw = slurp(argv[2], &g_raw_len);
    int face = atoi(argv[3]), every = atoi(argv[5]);
    const char *script = argv[4];
    int size = (int)sqrt((double)(g_raw_len / 2));
    muse_portrait_set_size(size);
    uint16_t *frame = malloc((size_t)size * size * 2);

    float len = 12.0f;
    if (!strcmp(script, "modes")) len = 15.0f;
    else if (!strcmp(script, "speak")) len = 6.0f;
    else if (!strcmp(script, "pet")) len = 4.5f;
    else if (!strcmp(script, "bored")) len = 50.0f;
    else if (!strncmp(script, "react:", 6)) len = 4.4f;

    muse_mode_t mode = MUSE_MODE_IDLE, last = MUSE_MODE_COUNT;
    float mode_t0 = 0, ui_level = 0;
    bool reacted = false;
    for (int f = 0; f * 0.04f < len; f++) {
        float t = (float)f * 0.04f;
        g_us = (int64_t)(t * 1e6f) + 1;
        float happy = 0;
        g_level = 0;
        if (!strcmp(script, "modes")) {
            mode = t < 1 ? MUSE_MODE_IDLE : t < 3 ? MUSE_MODE_LISTENING : t < 6 ? MUSE_MODE_THINKING :
                   t < 10 ? MUSE_MODE_SPEAKING : t < 11.5f ? MUSE_MODE_IDLE : t < 13.5f ? MUSE_MODE_ERROR : MUSE_MODE_OFF;
        } else if (!strcmp(script, "speak")) {
            mode = MUSE_MODE_SPEAKING;
        } else if (!strcmp(script, "pet")) {
            happy = t > 0.5f && t < 3.5f ? 1.0f : 0;
        } else if (!strncmp(script, "react:", 6) && t >= 0.4f && !reacted) {
            reacted = true;
            if (!muse_portrait_react(script + 6)) {
                fprintf(stderr, "unknown reaction %s\n", script + 6);
                return 1;
            }
        }
        if (mode == MUSE_MODE_SPEAKING) {
            g_level = speech(t);
        }
        if (mode != last) {
            last = mode;
            mode_t0 = t;
        }
        ui_level += (g_level - ui_level) * (g_level > ui_level ? 0.6f : 0.2f);
        muse_pose_t pose = { .mode = mode, .t = t, .mode_t = t - mode_t0 + (!strcmp(script, "bored") ? 40.0f : 0),
                             .level = ui_level, .happy = happy };
        if (!muse_portrait_render(&pose, face, 0)) {
            fprintf(stderr, "render failed at %.2f s\n", t);
            return 3;
        }
        int r[8][4];
        muse_portrait_dirty(r, 8);
        if (f % every == 0) {
            muse_portrait_scale(frame, size, 0, size - 1, 0, size - 1);
            fwrite(frame, 2, (size_t)size * size, stdout);
        }
    }
    return 0;
}
