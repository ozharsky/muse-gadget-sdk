/* A fake avatar renderer that draws the same frame every time: every frame is a
 * DEAD frame, so PIL merges the whole animation into a single GIF frame. Used by
 * tests/test_muse_anim_gate.py to prove make_gifs.py catches lost frames. */
#include <stdint.h>

#include "muse_pixel.h"

uint32_t muse_pixel_accent(muse_mode_t mode) { (void)mode; return 0; }
void muse_pixel_set_size(int px) { (void)px; }
void muse_pixel_render(const muse_pose_t *p) { (void)p; }

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        for (int x = x0; x <= x1; x++) {
            dst[x] = 0x1234;
        }
    }
}
