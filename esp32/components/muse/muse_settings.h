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
 * User settings, persisted in NVS. Setters save immediately and then notify the
 * change listener (from the caller's task), which applies the setting to the
 * hardware. Brightness, auto-sleep and the speaker are polled where they're used.
 */

#define MUSE_SSID_MAX 32
#define MUSE_PASS_MAX 64
#define MUSE_HOST_MAX 63
#define MUSE_VM_MAX 63
#define MUSE_TOKEN_MAX 1023

#define MUSE_MIC_GAIN_MAX 36      /* dB; ES7210 PGA, applied in 3 dB steps */

typedef enum {
    MUSE_SETTING_VOLUME,
    MUSE_SETTING_SPEAKER,
    MUSE_SETTING_MIC_GAIN,
    MUSE_SETTING_BRIGHTNESS,
    MUSE_SETTING_SLEEP,
    MUSE_SETTING_WIFI,          /* on/off or credentials */
    MUSE_SETTING_BLE,
    MUSE_SETTING_HATCH,
    MUSE_SETTING_AGENT,         /* Muse or Hermes */
} muse_setting_t;

typedef void (*muse_setting_cb_t)(muse_setting_t what);

esp_err_t muse_settings_init(void);
void muse_settings_set_listener(muse_setting_cb_t cb);

int muse_settings_volume(void);         /* 0..100 */
bool muse_settings_speaker_on(void);    /* off: replies are shown, not played */
int muse_settings_mic_gain(void);       /* dB, 0..MUSE_MIC_GAIN_MAX */
int muse_settings_brightness(void);     /* 10..100 */
int muse_settings_sleep_s(void);        /* 0 = never */
bool muse_settings_wifi_on(void);
uint32_t muse_settings_tts_chars(void);          /* characters sent to text-to-speech since the last reset */
void muse_settings_tts_add(uint32_t chars);
void muse_settings_tts_reset(void);
bool muse_settings_ble_on(void);

void muse_settings_wifi(char ssid[MUSE_SSID_MAX + 1], char pass[MUSE_PASS_MAX + 1]);
void muse_settings_hatch_host(char out[MUSE_HOST_MAX + 1]);
void muse_settings_hatch_vm(char out[MUSE_VM_MAX + 1]);
void muse_settings_hatch_token(char out[MUSE_TOKEN_MAX + 1]);
size_t muse_settings_hatch_token_len(void);

#define MUSE_AGENT_PROFILE_MAX 32   /* a Hermes profile name (agents.json key) */

void muse_settings_set_volume(int pct);
void muse_settings_set_speaker_on(bool on);
void muse_settings_set_mic_gain(int db);
void muse_settings_set_brightness(int pct);
void muse_settings_set_sleep_s(int secs);
void muse_settings_set_wifi_on(bool on);
void muse_settings_set_ble_on(bool on);
/* Which agent answers. Muse is the Meta cloud; the two Hermes slots are the Mac relay
 * against different Hermes PROFILES (agents.json on the relay routes by name). The ALT
 * slot stays dormant until a profile name is set, so a device that never sets one cycles
 * exactly as it did with the old boolean. */
enum {
    MUSE_AGENT_MUSE = 0,
    MUSE_AGENT_HERMES = 1,        /* the relay's default profile */
    MUSE_AGENT_HERMES_ALT = 2,    /* the relay profile named by muse_settings_agent_profile() */
    MUSE_AGENT_COUNT,
};

int muse_settings_agent(void);
/* True for either Hermes slot: the 20+ call sites that only ask "Hermes or Muse?" use this. */
bool muse_settings_agent_hermes(void);
/* The Hermes profile name ("" = the relay's default). Only meaningful off the MUSE slot. */
void muse_settings_agent_profile(char out[MUSE_AGENT_PROFILE_MAX + 1]);

void muse_settings_set_agent(int idx);
/* Kept for the boolean toggle: true = HERMES, false = MUSE (never jumps to ALT). */
void muse_settings_set_agent_hermes(bool hermes);
/* The profile the ALT slot points at; empty disables ALT (the cycle skips it). */
esp_err_t muse_settings_set_agent_profile(const char *name);
/* A network name is remembered first among the saved ones and joined now;
 * an empty ssid forgets every saved network. */
void muse_settings_set_wifi(const char *ssid, const char *pass);
void muse_settings_set_hatch_host(const char *host);
void muse_settings_set_hatch_vm(const char *vm);
/* append=true adds to the stored token (for chunked BLE writes). */
esp_err_t muse_settings_set_hatch_token(const char *token, bool append);
