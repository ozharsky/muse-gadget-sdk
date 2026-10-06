/* A fake avatar renderer whose every frame differs from the one before it: no
 * dead frames, so PIL keeps them all. The colour steps with each rendered frame
 * (times a large odd constant, so the shared palette never merges two of them).
 * Used by tests/test_muse_anim_gate.py as the good-renderer case. */
#include <stdint.h>

#include "muse_pixel.h"

static unsigned s_n;

uint32_t muse_pixel_accent(muse_mode_t mode) { (void)mode; return 0; }
void muse_pixel_set_size(int px) { (void)px; }
void muse_pixel_render(const muse_pose_t *p) { (void)p; s_n++; }

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    uint16_t c = (uint16_t)(s_n * 977u);
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        for (int x = x0; x <= x1; x++) {
            dst[x] = c;
        }
    }
}
