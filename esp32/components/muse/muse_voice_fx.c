// Copyright (c) Meta Platforms, Inc. and affiliates.
/*
 * A cold, damaged machine voice. Per sample, in order: sample-hold crunch,
 * ring modulation (the metal buzz), two echoes, soft-clip drive. On top, every
 * second or so the voice stutters: a ~24 ms slice loops for ~100 ms, replacing
 * the audio (so timing, and the captions timed to it, don't move).
 */
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include "esp_attr.h"

#include "muse_voice_fx.h"

#define RATE 16000
#define DL_N 4096                  /* power of two */
#define SLICE 384                  /* 24 ms */
#define RM_HZ 62.0f
#ifndef FX_CRUNCH
#define FX_CRUNCH 6                /* % of the sample-hold crunch mixed in */
#endif
#ifndef FX_BUZZ
#define FX_BUZZ 8                  /* % ring modulation */
#endif
#ifndef FX_GLITCH_MIN_S
#define FX_GLITCH_MIN_S 3          /* seconds between stutters: at least this... */
#endif
#ifndef FX_GLITCH_SPAN_S
#define FX_GLITCH_SPAN_S 5         /* ...plus up to this much more */
#endif
#ifndef FX_ECHO
#define FX_ECHO 50                 /* % of the full echo (1.0 = 38/26/20 % taps) */
#endif
#ifndef FX_DRIVE
#define FX_DRIVE 1.0f              /* soft-clip drive */
#endif
#define SIN_N 256

/* External RAM: internal RAM is scarce, and these only run at 16 kHz. */
static EXT_RAM_BSS_ATTR int16_t s_dl[DL_N];
static unsigned s_w;
static uint32_t s_ph;              /* ring-mod phase, Q32 */
static EXT_RAM_BSS_ATTR int16_t s_sin[SIN_N];
static int16_t s_hold;
static unsigned s_i;
static uint32_t s_rng = 0x2545f491u;
static EXT_RAM_BSS_ATTR int16_t s_cap[SLICE];
static int s_next = RATE * 4;      /* samples until the next stutter */
static int s_glitch;               /* samples left in the current one */
static int s_gpos;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static inline int16_t clip16(int v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void muse_voice_fx(int16_t *x, size_t n)
{
    static bool init;
    if (!init) {
        for (int i = 0; i < SIN_N; i++) {
            s_sin[i] = (int16_t)(sinf((float)i * 6.2831853f / SIN_N) * 32767.0f);
        }
        init = true;
    }
    const uint32_t step = (uint32_t)(RM_HZ / RATE * 4294967296.0f);
    for (size_t i = 0; i < n; i++) {
        int v = x[i];

        /* Stutter: capture a slice, then loop it. */
        if (s_glitch > 0) {
            v = s_cap[s_gpos++ % SLICE];
            s_glitch--;
        } else if (--s_next <= 0) {
            s_glitch = SLICE * 4 + (int)(rnd() % (SLICE * 3));
            s_next = FX_GLITCH_MIN_S * RATE + (int)(rnd() % (uint32_t)(FX_GLITCH_SPAN_S * RATE));
            s_gpos = 0;
            for (int k = 0; k < SLICE; k++) {
                s_cap[k] = s_dl[(s_w - SLICE + (unsigned)k) & (DL_N - 1)];   /* the audio just played */
            }
        }

        /* Crunch: hold every second sample and drop the low byte, mixed half and half. */
        if ((s_i++ & 1) == 0) {
            s_hold = (int16_t)(v & ~0x3ff);
        }
        v = (v * (100 - FX_CRUNCH) + s_hold * FX_CRUNCH) / 100;

        /* Metal buzz: ring modulation. */
        s_ph += step;
        int rm = (v * s_sin[s_ph >> 24]) >> 15;
        v = (v * (100 - FX_BUZZ) + rm * FX_BUZZ) / 100;

        /* Echoes at 23, 61 and 137 ms: a cold hall. */
        v += ((s_dl[(s_w - 368) & (DL_N - 1)] * 38 + s_dl[(s_w - 977) & (DL_N - 1)] * 26 +
               s_dl[(s_w - 2192) & (DL_N - 1)] * 20) / 100) * FX_ECHO / 100;
        s_dl[s_w & (DL_N - 1)] = clip16(v);
        s_w++;

        /* Drive: a cubic soft clip. */
        float f = (float)v / 32768.0f * FX_DRIVE;
        f = f > 1.0f ? 1.0f : f < -1.0f ? -1.0f : f;
        f = f - f * f * f / 3.0f;
        x[i] = clip16((int)(f * 32768.0f));
    }
}
