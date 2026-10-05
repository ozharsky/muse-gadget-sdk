// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Rogue-AI voice, in place on 16 kHz mono PCM: stutter, bit crunch, metal buzz, echo, drive. */
void muse_voice_fx(int16_t *x, size_t n);
