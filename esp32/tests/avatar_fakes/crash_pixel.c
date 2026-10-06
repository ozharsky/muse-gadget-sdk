/* A fake avatar renderer that dies as soon as it is asked to draw, so the frame
 * dump produces nothing at all. Used by tests/test_muse_anim_gate.py to prove
 * anim.c reports a non-zero exit instead of pretending the render succeeded. */
#include <stdint.h>
#include <stdlib.h>

#include "muse_pixel.h"

uint32_t muse_pixel_accent(muse_mode_t mode) { (void)mode; return 0; }
void muse_pixel_set_size(int px) { (void)px; }
void muse_pixel_render(const muse_pose_t *p) { (void)p; abort(); }

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    (void)dst; (void)stride_px; (void)x0; (void)x1; (void)y0; (void)y1;
}
