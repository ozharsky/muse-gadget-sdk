/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * The gadget's black box (muse_diag.c). The screen freezes happen off-USB and a
 * reboot takes the USB serial link with it, so console logs from the moment
 * that matters are lost. These records persist in NVS instead:
 *
 *   muse_diag_note_fault_restart(...)  -- set by whoever is about to restart
 *   muse_diag_boot_report()            -- called at boot: logs it and keeps
 *                                         it readable as "restart_last" later
 *
 * so "why did it restart?" is answerable even when the cable was never attached
 * at the time it happened.
 */

#define DIAG_FAULT_MAGIC 0x4C434446u   /* "LCDF" */

/* What the display looked like at the moment a fault was recorded. */
typedef struct {
    int chunks_out;      /* pieces the send task still owed */
    int recover_count;   /* graceful recoveries this boot */
    int driver_ms;       /* age of the stuck driver call in ms, 0 = none */
} muse_diag_lcd_t;

/* Call once at startup (muse_app_run, after settings). */
esp_err_t muse_diag_init(void);

/* At boot: log the previous restart reason and the last display fault, and move
 * the reason where @status/>diag can always reach it. */
void muse_diag_boot_report(void);

/* Record WHY this boot is about to end. Survives the reboot; best effort. */
void muse_diag_note_restart(const char *why);

/* Record the display's dying state (call before rebooting from a guard trip). */
void muse_diag_note_lcd_fault(const char *detail, const muse_diag_lcd_t *lcd);

/* Both records in one NVS write: the about-to-restart path. */
void muse_diag_note_fault_restart(const char *detail, const muse_diag_lcd_t *lcd, const char *why);

/* The most recent reason (this boot's pending one, else the last reported). */
bool muse_diag_read_restart(char *out, size_t cap);

/* Read the last fault: detail_out may be NULL; counts[] = {chunks_out, recover_count,
 * driver_ms, free_kb, min_kb} (5 entries, may be NULL). True if a record exists. */
bool muse_diag_read_lcd_fault(char *detail_out, size_t cap, uint32_t *uptime_s, int *counts);
