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

#include "muse_settings.h"

#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "muse_link.h"

static const char *TAG = "muse_settings";

#define NS "muse"
#define DEFAULT_HOST "hatch.metaaivm.com"

static struct {
    uint8_t volume;
    bool speaker_on;
    uint8_t mic_gain;
    uint8_t brightness;
    uint16_t sleep_s;
    bool wifi_on;
    bool ble_on;
    bool iphone_on;
    bool iphone_alerts;
    uint8_t agent;                       /* 0 Muse, 1 Hermes, 2 Hermes ALT */
    char agent_alt_profile[MUSE_AGENT_PROFILE_MAX + 1];   /* the ALT slot's relay profile name */
    char relay_lan[64];                        /* the relay's last announced LAN address */
    char relay_remote[MUSE_RELAY_URL_MAX + 1];   /* relay base URL from the setup page */
    char relay_token[MUSE_RELAY_TOKEN_MAX + 1];  /* relay X-Gadget-Token from the setup page */
    char ssid[MUSE_SSID_MAX + 1];
    char pass[MUSE_PASS_MAX + 1];
    char host[MUSE_HOST_MAX + 1];
    char vm[MUSE_VM_MAX + 1];
    char token[MUSE_TOKEN_MAX + 1];
} s = {
    .volume = CONFIG_MUSE_DEFAULT_VOLUME,
    .speaker_on = true,
    .mic_gain = 36,   /* the bench sweep: +6 dB speech; floor -60 dB stays far below it */
    .brightness = 100,
    .sleep_s = 120,
    .wifi_on = true,
    .host = DEFAULT_HOST,
};

static SemaphoreHandle_t s_lock;
static nvs_handle_t s_nvs;
static muse_setting_cb_t s_listener;

#define LOCKED(body) do { xSemaphoreTake(s_lock, portMAX_DELAY); body; xSemaphoreGive(s_lock); } while (0)

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Missing keys leave the default in place. */
static void load_str(const char *key, char *out, size_t len)
{
    size_t n = len;
    nvs_get_str(s_nvs, key, out, &n);
}

static void load_u8(const char *key, uint8_t *out)
{
    nvs_get_u8(s_nvs, key, out);
}

static esp_err_t save_u8(const char *key, uint8_t v)
{
    esp_err_t err = nvs_set_u8(s_nvs, key, v);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs save '%s' failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t save_str(const char *key, const char *v)
{
    esp_err_t err = nvs_set_str(s_nvs, key, v);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs save '%s' failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

static void notify(muse_setting_t what)
{
    if (s_listener) {
        s_listener(what);
    }
}

static uint32_t s_tts_chars;

uint32_t muse_settings_tts_chars(void) { return s_tts_chars; }

static void save_tts_chars(void)
{
    nvs_set_u32(s_nvs, "tts_chars", s_tts_chars);
    nvs_commit(s_nvs);
}

void muse_settings_tts_add(uint32_t chars)
{
    s_tts_chars += chars;
    save_tts_chars();
}

void muse_settings_tts_reset(void)
{
    s_tts_chars = 0;
    save_tts_chars();
}

esp_err_t muse_settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    s_lock = xSemaphoreCreateMutex();
    err = nvs_open(NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t b;
    nvs_get_u32(s_nvs, "tts_chars", &s_tts_chars);
    load_u8("volume", &s.volume);
    if (nvs_get_u8(s_nvs, "speaker", &b) == ESP_OK) {
        s.speaker_on = b;
    }
    load_u8("mic_gain", &s.mic_gain);
    load_u8("bright", &s.brightness);
    nvs_get_u16(s_nvs, "sleep_s", &s.sleep_s);
    if (nvs_get_u8(s_nvs, "wifi_on", &b) == ESP_OK) {
        s.wifi_on = b;
    }
    if (nvs_get_u8(s_nvs, "ble_on", &b) == ESP_OK) {
        s.ble_on = b;
    }
    if (nvs_get_u8(s_nvs, "iphone_on", &b) == ESP_OK) {
        s.iphone_on = b;
    }
    if (nvs_get_u8(s_nvs, "iphone_alert", &b) == ESP_OK) {
        s.iphone_alerts = b;
    }
    if (nvs_get_u8(s_nvs, "agent", &b) == ESP_OK) {
        /* Old value is the same 0/1 (Muse/Hermes) the new enum keeps for those slots. */
        s.agent = b < MUSE_AGENT_COUNT ? b : MUSE_AGENT_HERMES;
    }
    load_str("agent_alt", s.agent_alt_profile, sizeof(s.agent_alt_profile));
    load_str("relay_lan", s.relay_lan, sizeof(s.relay_lan));
    load_str("relay_remote", s.relay_remote, sizeof(s.relay_remote));
    load_str("relay_token", s.relay_token, sizeof(s.relay_token));
    load_str("ssid", s.ssid, sizeof(s.ssid));
    load_str("pass", s.pass, sizeof(s.pass));
    load_str("host", s.host, sizeof(s.host));
    load_str("vm", s.vm, sizeof(s.vm));
    load_str("token", s.token, sizeof(s.token));

    s.volume = clampi(s.volume, 0, 100);
    s.mic_gain = clampi(s.mic_gain, 0, MUSE_MIC_GAIN_MAX);
    s.brightness = clampi(s.brightness, 10, 100);
    ESP_LOGI(TAG, "vol %d%s, mic %d dB, bright %d, sleep %ds, wifi %s (%s), ble %s, muse %s",
             s.volume, s.speaker_on ? "" : " (speaker off)", s.mic_gain, s.brightness, s.sleep_s, s.wifi_on ? "on" : "off",
             "network saved by Link", s.ble_on ? "on" : "off", s.token[0] ? "token set" : "no token");
    return ESP_OK;
}

void muse_settings_set_listener(muse_setting_cb_t cb)
{
    s_listener = cb;
}

int muse_settings_volume(void) { return s.volume; }
bool muse_settings_speaker_on(void) { return s.speaker_on; }
int muse_settings_mic_gain(void) { return s.mic_gain; }
int muse_settings_brightness(void) { return s.brightness; }
int muse_settings_sleep_s(void) { return s.sleep_s; }
bool muse_settings_wifi_on(void) { return s.wifi_on; }
bool muse_settings_ble_on(void) { return s.ble_on; }
bool muse_settings_iphone_on(void) { return s.iphone_on; }
bool muse_settings_iphone_alerts(void) { return s.iphone_alerts; }
int muse_settings_agent(void) { return s.agent; }
bool muse_settings_agent_hermes(void) { return s.agent != MUSE_AGENT_MUSE; }
void muse_settings_agent_profile(char out[MUSE_AGENT_PROFILE_MAX + 1]) {
    strlcpy(out, s.agent == MUSE_AGENT_HERMES_ALT ? s.agent_alt_profile : "", MUSE_AGENT_PROFILE_MAX + 1);
}

/* The relay's announced LAN address, persisted (an NVS string; empty = unknown). */
void muse_settings_relay_lan(char out[64])
{
    LOCKED(strlcpy(out, s.relay_lan, 64));
}

void muse_settings_set_relay_lan(const char *lan)
{
    /* A bare host only: the URL is composed as http://<lan>:8788/turn. A pasted
     * URL ("http://10.0.0.69:8788") used to be stored verbatim and produced
     * http://http://10.0.0.69:8788:8788 -- every turn failed with a misleading
     * OOM. Normalise, and reject anything that still is not a host. */
    char norm[64] = "";
    if (lan && *lan) {
        const char *p = lan;
        if (!strncmp(p, "http://", 7)) {
            p += 7;
        } else if (!strncmp(p, "https://", 8)) {
            p += 8;
        }
        strlcpy(norm, p, sizeof(norm));
        size_t n = strlen(norm);
        while (n && (norm[n - 1] == '/' || norm[n - 1] == ' ')) {
            norm[--n] = '\0';
        }
        if (strchr(norm, '/') || strchr(norm, ':') || strchr(norm, ' ')) {
            ESP_LOGW(TAG, "relay lan '%s' is not a bare host; rejected", lan);
            norm[0] = '\0';
        }
    }
    LOCKED(strlcpy(s.relay_lan, norm, sizeof(s.relay_lan)));
    nvs_set_str(s_nvs, "relay_lan", s.relay_lan);
    nvs_commit(s_nvs);
    ESP_LOGI(TAG, "relay lan: '%s'", s.relay_lan);
}

/* The relay's remote base URL, saved from the setup page (empty = none saved). */
void muse_settings_relay_remote(char out[MUSE_RELAY_URL_MAX + 1])
{
    LOCKED(strlcpy(out, s.relay_remote, MUSE_RELAY_URL_MAX + 1));
}

void muse_settings_set_relay_remote(const char *url)
{
    LOCKED({
        strlcpy(s.relay_remote, url ? url : "", sizeof(s.relay_remote));
        save_str("relay_remote", s.relay_remote);
    });
    ESP_LOGI(TAG, "relay remote: '%s'", s.relay_remote);
}

/* The relay token, saved from the setup page; append=true adds a chunk (BLE writes are
 * sized under the attribute limit). Never logged. */
void muse_settings_relay_token(char out[MUSE_RELAY_TOKEN_MAX + 1])
{
    LOCKED(strlcpy(out, s.relay_token, MUSE_RELAY_TOKEN_MAX + 1));
}

size_t muse_settings_relay_token_len(void)
{
    size_t n;
    LOCKED(n = strlen(s.relay_token));
    return n;
}

esp_err_t muse_settings_set_relay_token(const char *token, bool append)
{
    esp_err_t err = ESP_OK;
    LOCKED({
        size_t have = append ? strlen(s.relay_token) : 0;
        size_t add = strlen(token ? token : "");
        if (have + add > MUSE_RELAY_TOKEN_MAX) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(s.relay_token + have, token, add);
            s.relay_token[have + add] = '\0';
            save_str("relay_token", s.relay_token);
        }
    });
    return err;
}

bool muse_settings_relay_configured(void)
{
    bool nvs = false;
    LOCKED(nvs = s.relay_lan[0] != 0 || s.relay_remote[0] != 0);
    if (nvs) {
        return true;
    }
#ifdef CONFIG_MUSE_HERMES_URL
    return CONFIG_MUSE_HERMES_URL[0] != 0;
#else
    return false;
#endif
}

void muse_settings_agent_alt(char out[MUSE_AGENT_PROFILE_MAX + 1]) {
    strlcpy(out, s.agent_alt_profile, MUSE_AGENT_PROFILE_MAX + 1);
}

void muse_settings_agent_name(int idx, char out[MUSE_AGENT_PROFILE_MAX + 1])
{
    if (idx == MUSE_AGENT_MUSE) {
        strlcpy(out, "Muse", MUSE_AGENT_PROFILE_MAX + 1);
    } else if (idx == MUSE_AGENT_HERMES_ALT) {
        strlcpy(out, s.agent_alt_profile[0] ? s.agent_alt_profile : "Alt", MUSE_AGENT_PROFILE_MAX + 1);
    } else {
        strlcpy(out, "Hermes", MUSE_AGENT_PROFILE_MAX + 1);
    }
}

/* Home Link owns the saved networks (this is the first); the local copy is only a fallback. */
void muse_settings_wifi(char ssid[MUSE_SSID_MAX + 1], char pass[MUSE_PASS_MAX + 1])
{
    if (muse_link_wifi_get(ssid, pass)) {
        return;
    }
    LOCKED({
        strlcpy(ssid, s.ssid, MUSE_SSID_MAX + 1);
        if (pass) {
            strlcpy(pass, s.pass, MUSE_PASS_MAX + 1);
        }
    });
}

void muse_settings_hatch_host(char out[MUSE_HOST_MAX + 1])
{
    LOCKED(strlcpy(out, s.host, MUSE_HOST_MAX + 1));
}

void muse_settings_hatch_vm(char out[MUSE_VM_MAX + 1])
{
    LOCKED(strlcpy(out, s.vm, MUSE_VM_MAX + 1));
}

void muse_settings_hatch_token(char out[MUSE_TOKEN_MAX + 1])
{
    LOCKED(strlcpy(out, s.token, MUSE_TOKEN_MAX + 1));
}

size_t muse_settings_hatch_token_len(void)
{
    size_t n;
    LOCKED(n = strlen(s.token));
    return n;
}

esp_err_t muse_settings_set_volume(int pct)
{
    s.volume = clampi(pct, 0, 100);
    esp_err_t err = save_u8("volume", s.volume);
    notify(MUSE_SETTING_VOLUME);
    return err;
}

esp_err_t muse_settings_set_speaker_on(bool on)
{
    s.speaker_on = on;
    esp_err_t err = save_u8("speaker", on);
    notify(MUSE_SETTING_SPEAKER);
    return err;
}

esp_err_t muse_settings_set_mic_gain(int db)
{
    s.mic_gain = clampi(db, 0, MUSE_MIC_GAIN_MAX);
    esp_err_t err = save_u8("mic_gain", s.mic_gain);
    notify(MUSE_SETTING_MIC_GAIN);
    return err;
}

esp_err_t muse_settings_set_brightness(int pct)
{
    s.brightness = clampi(pct, 10, 100);
    esp_err_t err = save_u8("bright", s.brightness);
    notify(MUSE_SETTING_BRIGHTNESS);
    return err;
}

esp_err_t muse_settings_set_sleep_s(int secs)
{
    s.sleep_s = clampi(secs, 0, 3600);
    esp_err_t err = nvs_set_u16(s_nvs, "sleep_s", s.sleep_s);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs save 'sleep_s' failed: %s", esp_err_to_name(err));
    }
    notify(MUSE_SETTING_SLEEP);
    return err;
}

void muse_settings_set_wifi_on(bool on)
{
    s.wifi_on = on;
    save_u8("wifi_on", on);
    notify(MUSE_SETTING_WIFI);
}

void muse_settings_set_agent(int idx)
{
    if (idx >= MUSE_AGENT_COUNT) {
        idx = MUSE_AGENT_HERMES;
    }
    /* The ALT slot with no profile name would strand the user on a relay target that
     * cannot resolve: fall through to Muse so the cycle stays honest. */
    if (idx == MUSE_AGENT_HERMES_ALT && !s.agent_alt_profile[0]) {
        idx = MUSE_AGENT_MUSE;
    }
    s.agent = (uint8_t)idx;
    save_u8("agent", s.agent);
    notify(MUSE_SETTING_AGENT);
}

void muse_settings_set_agent_hermes(bool hermes)
{
    muse_settings_set_agent(hermes ? MUSE_AGENT_HERMES : MUSE_AGENT_MUSE);
}

esp_err_t muse_settings_set_agent_profile(const char *name)
{
    /* Keep it to the characters the relay can put in a URL path; anything else would be
     * a profile that cannot be routed to. */
    char clean[MUSE_AGENT_PROFILE_MAX + 1] = {0};
    size_t n = 0;
    for (const char *p = name ? name : ""; *p && n < MUSE_AGENT_PROFILE_MAX; p++) {
        char ch = *p;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') {
            clean[n++] = ch;
        }
    }
    strlcpy(s.agent_alt_profile, clean, sizeof(s.agent_alt_profile));
    nvs_set_str(s_nvs, "agent_alt", s.agent_alt_profile);
    nvs_commit(s_nvs);
    /* A profile name now exists, or was just cleared: if ALT was the active slot and the
     * name is gone, drop back to Muse rather than pointing at nothing. */
    if (!s.agent_alt_profile[0] && s.agent == MUSE_AGENT_HERMES_ALT) {
        muse_settings_set_agent(MUSE_AGENT_MUSE);
    }
    ESP_LOGI(TAG, "agent alt profile: '%s'", s.agent_alt_profile);
    return ESP_OK;
}

void muse_settings_set_ble_on(bool on)
{
    s.ble_on = on;
    save_u8("ble_on", on);
    notify(MUSE_SETTING_BLE);
}

void muse_settings_set_iphone_alerts(bool on)
{
    s.iphone_alerts = on;
    save_u8("iphone_alert", on);
}

void muse_settings_set_iphone_on(bool on)
{
    s.iphone_on = on;
    save_u8("iphone_on", on);
    notify(MUSE_SETTING_IPHONE);
}

void muse_settings_set_wifi(const char *ssid, const char *pass)
{
    if (muse_link_wifi_set(ssid ? ssid : "", ssid && ssid[0] && pass ? pass : "")) {
        ESP_LOGI(TAG, "wifi network: %s", ssid && ssid[0] ? ssid : "(all forgotten)");
        notify(MUSE_SETTING_WIFI);
        return;
    }
    LOCKED({
        strlcpy(s.ssid, ssid ? ssid : "", sizeof(s.ssid));
        strlcpy(s.pass, s.ssid[0] && pass ? pass : "", sizeof(s.pass));
        save_str("ssid", s.ssid);
        save_str("pass", s.pass);
    });
    ESP_LOGI(TAG, "wifi network: %s", s.ssid[0] ? s.ssid : "(forgotten)");
    notify(MUSE_SETTING_WIFI);
}

esp_err_t muse_settings_set_hatch_host(const char *host)
{
    esp_err_t err = ESP_OK;
    LOCKED({
        strlcpy(s.host, host && host[0] ? host : DEFAULT_HOST, sizeof(s.host));
        err = save_str("host", s.host);
    });
    notify(MUSE_SETTING_HATCH);
    return err;
}

esp_err_t muse_settings_set_hatch_vm(const char *vm)
{
    esp_err_t err = ESP_OK;
    LOCKED({
        strlcpy(s.vm, vm ? vm : "", sizeof(s.vm));
        err = save_str("vm", s.vm);
    });
    notify(MUSE_SETTING_HATCH);
    return err;
}

esp_err_t muse_settings_set_hatch_token(const char *token, bool append)
{
    esp_err_t err = ESP_OK;
    LOCKED({
        size_t have = append ? strlen(s.token) : 0;
        size_t add = strlen(token ? token : "");
        if (have + add > MUSE_TOKEN_MAX) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(s.token + have, token, add);
            s.token[have + add] = '\0';
            save_str("token", s.token);
        }
    });
    if (err == ESP_OK) {
        notify(MUSE_SETTING_HATCH);
    }
    return err;
}
