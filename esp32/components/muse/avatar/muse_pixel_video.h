// Bench/relay video player (muse_pixel_video.c): plays compiled-in indexed 64x64 clips.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bench hook (>video=<name>): play a clip forever, or stop with "stop". false = unknown name.
bool muse_pixel_video(const char *name);

// Relay path: play a clip for `loops` full loops, then return to the face (used as the
// fallback for [react:<name>] names no face implements -- the themed reaction clips).
bool muse_pixel_video_play(const char *name, int loops);

// True while a clip owns the avatar canvas (LVGL task only).
bool muse_pixel_video_active(void);

// Advance + decode; call from muse_pixel_render when active (LVGL task).
void muse_pixel_video_render(void);

// The faces' scale-path contract (muse_pixel.h), over the clip's own palette.
void muse_pixel_video_set_size(int px_size);
void muse_pixel_video_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);

#ifdef __cplusplus
}
#endif
