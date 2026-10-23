/*
 * The iPhone link (see muse_iphone.h). Lives beside Home Link's BLE server: same NimBLE host,
 * its own advertising (ANCS solicitation, so iOS lists the gadget in Settings > Bluetooth) and
 * its own connection callback, so Link's setup session never sees the phone's connection.
 *
 * After the link is encrypted (bonded, passkey on the glass the first time) it walks a fixed
 * GATT-client script: find ANCS + AMS, their characteristics and CCCDs, subscribe, and register
 * for AMS track/player updates. ANCS notifications are fetched one at a time (the Control Point
 * allows a single outstanding request) through a small UID queue.
 */
#include "muse_iphone.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_store.h"
#include "muse_ble.h"
#include "muse_input.h"
#include "muse_link.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "muse_voice.h"

static const char *TAG = "muse_iphone";

static const ble_uuid128_t ANCS_SVC = BLE_UUID128_INIT(0xd0, 0x00, 0x2d, 0x12, 0x1e, 0x4b, 0x0f, 0xa4, 0x99, 0x4e, 0xce, 0xb5, 0x31, 0xf4, 0x05, 0x79);
static const ble_uuid128_t ANCS_NS = BLE_UUID128_INIT(0xbd, 0x1d, 0xa2, 0x99, 0xe6, 0x25, 0x58, 0x8c, 0xd9, 0x42, 0x01, 0x63, 0x0d, 0x12, 0xbf, 0x9f);
static const ble_uuid128_t ANCS_CP = BLE_UUID128_INIT(0xd9, 0xd9, 0xaa, 0xfd, 0xbd, 0x9b, 0x21, 0x98, 0xa8, 0x49, 0xe1, 0x45, 0xf3, 0xd8, 0xd1, 0x69);
static const ble_uuid128_t ANCS_DS = BLE_UUID128_INIT(0xfb, 0x7b, 0x7c, 0xce, 0x6a, 0xb3, 0x44, 0xbe, 0xb5, 0x4b, 0xd6, 0x24, 0xe9, 0xc6, 0xea, 0x22);
static const ble_uuid128_t AMS_SVC = BLE_UUID128_INIT(0xdc, 0xf8, 0x55, 0xad, 0x02, 0xc5, 0xf4, 0x8e, 0x3a, 0x43, 0x36, 0x0f, 0x2b, 0x50, 0xd3, 0x89);
static const ble_uuid128_t AMS_RC = BLE_UUID128_INIT(0xc2, 0x51, 0xca, 0xf7, 0x56, 0x0e, 0xdf, 0xb8, 0x8a, 0x4a, 0xb1, 0x57, 0xd8, 0x81, 0x3c, 0x9b);
static const ble_uuid128_t AMS_EU = BLE_UUID128_INIT(0x02, 0xc1, 0x96, 0xba, 0x92, 0xbb, 0x0c, 0x9a, 0x1f, 0x41, 0x8d, 0x80, 0xce, 0xab, 0x7c, 0x2f);
static const ble_uuid16_t CCCD = BLE_UUID16_INIT(0x2902);

/* Connection + the handles the GATT script finds. */
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_pairing, s_ready, s_adv_ours;
static struct {
    uint16_t start, end;
} s_ancs, s_ams;
static uint16_t s_ns, s_cp, s_ds, s_ns_cccd, s_ds_cccd;   /* ANCS */
static uint16_t s_rc, s_eu, s_eu_cccd;                     /* AMS */
static int s_step;

/* ANCS: one Get Notification Attributes request at a time; UIDs wait here. */
#define UIDQ 8
static uint32_t s_uidq[UIDQ];
static int s_uid_head, s_uid_n;
static bool s_req_busy;
static uint8_t s_ds_buf[400];
static int s_ds_len;

/* AMS now-playing. */
static muse_now_playing_t s_now;
static volatile uint32_t s_gen;

static void step(void);
static int on_gap(struct ble_gap_event *ev, void *arg);

/* ---------- advertising (only ours, only while the setting is on and no phone is connected) ---------- */

static void adv_start(void)
{
    if (!muse_link_ble_started() || !ble_hs_synced() || s_conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) {
        return;
    }
    struct ble_hs_adv_fields adv = { 0 };
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.sol_uuids128 = &ANCS_SVC;           /* iOS lists an ANCS-soliciting accessory in Bluetooth settings */
    adv.sol_num_uuids128 = 1;
    static const uint8_t short_name[] = "Muse";
    adv.name = short_name;
    adv.name_len = 4;
    adv.name_is_complete = 0;
    if (ble_gap_adv_set_fields(&adv) != 0) {
        return;
    }
    muse_ble_status_t b;
    muse_ble_status(&b);
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (const uint8_t *)b.name;
    rsp.name_len = (uint8_t)strlen(b.name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);
    struct ble_gap_adv_params p = { 0 };
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = 800;    /* 500 ms .. 1 s: a bonded phone finds it quickly, the battery barely notices */
    p.itvl_max = 1600;
    int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p, on_gap, NULL);
    s_adv_ours = rc == 0;
    if (rc == 0) {
        ESP_LOGI(TAG, "advertising for the iPhone as %s", b.name);
    }
}

static void adv_stop(void)
{
    if (s_adv_ours && ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
    s_adv_ours = false;
}

/* ---------- presenting a phone notification ---------- */

static const char *app_name(const char *bundle)
{
    static const struct { const char *id, *name; } known[] = {
        { "com.apple.MobileSMS", "Messages" }, { "com.apple.mobilephone", "Phone" },
        { "com.apple.mobilemail", "Mail" },     { "com.apple.mobilecal", "Calendar" },
        { "com.apple.reminders", "Reminders" }, { "net.whatsapp.WhatsApp", "WhatsApp" },
        { "com.facebook.Messenger", "Messenger" }, { "com.burbn.instagram", "Instagram" },
        { "com.tinyspeck.chatlyio", "Slack" },  { "com.hammerandchisel.discord", "Discord" },
        { "ph.telegra.Telegraph", "Telegram" }, { "com.google.Gmail", "Gmail" },
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (!strcmp(bundle, known[i].id)) {
            return known[i].name;
        }
    }
    const char *dot = strrchr(bundle, '.');
    return dot && dot[1] ? dot + 1 : bundle;
}

static void present(const char *bundle, const char *title, const char *msg)
{
    if (muse_input_dnd()) {
        return;   /* face-down: the phone has it anyway */
    }
    const char *app = app_name(bundle);
    muse_state_set_asleep(false);
    muse_input_poke();
    muse_ui_react("alert");
    muse_voice_request_notif();
    /* Two caption lines: who (or the app), then what. */
    if (title[0] && msg[0]) {
        muse_state_set_caption("%s (%s)\n%s", title, app, msg);
    } else {
        muse_state_set_caption("%s\n%s", app, title[0] ? title : msg);
    }
    muse_state_poke();
    ESP_LOGI(TAG, "notification from %s", app);
}

/* ---------- ANCS ---------- */

static int on_written(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (err->status != 0) {
        ESP_LOGW(TAG, "write %u failed: %d", attr ? attr->handle : 0, err->status);
    }
    if (arg) {   /* part of the setup script: keep walking */
        step();
    }
    return 0;
}

static void request_next(void)
{
    if (s_req_busy || !s_uid_n || !s_cp || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    uint32_t uid = s_uidq[s_uid_head];
    s_uid_head = (s_uid_head + 1) % UIDQ;
    s_uid_n--;
    /* Get Notification Attributes: AppIdentifier, Title (<= 40), Message (<= 120). */
    uint8_t cmd[] = { 0, (uint8_t)uid, (uint8_t)(uid >> 8), (uint8_t)(uid >> 16), (uint8_t)(uid >> 24),
                      0, 1, 40, 0, 3, 120, 0 };
    s_req_busy = true;
    s_ds_len = 0;
    if (ble_gattc_write_flat(s_conn, s_cp, cmd, sizeof(cmd), on_written, NULL) != 0) {
        s_req_busy = false;
    }
}

/* Parses a complete Data Source response; false = more fragments to come. */
static bool parse_ds(void)
{
    if (s_ds_len < 5) {
        return false;
    }
    char app[64] = "", title[48] = "", msg[128] = "";
    int i = 5, got = 0;
    while (got < 3) {
        if (i + 3 > s_ds_len) {
            return false;
        }
        uint8_t id = s_ds_buf[i];
        int len = s_ds_buf[i + 1] | s_ds_buf[i + 2] << 8;
        if (i + 3 + len > s_ds_len) {
            return false;
        }
        char *dst = id == 0 ? app : id == 1 ? title : id == 3 ? msg : NULL;
        size_t cap = id == 0 ? sizeof(app) : id == 1 ? sizeof(title) : sizeof(msg);
        if (dst) {
            size_t n = (size_t)len < cap - 1 ? (size_t)len : cap - 1;
            memcpy(dst, s_ds_buf + i + 3, n);
            dst[n] = 0;
        }
        i += 3 + len;
        got++;
    }
    present(app, title, msg);
    return true;
}

static void on_ns(const uint8_t *d, int n)
{
    if (n < 8) {
        return;
    }
    uint8_t event = d[0], flags = d[1];
    uint32_t uid = d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24;
    /* Added, not silent, not one of the pile that was there before we connected. */
    if (event != 0 || (flags & 0x01) || (flags & 0x04) || s_uid_n >= UIDQ) {
        return;
    }
    s_uidq[(s_uid_head + s_uid_n) % UIDQ] = uid;
    s_uid_n++;
    request_next();
}

static void on_ds(const uint8_t *d, int n)
{
    if (s_ds_len + n > (int)sizeof(s_ds_buf)) {
        s_ds_len = 0;   /* oversized: drop it, the next request starts clean */
        s_req_busy = false;
        request_next();
        return;
    }
    memcpy(s_ds_buf + s_ds_len, d, n);
    s_ds_len += n;
    if (parse_ds()) {
        s_req_busy = false;
        request_next();
    }
}

/* ---------- AMS ---------- */

static void on_eu(const uint8_t *d, int n)
{
    if (n < 3) {
        return;
    }
    uint8_t entity = d[0], attr = d[1];
    char v[64];
    int len = n - 3 < (int)sizeof(v) - 1 ? n - 3 : (int)sizeof(v) - 1;
    memcpy(v, d + 3, len);
    v[len] = 0;
    if (entity == 0 && attr == 1) {          /* Player: PlaybackInfo "state,rate,elapsed" */
        s_now.playing = v[0] == '1';
    } else if (entity == 2 && attr == 0) {   /* Track: Artist */
        strlcpy(s_now.artist, v, sizeof(s_now.artist));
    } else if (entity == 2 && attr == 2) {   /* Track: Title */
        strlcpy(s_now.title, v, sizeof(s_now.title));
    } else {
        return;
    }
    s_now.active = s_now.title[0] != 0;
    s_gen = s_gen + 1;
}

/* ---------- the GATT setup script ---------- */

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0 && svc) {
        if (arg == &s_ancs) {
            s_ancs.start = svc->start_handle;
            s_ancs.end = svc->end_handle;
        } else {
            s_ams.start = svc->start_handle;
            s_ams.end = svc->end_handle;
        }
        return 0;
    }
    if (err->status == BLE_HS_EDONE || err->status == BLE_HS_ENOENT || err->status == BLE_HS_EAPP ||
        err->status != 0) {
        step();
    }
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr) {
        const ble_uuid_t *u = &chr->uuid.u;
        if (!ble_uuid_cmp(u, &ANCS_NS.u)) s_ns = chr->val_handle;
        else if (!ble_uuid_cmp(u, &ANCS_CP.u)) s_cp = chr->val_handle;
        else if (!ble_uuid_cmp(u, &ANCS_DS.u)) s_ds = chr->val_handle;
        else if (!ble_uuid_cmp(u, &AMS_RC.u)) s_rc = chr->val_handle;
        else if (!ble_uuid_cmp(u, &AMS_EU.u)) s_eu = chr->val_handle;
        return 0;
    }
    step();
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val, const struct ble_gatt_dsc *dsc, void *arg)
{
    if (err->status == 0 && dsc) {
        if (!ble_uuid_cmp(&dsc->uuid.u, &CCCD.u)) {
            if (chr_val == s_ns) s_ns_cccd = dsc->handle;
            else if (chr_val == s_ds) s_ds_cccd = dsc->handle;
            else if (chr_val == s_eu) s_eu_cccd = dsc->handle;
        }
        return 0;
    }
    step();
    return 0;
}

static void subscribe(uint16_t cccd)
{
    static const uint8_t on[2] = { 1, 0 };
    if (!cccd || ble_gattc_write_flat(s_conn, cccd, on, sizeof(on), on_written, (void *)1) != 0) {
        step();   /* missing on this phone: carry on */
    }
}

/* One action per call; each callback calls step() again. */
static void step(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    switch (s_step++) {
    case 0:
        if (ble_gattc_disc_svc_by_uuid(s_conn, &ANCS_SVC.u, on_svc, &s_ancs) != 0) step();
        break;
    case 1:
        if (ble_gattc_disc_svc_by_uuid(s_conn, &AMS_SVC.u, on_svc, &s_ams) != 0) step();
        break;
    case 2:
        if (!s_ancs.start || ble_gattc_disc_all_chrs(s_conn, s_ancs.start, s_ancs.end, on_chr, NULL) != 0) step();
        break;
    case 3:
        if (!s_ams.start || ble_gattc_disc_all_chrs(s_conn, s_ams.start, s_ams.end, on_chr, NULL) != 0) step();
        break;
    case 4:
        if (!s_ancs.start || ble_gattc_disc_all_dscs(s_conn, s_ancs.start, s_ancs.end, on_dsc, NULL) != 0) step();
        break;
    case 5:
        if (!s_ams.start || ble_gattc_disc_all_dscs(s_conn, s_ams.start, s_ams.end, on_dsc, NULL) != 0) step();
        break;
    case 6:
        subscribe(s_ds_cccd);   /* Data Source before Notification Source, as the spec asks */
        break;
    case 7:
        subscribe(s_ns_cccd);
        break;
    case 8:
        subscribe(s_eu_cccd);
        break;
    case 9: {   /* AMS: Player -> PlaybackInfo */
        static const uint8_t reg[] = { 0, 1 };
        if (!s_eu || ble_gattc_write_flat(s_conn, s_eu, reg, sizeof(reg), on_written, (void *)1) != 0) step();
        break;
    }
    case 10: {  /* AMS: Track -> Artist, Title */
        static const uint8_t reg[] = { 2, 0, 2 };
        if (!s_eu || ble_gattc_write_flat(s_conn, s_eu, reg, sizeof(reg), on_written, (void *)1) != 0) step();
        break;
    }
    default:
        if (!s_ready) {
            s_ready = true;
            ESP_LOGI(TAG, "iPhone ready: notifications %s, media %s", s_ns_cccd ? "on" : "unavailable",
                     s_eu_cccd ? "on" : "unavailable");
        }
        break;
    }
}

static void reset_link(void)
{
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_pairing = s_ready = false;
    memset(&s_ancs, 0, sizeof(s_ancs));
    memset(&s_ams, 0, sizeof(s_ams));
    s_ns = s_cp = s_ds = s_ns_cccd = s_ds_cccd = s_rc = s_eu = s_eu_cccd = 0;
    s_step = 0;
    s_uid_n = s_uid_head = 0;
    s_req_busy = false;
    s_ds_len = 0;
    if (s_now.active) {
        memset(&s_now, 0, sizeof(s_now));
        s_gen = s_gen + 1;
    }
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    return 0;
}

static int on_gap(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_adv_ours = false;
        if (ev->connect.status != 0) {
            adv_start();
            break;
        }
        reset_link();
        s_conn = ev->connect.conn_handle;
        ESP_LOGI(TAG, "iPhone connected; securing the link");
        ble_gattc_exchange_mtu(s_conn, on_mtu, NULL);
        s_pairing = true;
        ble_gap_security_initiate(s_conn);   /* bonded: re-encrypts; new: the passkey shows */
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "iPhone disconnected (reason %d)", ev->disconnect.reason);
        reset_link();
        if (muse_settings_iphone_on()) {
            adv_start();
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        muse_ble_gap_event(ev);   /* clears the passkey on the glass */
        s_pairing = false;
        if (ev->enc_change.status == 0) {
            s_step = 0;
            step();
        } else {
            ESP_LOGW(TAG, "iPhone encryption failed (%d)", ev->enc_change.status);
        }
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        return muse_ble_gap_event(ev);   /* shows the passkey / drops a stale bond */
    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t buf[256];
        uint16_t n = OS_MBUF_PKTLEN(ev->notify_rx.om);
        n = n > sizeof(buf) ? sizeof(buf) : n;
        if (ble_hs_mbuf_to_flat(ev->notify_rx.om, buf, n, &n) != 0) {
            break;
        }
        uint16_t h = ev->notify_rx.attr_handle;
        if (h == s_ns) on_ns(buf, n);
        else if (h == s_ds) on_ds(buf, n);
        else if (h == s_eu) on_eu(buf, n);
        break;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_adv_ours = false;
        break;
    default:
        break;
    }
    return 0;
}

/* ---------- public ---------- */

static void tick(void *arg)
{
    bool on = muse_settings_iphone_on();
    if (!on) {
        adv_stop();
        return;
    }
    adv_start();   /* no-op while connected or while something else is advertising */
}

void muse_iphone_start(void)
{
    static esp_timer_handle_t t;
    if (t) {
        return;
    }
    const esp_timer_create_args_t a = { .callback = tick, .name = "iphone" };
    esp_timer_create(&a, &t);
    esp_timer_start_periodic(t, 3 * 1000000LL);
}

void muse_iphone_apply(void)
{
    if (!muse_settings_iphone_on()) {
        adv_stop();
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    } else {
        adv_start();
    }
}

muse_iphone_state_t muse_iphone_state(void)
{
    if (!muse_settings_iphone_on()) return MUSE_IPHONE_OFF;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return MUSE_IPHONE_WAITING;
    return s_pairing || !s_ready ? MUSE_IPHONE_PAIRING : MUSE_IPHONE_CONNECTED;
}

uint32_t muse_iphone_gen(void)
{
    return s_gen;
}

bool muse_iphone_now_playing(muse_now_playing_t *out)
{
    *out = s_now;
    return s_now.active;
}

void muse_iphone_bench_now(const char *title, const char *artist, bool playing)
{
    strlcpy(s_now.title, title, sizeof(s_now.title));
    strlcpy(s_now.artist, artist, sizeof(s_now.artist));
    s_now.playing = playing;
    s_now.active = title[0] != 0;
    s_gen = s_gen + 1;
}

bool muse_iphone_media(int cmd)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_rc) {
        return false;
    }
    uint8_t c = (uint8_t)cmd;
    return ble_gattc_write_flat(s_conn, s_rc, &c, 1, NULL, NULL) == 0;
}
