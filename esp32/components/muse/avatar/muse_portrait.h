// Portrait faces (muse_portrait.c): AI-rendered portraits that blink, talk and glance.
// Packs come from tools/muse/portrait_pack.py; muse_pixel_switch.c routes the face here.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "muse_pixel.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bench toggle (>portrait=0|1): off brings back the pixel faces. On by default.
void muse_portrait_enable(bool on);

// True when the last render drew a portrait (LVGL task).
bool muse_portrait_on(void);

// One frame for face 0 (Muse), 1 (Hermes) or 2 (BEEPBOP); glitch 0..1 tears it while the agent
// changes. false = no portrait (turned off, or its pack failed to load): draw the pixel face.
bool muse_portrait_render(const muse_pose_t *p, int face, float glitch);

uint32_t muse_portrait_accent(int face, muse_mode_t mode);

// The whole face redraws next frame (something else drew over the canvas: a clip).
void muse_portrait_redraw_all(void);

// A reaction by name (the relay's [react:...] moods): true if the portrait plays it.
bool muse_portrait_react(const char *name);

// What this frame changed (muse_pixel_dirty's contract).
int muse_portrait_dirty(int (*r)[4], int max);

// The faces' scale-path contract (muse_pixel.h).
void muse_portrait_set_size(int px);
void muse_portrait_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);

#ifdef __cplusplus
}
#endif
