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

/*
 * The Muse chat session: one HTTP-over-Noise connection to the VM (the transport
 * Home Link uses: TLS, WebSocket upgrade on /v1/noise, Noise handshake, then
 * multiplexed request streams), and the push-to-talk turns that run over it.
 *
 * A turn:
 *   1. POST /api/voice/dictation, request body left open; the mic is streamed
 *      up as 24 kHz PCM16 while the button is held, then half-closed. The VM
 *      answers with NDJSON transcripts (cumulative partials, then a final).
 *   2. POST /chat/stream with the transcript. The reply arrives as events on
 *      the connection's long-lived POST /chat/subscribe stream: one or more
 *      assistant messages, each delta.message_start / text_append / message_done.
 *   3. Each finished message is shown at reading pace (see start_tts to
 *      speak it with a TTS API of your own; Muse doesn't speak gadget replies).
 * A turn has no explicit end event; like Sidekick, it settles once every
 * message is done and nothing has arrived for a few seconds.
 *
 * Everything network-side runs on one task. The voice task talks to it through
 * a command queue, a stream buffer of mic audio, an event queue (captions) and
 * a stream buffer of reply audio. A generation number tags each turn so that
 * events and audio of a cancelled turn are dropped.
 *
 * A typed turn (muse_hatch_text_turn, from the serial console) skips steps 1
 * and 3: the text goes to /chat/stream and the reply streams back to the
 * console as "@chat" lines instead of to the voice task.
 */

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "esp_netif.h"

extern "C" {
#include "cJSON.h"
#include "minimp3.h"
#include "muse_account_api.h"
extern "C" {
#include "muse_adpcm.h"
}
#include "muse_diag.h"
#include "muse_link.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_voice.h"
#include "muse_wifi.h"
}
#include "muse_chat_priv.h"

#include <xplat/noise/core/ClientSession.h>
#include <xplat/noise/core/PsaCryptoBackend.h>
#include <xplat/noise/core/ServiceCodec.h>

using namespace musegadgets::noise::core;

static const char *TAG = "muse_chat_session";

/* ---- Hermes streaming turn: the reply arrives as NDJSON lines; audio/status ride along ---- */

struct hermes_stream_t {
    esp_http_client_handle_t h;   /* while a streamed turn is in flight */
    char *line;                   /* NDJSON line being assembled (PSRAM) */
    size_t cap, len;
    bool overflow, headers_done, ended;
    bool got_heard, got_reply;
    char react[32];
};
static hermes_stream_t s_hstream;

#define NOISE_PATH "/v1/noise"
#define NOISE_PORT 443
#define IO_TIMEOUT_US (15 * 1000000LL)

#define MIC_RATE 16000
#define DICT_RATE 24000
#define DICT_CHUNK_BYTES 8192              /* ~170 ms at 24 kHz, as hatch-test sends */
#define SCRATCH (64 * 1024)                /* inbound frames reassemble up to this */
#define NDJSON_LINE_MAX (16 * 1024)               /* one NDJSON line / the chat ack */
#define SUB_LINE_MAX (256 * 1024)          /* an event line grows its buffer up to this */
#define CHAT_PART (16 * 1024)              /* one body chunk of a long typed message */
#define MP3_BUF (256 * 1024)              /* ~64 s at 32 kbps; it slides as it plays (decode) */
#define MP3_HOLD (1441 + 4)               /* the largest MP3 frame and the next header */
#define MP3_POLL_ROOM (SCRATCH + 8192)     /* stop reading the socket below this much MP3 room */
#define IN_BYTES (MIC_RATE * 2 * 8)        /* 8 s of mic backlog while connecting */
#define OUT_BYTES (MIC_RATE * 2 * 10)      /* 10 s of decoded reply (PSRAM): a whole speech piece decodes ahead, so the next piece's ~3 s fetch never gaps the speaker */
/* Chunked Muse TTS: a piece this long keeps its own fetch (1.3 s fixed + ~8 ms/char)
 * shorter than the piece before it plays, so speech never waits on a later piece. */
#define TTS_CHUNK_MIN 40
#define TTS_CHUNK_MAX 90
#define EV_TEXT 72
#define TEXT_MAX 1024                      /* a message's text, for captions timed to its speech */
#define SPEECH_CHARS_PER_S 14              /* until the speech's length is known */
#define TEXT_CHARS_PER_S 16                /* speaker off: reading pace, a little over speech */
#define TEXT_HOLD_S 2                      /* speaker off: how long a message's last lines stay up */

#define PING_US (20 * 1000000LL)
#define DEAD_US (60 * 1000000LL)           /* nothing from the server, pongs included */
#define IDLE_CLOSE_US (10 * 60 * 1000000LL)
#define AUTO_RETRY_MIN_US (5 * 1000000LL)  /* connecting without a turn, after a failure */
#define AUTO_RETRY_MAX_US (30 * 1000000LL)   /* awake, someone may be waiting to talk */
#define FINAL_TIMEOUT_US (15 * 1000000LL)  /* release -> final transcript */
#define REPLY_TIMEOUT_US (60 * 1000000LL)  /* chat posted -> first assistant message */
#define TURN_CAP_US (180 * 1000000LL)
#define SETTLE_US (3 * 1000000LL)          /* quiet period that ends a turn */
#define BUSY_HOLD_US (20 * 1000000LL)      /* how long a busy agent keeps it open */
#define TEXT_REPLY_TIMEOUT_US (5 * 60 * 1000000LL)   /* typed turns: agents can work a while */
#define TEXT_TURN_CAP_US (15 * 60 * 1000000LL)
#define TEXT_BUSY_HOLD_US (5 * 60 * 1000000LL)
/*
 * The VM's streaming dictation has no ASR behind it right now, so each press
 * goes to the chat as a voice note, the way the phone app sends them, and the
 * server transcribes it. Set to 0 to stream to /api/voice/dictation instead.
 */
#define VOICE_NOTE 1
#define NOTE_MAX_BYTES (MIC_RATE * 2 * 20) /* 20 s of 16 kHz PCM; Muse stops at 15 */
#define NOTE_PART_BYTES (DICT_CHUNK_BYTES / 4 * 3)   /* staged PCM that base64s to one body chunk */

#define MAX_MSGS 8

/* ---- Voice task <-> session task ---- */

enum cmd_type_t : uint8_t { CMD_CONNECT, CMD_FORGET, CMD_AGENT, CMD_BEGIN, CMD_END, CMD_CANCEL, CMD_TEXT, CMD_TEXT_CANCEL, CMD_WAKE };

struct cmd_t {
    cmd_type_t type;
    uint32_t gen;
    char *text;              /* CMD_TEXT: malloc'd, freed by the session task */
};

struct ev_t {
    muse_hatch_ev_t type;
    uint32_t gen;
    char text[EV_TEXT];
};

static QueueHandle_t s_cmds, s_events;
static StreamBufferHandle_t s_in, s_out;
static std::atomic<uint32_t> s_gen{0};
static std::atomic<bool> s_resting{false};

/* ---- Connection ---- */

struct conn_t {
    esp_tls_t *tls;
    PsaCryptoBackend *crypto;
    ClientSession *session;
    uint8_t *ws, *rx, *tf, *sr, *svc, *env;   /* PSRAM scratch */
    int64_t next_id;
    int64_t sub_id;
    int64_t last_rx_us, last_ping_us, last_use_us;
};

static conn_t s_conn;
static bool s_connected;
static muse_hatch_vm_t s_vm;       /* cached per-VM credentials */
static bool s_vm_direct;           /* s_vm.vm_token is the device token itself */
static char s_host[MUSE_HOST_MAX + 1];
/* When to connect without a turn asking: once Wi-Fi is up, again with backoff
 * if that fails or the connection drops, not after an idle close. */
static int64_t s_auto_next_us;
static int64_t s_auto_backoff_us = AUTO_RETRY_MIN_US;

/* ---- Streams on the connection ---- */

enum kind_t : uint8_t { K_NONE, K_SUB, K_DICT, K_CHAT, K_TTS };

struct stream_t {
    int64_t id;
    kind_t kind;
    int status;
    int msg;                 /* K_TTS: index into the turn's messages */
    char *line;              /* NDJSON line / buffered body */
    size_t cap;              /* line's size: NDJSON_LINE_MAX, grown for long event lines */
    size_t len;
    bool overflow;
};

#define MAX_STREAMS 6
static stream_t s_streams[MAX_STREAMS];

/* ---- The current turn ---- */

enum phase_t : uint8_t { P_IDLE, P_LISTEN, P_WAIT_FINAL, P_WAIT_REPLY };

enum tts_t : uint8_t { TTS_NONE, TTS_QUEUED, TTS_ACTIVE, TTS_FINISHED };

struct msg_t {
    char id[80];
    size_t len;              /* reply text length so far */
    char tail[128];          /* its last characters, for the caption */
    bool done;
    tts_t tts;
    bool store_want;         /* done: the reading view's add runs in start_tts */
    uint32_t pcm_start;      /* where its speech starts in the reply audio */
    uint32_t pcm_frames;     /* how long it is; 0 until the MP3 has all arrived */
};

struct resampler_t {
    uint32_t step;           /* Q16 input samples per output sample */
    uint32_t pos;
    int16_t prev;
};

struct turn_t {
    phase_t phase;
    uint32_t gen;
    bool text;               /* typed at the console: the reply goes there, unspoken */
    bool hermes;             /* owning backend, pinned at turn_start (live hermes_mode can flip mid-turn) */
    bool end_requested, end_sent, chat_posted, acked;
    int64_t dict_id, chat_id;
    int64_t start_us, end_sent_us, chat_us, last_event_us, last_content_us;
    uint64_t sent24;         /* 24 kHz frames sent to dictation */
    resampler_t up;
    uint8_t *chunk;          /* DICT_CHUNK_BYTES, plus the note's tail */
    size_t chunk_len;
    uint8_t *note;           /* NOTE_PART_BYTES of 16 kHz PCM waiting for base64 */
    size_t note_len;
    size_t pcm_bytes;        /* the note so far */
    bool up_started;         /* the note's upload began at the press (s_up) */
    size_t body_sent;
    char *texts;             /* MAX_MSGS * TEXT_MAX: each message's text */
    uint32_t pcm_out;        /* reply audio frames handed to the voice task */
    char committed[512];     /* finals that arrived before the half-close */
    char partial[512];
    char user_ids[2][80];
    muse_chat_rejected_t rejected;
    msg_t msgs[MAX_MSGS];
    int nmsgs;
    bool agent_busy;
    /* TTS */
    int tts_msg;             /* message being fetched (or shown, speaker off), or -1 */
    bool silent;             /* speaker off: tts_msg is paced by silence, not fetched */
    uint8_t *mp3;            /* MP3_BUF */
    size_t mp3_len;
    bool mp3_ended;
    size_t tts_off;          /* Muse: characters of the speaking message already fetched */
    bool tts_more;           /* Muse chunked fetch: another piece is still to come */
    uint8_t tts_fails;       /* pieces that failed this message (3 = give up) */
    mp3dec_t dec;
    resampler_t down;
    int kbps;
    int down_rate;
};

/* 10 KB, most of it the MP3 decoder: in PSRAM on boards that let static data go
 * there (the AIPI), so Wi-Fi setup and TLS have the internal RAM. */
EXT_RAM_BSS_ATTR static turn_t s_turn;

/* Turn milestones, logged together when the turn ends. */
enum mark_t : uint8_t { M_RELEASE, M_SENT, M_ACK, M_TEXT, M_DONE, M_TTS, M_MP3, M_AUDIO, M_COUNT };
static const char *const MARK_NAMES[M_COUNT] = { "release", "sent", "ack", "text", "done", "tts", "mp3", "audio" };
static int64_t s_marks[M_COUNT];
static char s_reply_shown[EV_TEXT];   /* the pre-speech caption last sent */

static void mark(mark_t m);
static int64_t s_think_poke_us;   /* last 'working' mood re-fire */
static bool open_note(void);
static void log_marks(void);
static int16_t *s_pcm;       /* MINIMP3_MAX_SAMPLES_PER_FRAME */
static int16_t *s_pcm16;     /* resampled output */
static int64_t s_last_seq;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static void mark(mark_t m)
{
    if (!s_marks[m]) {
        s_marks[m] = now_us();
    }
}

static void log_marks(void)
{
    char line[160];
    int n = 0;
    for (int i = M_SENT; i < M_COUNT && n < (int)sizeof(line); i++) {
        if (s_marks[M_RELEASE] && s_marks[i]) {
            n += snprintf(line + n, sizeof(line) - n, " %s +%d", MARK_NAMES[i],
                          (int)((s_marks[i] - s_marks[M_RELEASE]) / 1000));
        }
    }
    ESP_LOGI(TAG, "timing (ms after release):%s", n ? line : " none");
}

static void *psram_alloc(size_t n)
{
    return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

static void *json_alloc(size_t n)
{
    return psram_alloc(n);
}

/* ---- Events to the voice task ---- */

static void emit(muse_hatch_ev_t type, const char *text)
{
    if (s_turn.text) {
        return;   /* typed turns report to the console instead */
    }
    ev_t ev = {};
    ev.type = type;
    ev.gen = s_turn.gen;
    if (type == MUSE_HATCH_EV_HEARD) {
        muse_hatch_tail_words(text ? text : "", ev.text, sizeof(ev.text));
    } else if (text) {
        strlcpy(ev.text, text, sizeof(ev.text));
    }
    /* Captions are lossy; the end of a turn, and whether the note got there, must get through. */
    TickType_t wait = (type == MUSE_HATCH_EV_DONE || type == MUSE_HATCH_EV_ERROR || type == MUSE_HATCH_EV_SENT)
                          ? pdMS_TO_TICKS(200)
                          : 0;
    xQueueSend(s_events, &ev, wait);
}

/* ---- TLS / WebSocket (from hatch-link's noise_control.cpp) ---- */

static bool write_all(esp_tls_t *tls, const void *buf, size_t len)
{
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    int64_t deadline = now_us() + IO_TIMEOUT_US;
    while (len) {
        if (now_us() >= deadline) {
            return false;
        }
        ssize_t n = esp_tls_conn_write(tls, p, len);
        if (n > 0) {
            p += n;
            len -= n;
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            ESP_LOGW(TAG, "tls write: %d (errno %d)", (int)n, errno);
            return false;
        }
    }
    return true;
}

/* Reads exactly len bytes, waiting up to the I/O timeout. */
static bool read_all(esp_tls_t *tls, uint8_t *buf, size_t len)
{
    int64_t deadline = now_us() + IO_TIMEOUT_US;
    while (len) {
        if (now_us() >= deadline) {
            return false;
        }
        ssize_t n = esp_tls_conn_read(tls, buf, len);
        if (n > 0) {
            buf += n;
            len -= n;
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            return false;
        }
    }
    return true;
}

static bool ws_send_binary(esp_tls_t *tls, const uint8_t *payload, size_t len)
{
    uint8_t hdr[14];
    size_t hl;
    hdr[0] = 0x82;   /* FIN | binary */
    if (len < 126) {
        hdr[1] = 0x80 | len;
        hl = 2;
    } else if (len <= 65535) {
        hdr[1] = 0x80 | 126;
        hdr[2] = len >> 8;
        hdr[3] = len & 0xff;
        hl = 4;
    } else {
        hdr[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++) {
            hdr[2 + i] = (uint64_t)len >> (56 - 8 * i);
        }
        hl = 10;
    }
    uint32_t key = esp_random();
    memcpy(hdr + hl, &key, 4);
    hl += 4;
    if (!write_all(tls, hdr, hl)) {
        return false;
    }
    const uint8_t *mk = reinterpret_cast<const uint8_t *>(&key);
    uint8_t chunk[512];
    for (size_t off = 0; off < len;) {
        size_t n = len - off < sizeof(chunk) ? len - off : sizeof(chunk);
        for (size_t i = 0; i < n; i++) {
            chunk[i] = payload[off + i] ^ mk[(off + i) & 3];
        }
        if (!write_all(tls, chunk, n)) {
            return false;
        }
        off += n;
    }
    return true;
}

static bool ws_send_ping(esp_tls_t *tls)
{
    uint8_t frame[6] = { 0x89, 0x80 };
    uint32_t key = esp_random();
    memcpy(frame + 2, &key, 4);
    return write_all(tls, frame, sizeof(frame));
}

/*
 * Reads one WebSocket frame. With `wait` false, returns -2 at once if nothing
 * is pending. Returns the payload length, -3 for a ping/pong (answered here),
 * 0 when the peer closed, -1 on error.
 */
static ssize_t ws_recv(esp_tls_t *tls, uint8_t *buf, size_t cap, bool wait)
{
    uint8_t hdr[2];
    int64_t deadline = now_us() + IO_TIMEOUT_US;
    for (;;) {
        ssize_t n = esp_tls_conn_read(tls, hdr, 1);
        if (n == 1) {
            break;
        }
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            if (!wait) {
                return -2;
            }
            if (now_us() >= deadline) {
                return -1;
            }
            vTaskDelay(1);
            continue;
        }
        return n == 0 ? 0 : -1;
    }
    if (!read_all(tls, hdr + 1, 1)) {
        return -1;
    }
    uint8_t opcode = hdr[0] & 0x0f;
    bool masked = hdr[1] & 0x80;
    uint64_t plen = hdr[1] & 0x7f;
    uint8_t ext[8];
    if (plen == 126) {
        if (!read_all(tls, ext, 2)) {
            return -1;
        }
        plen = (ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        if (!read_all(tls, ext, 8)) {
            return -1;
        }
        plen = 0;
        for (int i = 0; i < 8; i++) {
            plen = (plen << 8) | ext[i];
        }
    }
    uint8_t mask[4] = { 0 };
    if (masked && !read_all(tls, mask, 4)) {
        return -1;
    }
    if (plen > cap) {
        ESP_LOGE(TAG, "WS frame too large: %llu", (unsigned long long)plen);
        return -1;
    }
    if (!read_all(tls, buf, plen)) {
        return -1;
    }
    if (masked) {
        for (size_t i = 0; i < plen; i++) {
            buf[i] ^= mask[i & 3];
        }
    }
    if (opcode == 0x8) {
        ESP_LOGW(TAG, "WS close frame");
        return -1;
    }
    if (opcode == 0x9) {
        uint8_t pong[6] = { 0x8A, (uint8_t)(0x80 | plen), 0, 0, 0, 0 };   /* mask key 0 */
        write_all(tls, pong, sizeof(pong));
        if (plen) {
            write_all(tls, buf, plen);
        }
        return -3;
    }
    if (opcode == 0xA) {
        return -3;
    }
    return (ssize_t)plen;
}

/* Query escaping as hatch-link's noise_upgrade.h: keeps RFC 3986 unreserved plus !~*'(). */
static void query_escape(const char *in, char *out, size_t cap)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char c = *in;
        if (isalnum(c) || strchr("-_.!~*'()", c)) {
            out[o++] = c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
}

/* Returns the HTTP status of the upgrade (101 on success), or 0. */
static int ws_upgrade(esp_tls_t *tls, const char *vm_id, const char *token)
{
    char id[3 * 128 + 1];
    query_escape(vm_id, id, sizeof(id));
    size_t cap = 512 + strlen(id) + strlen(token) + strlen(s_host);
    char *req = static_cast<char *>(malloc(cap));
    if (!req) {
        return 0;
    }
    int len = snprintf(req, cap,
                       "GET " NOISE_PATH "?vm_id=%s HTTP/1.1\r\n"
                       "Host: %s\r\n"
                       "Authorization: Bearer %s\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Version: 13\r\n"
                       "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                       "\r\n",
                       id, s_host, token);
    bool sent = write_all(tls, req, len);
    free(req);
    if (!sent) {
        return 0;
    }
    /* Edge error responses carry a ~1.4 KB Proxy-Status header. */
    const size_t kCap = 4096;
    char *hdr = static_cast<char *>(malloc(kCap));
    if (!hdr) {
        return 0;
    }
    size_t have = 0;
    int status = 0;
    int64_t deadline = now_us() + IO_TIMEOUT_US;
    while (now_us() < deadline && have < kCap) {
        ssize_t n = esp_tls_conn_read(tls, hdr + have, kCap - have);
        if (n > 0) {
            size_t from = have >= 3 ? have - 3 : 0;
            have += n;
            bool end = false;
            for (size_t i = from; i + 3 < have; i++) {
                if (!memcmp(hdr + i, "\r\n\r\n", 4)) {
                    end = true;
                    break;
                }
            }
            if (end) {
                break;
            }
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            break;
        }
    }
    if (have >= 12 && !memcmp(hdr, "HTTP/", 5)) {
        status = atoi(hdr + 9);
    }
    free(hdr);
    return status;
}

static bool noise_handshake(conn_t *c)
{
    ByteSpan out(c->ws, ClientSession::kMaxOutboundWebSocketPayloadSize);
    auto m1 = c->session->WriteHandshakeMessage1(out);
    if (!m1.ok() || !ws_send_binary(c->tls, c->ws, m1.size())) {
        ESP_LOGE(TAG, "handshake msg1 failed");
        return false;
    }
    ssize_t n = ws_recv(c->tls, c->rx, SCRATCH, true);
    if (n <= 0) {
        ESP_LOGE(TAG, "handshake msg2 not received (%d)", (int)n);
        return false;
    }
    uint8_t extra[256];
    size_t extra_len = 0;
    Status st = c->session->ReadHandshakeMessage2(ConstByteSpan(c->rx, n), ByteSpan(extra, sizeof(extra)), extra_len);
    if (!st.ok()) {
        ESP_LOGE(TAG, "handshake msg2: %s", st.str());
        return false;
    }
    /* The owner's msg3 payload is an empty protobuf; the bearer header authenticated us. */
    auto m3 = c->session->WriteHandshakeMessage3(ConstByteSpan(), out);
    if (!m3.ok() || !ws_send_binary(c->tls, c->ws, m3.size())) {
        ESP_LOGE(TAG, "handshake msg3 failed");
        return false;
    }
    return c->session->isEstablished();
}

static bool flush_outbound(conn_t *c)
{
    while (c->session->HasOutboundWebSocketPayload()) {
        auto r = c->session->WriteNextOutboundWebSocketPayload(
            ByteSpan(c->ws, ClientSession::kMaxOutboundWebSocketPayloadSize));
        if (!r.ok() || !ws_send_binary(c->tls, c->ws, r.size())) {
            ESP_LOGE(TAG, "send failed");
            return false;
        }
    }
    c->last_use_us = now_us();
    return true;
}

/* ---- Streams ---- */

static stream_t *find_stream(int64_t id)
{
    for (auto &s : s_streams) {
        if (s.kind != K_NONE && s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

/* A handler may end a stream and a new one may take its slot. */
static bool alive(const stream_t *s, int64_t id)
{
    return s->kind != K_NONE && s->id == id;
}

static void close_stream(stream_t *s)
{
    if (s) {
        s->kind = K_NONE;
    }
}

static bool send_reset(int64_t id)
{
    if (!s_connected || id <= 0) {
        return true;
    }
    close_stream(find_stream(id));
    ResetView rv{ ResetCode::Cancelled, StringView("cancelled") };
    auto r = s_conn.session->StartOutboundReset(ServiceType::Daemon, id, rv, ByteSpan(s_conn.svc, SCRATCH),
                                                ByteSpan(s_conn.env, SCRATCH));
    return r.ok() && flush_outbound(&s_conn);
}

/* Opens a request stream. Returns its id, or 0 if the connection failed. */
static int64_t open_stream(kind_t kind, const char *verb, const char *path, const char *content_type,
                           const char *accept, const char *body, bool end_body)
{
    stream_t *slot = nullptr;
    for (auto &s : s_streams) {
        if (s.kind == K_NONE) {
            slot = &s;
            break;
        }
    }
    if (!slot) {
        ESP_LOGE(TAG, "no free stream slot");
        return 0;
    }
    char req_id[40];
    snprintf(req_id, sizeof(req_id), "muse-%08lx-%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    HeaderView hdrs[4];
    size_t nh = 0;
    hdrs[nh++] = { StringView("x-request-id"), StringView(req_id) };
    hdrs[nh++] = { StringView("x-app-id"), StringView("hatch-web") };
    if (content_type) {
        hdrs[nh++] = { StringView("Content-Type"), StringView(content_type) };
    }
    if (accept) {
        hdrs[nh++] = { StringView("accept"), StringView(accept) };
    }
    int64_t id = s_conn.next_id++;
    ApplicationRequestView req;
    req.verb = StringView(verb);
    req.path = StringView(path);
    req.headers = Span<const HeaderView>(hdrs, nh);
    req.body = body ? ConstByteSpan(reinterpret_cast<const uint8_t *>(body), strlen(body)) : ConstByteSpan();
    req.end_body = end_body;
    auto r = s_conn.session->StartOutboundApplicationRequest(ServiceType::Daemon, id, req,
                                                             ByteSpan(s_conn.svc, SCRATCH),
                                                             ByteSpan(s_conn.env, SCRATCH));
    if (!r.ok()) {
        ESP_LOGE(TAG, "%s %s: %s", verb, path, r.status().str());
        return 0;
    }
    if (!flush_outbound(&s_conn)) {
        return 0;
    }
    slot->id = id;
    slot->kind = kind;
    slot->status = 0;
    slot->msg = -1;
    slot->len = 0;
    slot->overflow = false;
    ESP_LOGI(TAG, "stream %lld: %s %s", (long long)id, verb, path);
    return id;
}

static bool send_body(int64_t id, const uint8_t *data, size_t len, bool end_body)
{
    BodyChunkView chunk{ ConstByteSpan(data, len), end_body };
    auto r = s_conn.session->StartOutboundBodyChunk(ServiceType::Daemon, id, chunk, ByteSpan(s_conn.svc, SCRATCH),
                                                    ByteSpan(s_conn.env, SCRATCH));
    if (!r.ok()) {
        ESP_LOGE(TAG, "body chunk: %s", r.status().str());
        return false;
    }
    return flush_outbound(&s_conn);
}

/* ---- Connect / disconnect ---- */

static void disconnect(const char *why)
{
    if (s_conn.tls) {
        ESP_LOGI(TAG, "disconnect: %s", why);
        esp_tls_conn_destroy(s_conn.tls);
    }
    if (s_conn.session) {
        s_conn.session->~ClientSession();
        heap_caps_free(s_conn.session);
    }
    if (s_conn.crypto) {
        s_conn.crypto->~PsaCryptoBackend();
        heap_caps_free(s_conn.crypto);
    }
    uint8_t *bufs[] = { s_conn.ws, s_conn.rx, s_conn.tf, s_conn.sr, s_conn.svc, s_conn.env };
    for (uint8_t *b : bufs) {
        heap_caps_free(b);
    }
    s_conn = conn_t{};
    for (auto &s : s_streams) {
        s.kind = K_NONE;
    }
    s_connected = false;
}

#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
/* The last TLS session with the VM host: a reconnect (every switch back to Muse) resumes it
 * instead of verifying the certificate chain and doing a key exchange again -- that software
 * ECDSA/ECDHE work took ~1.5 s and starved the display through the shared cache while it ran. */
static esp_tls_client_session_t *s_tls_session;
#endif

static void forget_vm(void)
{
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    esp_tls_free_client_session(s_tls_session);   /* a new host or account starts clean */
    s_tls_session = nullptr;
#endif
    free(s_vm.vm_token);
    s_vm = muse_hatch_vm_t{};
    s_vm_direct = false;
}

/* Fills s_vm from the account API (or the token itself). */
static bool resolve_vm(char *err, size_t err_cap)
{
    if (s_vm.vm_token) {
        return true;
    }
    char want[MUSE_VM_MAX + 1];
    muse_settings_hatch_vm(want);
    if (!muse_settings_hatch_token_len()) {
        /* No token of our own: use Link's Hatch account. */
        if (muse_link_hatch_vm(want, s_vm.vm_id, sizeof(s_vm.vm_id), s_vm.vm_name, sizeof(s_vm.vm_name),
                               &s_vm.vm_token)) {
            ESP_LOGI(TAG, "VM %s (%s) via Link", s_vm.vm_id, s_vm.vm_name);
            return true;
        }
        strlcpy(err, muse_link_hatch_linked() ? "Can't reach Muse's server" : "Not paired", err_cap);
        return false;
    }
    char *token = static_cast<char *>(malloc(MUSE_TOKEN_MAX + 1));
    if (!token) {
        strlcpy(err, "Out of memory", err_cap);
        return false;
    }
    muse_settings_hatch_token(token);
    /* The hatch slot may hold an identity-class token (the pairing's SDK token, "mgst_").
     * api.muse.ai refuses that as a Bearer -- by design -- so asking with it only produced
     * a 401 that fed the app's auth-failure machinery (and, once, a false unpair). Skip
     * straight to the app layer's real access token. */
    bool identity_class = strncmp(token, "mgst_", 5) == 0;
    int rc = identity_class ? MUSE_HATCH_API_AUTH : muse_hatch_api_find_vm(token, want, &s_vm);
    if (rc == 0) {
        ESP_LOGI(TAG, "VM %s (%s)", s_vm.vm_id, s_vm.vm_name);
        free(token);
        return true;
    }
    /* The hatch slot may hold an identity-class token (the pairing's SDK token), which the
     * account API refuses as a Bearer. The pairing ALSO delivered the real access token —
     * the app layer stores + refreshes it: ask there once before giving up. */
    if (rc == MUSE_HATCH_API_AUTH) {
        if (identity_class) {
            ESP_LOGI(TAG, "hatch slot holds an SDK token; asking the app layer for the bearer");
        }
        char *bearer = static_cast<char *>(malloc(MUSE_TOKEN_MAX + 1));
        if (bearer) {
            int n = muse_link_vm_bearer(bearer, MUSE_TOKEN_MAX + 1);
            if (n > 0) {
                rc = muse_hatch_api_find_vm(bearer, want, &s_vm);
                if (rc == 0) {
                    ESP_LOGI(TAG, "VM %s (%s) via the app-layer bearer", s_vm.vm_id, s_vm.vm_name);
                    free(bearer);
                    free(token);
                    return true;
                }
            }
            free(bearer);
        }
    }
    if (want[0]) {
        /* Not a device token (or the API is down): try it as the VM's own token. */
        ESP_LOGW(TAG, "account API %s; using the token for VM %s directly",
                 rc == MUSE_HATCH_API_AUTH ? "rejected the token" : "failed", want);
        strlcpy(s_vm.vm_id, want, sizeof(s_vm.vm_id));
        s_vm.vm_token = token;
        s_vm_direct = true;
        return true;
    }
    free(token);
    strlcpy(err, rc == MUSE_HATCH_API_AUTH ? "Token rejected" : "Can't reach Muse's server", err_cap);
    return false;
}

static bool open_subscription(void)
{
    s_last_seq = 0;
    s_conn.sub_id = open_stream(K_SUB, "POST", "/chat/subscribe", "application/json", "application/x-ndjson", "{}",
                                true);
    return s_conn.sub_id != 0;
}

static bool connect_once(char *err, size_t err_cap, int *http_status)
{
    *http_status = 0;
    conn_t &c = s_conn;
    c.tls = esp_tls_init();
    if (!c.tls) {
        strlcpy(err, "Out of memory", err_cap);
        return false;
    }
    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    cfg.client_session = s_tls_session;   /* a refused ticket falls back to a full handshake */
#endif
    int64_t t0 = now_us();
    if (esp_tls_conn_new_sync(s_host, strlen(s_host), NOISE_PORT, &cfg, c.tls) != 1) {
        snprintf(err, err_cap, "Can't reach %s", s_host);
        return false;
    }
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    if (esp_tls_client_session_t *fresh = esp_tls_get_client_session(c.tls)) {
        esp_tls_free_client_session(s_tls_session);
        s_tls_session = fresh;
    }
    ESP_LOGI(TAG, "TLS to %s in %d ms", s_host, (int)((now_us() - t0) / 1000));
#endif
    int status = ws_upgrade(c.tls, s_vm.vm_id, s_vm.vm_token);
    *http_status = status;
    if (status != 101) {
        ESP_LOGW(TAG, "upgrade rejected: HTTP %d", status);
        if (status == 401 || status == 403) {
            strlcpy(err, "VM refused the token", err_cap);
        } else {
            snprintf(err, err_cap, "VM connect failed (HTTP %d)", status);
        }
        return false;
    }
    void *mem = psram_alloc(sizeof(PsaCryptoBackend));
    c.crypto = mem ? new (mem) PsaCryptoBackend() : nullptr;
    mem = c.crypto ? psram_alloc(sizeof(ClientSession)) : nullptr;
    c.session = mem ? new (mem) ClientSession(*c.crypto) : nullptr;
    c.ws = static_cast<uint8_t *>(psram_alloc(ClientSession::kMaxOutboundWebSocketPayloadSize));
    c.rx = static_cast<uint8_t *>(psram_alloc(SCRATCH));
    c.tf = static_cast<uint8_t *>(psram_alloc(SCRATCH));
    c.sr = static_cast<uint8_t *>(psram_alloc(SCRATCH));
    c.svc = static_cast<uint8_t *>(psram_alloc(SCRATCH));
    c.env = static_cast<uint8_t *>(psram_alloc(SCRATCH));
    if (!c.session || !c.ws || !c.rx || !c.tf || !c.sr || !c.svc || !c.env) {
        strlcpy(err, "Out of memory", err_cap);
        return false;
    }
    if (!noise_handshake(&c)) {
        strlcpy(err, "Noise handshake failed", err_cap);
        return false;
    }
    int fd = -1;
    if (esp_tls_get_conn_sockfd(c.tls, &fd) != ESP_OK || fd < 0) {
        strlcpy(err, "Socket error", err_cap);
        return false;
    }
    lwip_fcntl(fd, F_SETFL, lwip_fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    c.next_id = 1;
    c.last_rx_us = c.last_ping_us = c.last_use_us = now_us();
    s_connected = true;
    if (!open_subscription()) {
        strlcpy(err, "Subscribe failed", err_cap);
        return false;
    }
    ESP_LOGI(TAG, "connected to VM %s in %d ms", s_vm.vm_id, (int)((now_us() - t0) / 1000));
    return true;
}

/* Hermes is the active agent: voice goes to the relay, and there is no Muse link to keep. */
static bool hermes_mode(void)
{
    return muse_settings_relay_configured() && muse_settings_agent_hermes();
}

/* The relay profile query for the active agent: "&profile=<name>" for the ALT Hermes
 * slot, empty for Muse and for the default Hermes (agents.json routes the bare paths).
 * The name is validated [A-Za-z0-9_-] at set time (muse_settings_set_agent_profile), so it
 * is URL-safe as-is. */
static const char *agent_profile_query(void)
{
    static char q[MUSE_AGENT_PROFILE_MAX + 16];
    char name[MUSE_AGENT_PROFILE_MAX + 1];
    muse_settings_agent_profile(name);
    q[0] = '\0';
    if (name[0] && muse_settings_agent_hermes()) {
        snprintf(q, sizeof(q), "&profile=%s", name);
    }
    return q;
}


/*
 * Muse must come back by itself. Its link and a reply need ~1 MB of PSRAM; with Hermes's note
 * buffer (then 640 KB of PCM) still held, connects failed "Out of memory" for good -- until a
 * restart (2026-10-24).
 * That buffer is now let go off Hermes (hatch_task), and if Muse still can't be reached the
 * last resort is the restart a person would do: Muse failing for HEAL_AFTER_US (3+ failed
 * attempts, the latest within a minute) while Wi-Fi is up and nothing is happening. Never in
 * the first 10 minutes of a boot (so it can't loop: a boot loop costs the Muse pairing), and
 * each one in a row waits twice as long, up to an hour; any successful connect resets that.
 */
#define HEAL_AFTER_US (5 * 60 * 1000000LL)
#define HEAL_MIN_UPTIME_US (10 * 60 * 1000000LL)
#define HEAL_MAX_UPTIME_US (60 * 60 * 1000000LL)
#define HEAL_MAGIC 0x4845414Cu   /* "HEAL" */
static RTC_NOINIT_ATTR uint32_t s_heal_magic, s_heal_count;   /* restarts in a row, kept across them */
static struct {
    int fails;
    int64_t down_since_us, last_fail_us;
} s_heal;

static void heal_check(void)
{
    if (s_heal_magic != HEAL_MAGIC) {
        s_heal_magic = HEAL_MAGIC;   /* first boot after power-on */
        s_heal_count = 0;
    }
    int64_t now = now_us();
    if (s_connected || !muse_wifi_connected() || !muse_hatch_configured()) {
        s_heal.fails = 0;
        s_heal.down_since_us = 0;
        return;
    }
    int64_t min_up = HEAL_MIN_UPTIME_US << (s_heal_count < 3 ? s_heal_count : 3);
    min_up = min_up < HEAL_MAX_UPTIME_US ? min_up : HEAL_MAX_UPTIME_US;
    if (s_heal.fails < 3 || now - s_heal.down_since_us < HEAL_AFTER_US || now - s_heal.last_fail_us > 60 * 1000000LL ||
        now < min_up || s_turn.phase != P_IDLE) {
        return;
    }
    s_heal_count++;
    ESP_LOGE(TAG, "Muse unreachable for %d min (%d failed attempts): restarting (%u in a row)",
             (int)((now - s_heal.down_since_us) / 60000000LL), s_heal.fails, (unsigned)s_heal_count);
    muse_diag_note_restart("muse_unreachable");
    esp_restart();
}

static bool ensure_connected(void)
{
    if (s_connected || hermes_mode()) {
        return true;
    }
    if (!muse_hatch_configured() || !muse_wifi_connected()) {
        return false;
    }
    muse_hatch_report(MUSE_HATCH_TESTING, "Connecting...");
    muse_settings_hatch_host(s_host);
    if (!s_host[0]) {
        strlcpy(s_host, "hatch.metaaivm.com", sizeof(s_host));
    }
    char err[48] = "";
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!resolve_vm(err, sizeof(err))) {
            break;
        }
        int status;
        if (connect_once(err, sizeof(err), &status)) {
            /* The settings page shows the VM in its VM ID row. */
            muse_hatch_report_vm(s_vm.vm_id);
            muse_hatch_report(MUSE_HATCH_REACHABLE, "");
            s_heal.fails = 0;
            s_heal.down_since_us = 0;
            s_heal_count = 0;
            return true;
        }
        disconnect(err);
        if (status == 0 && attempt == 0) {
            vTaskDelay(pdMS_TO_TICKS(1500));   /* just joined a network: DNS and the route may need a moment */
            continue;
        }
        /* A refused VM token may just be stale: fetch a fresh one once. */
        if ((status == 401 || status == 403) && !s_vm_direct) {
            forget_vm();
            continue;
        }
        break;
    }
    ESP_LOGW(TAG, "connect failed: %s (int free %uK, largest %uK; psram free %uK, largest %uK)", err,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    muse_hatch_report(MUSE_HATCH_UNREACHABLE, err);
    s_heal.fails++;
    s_heal.last_fail_us = now_us();
    if (!s_heal.down_since_us) {
        s_heal.down_since_us = s_heal.last_fail_us;
    }
    if (s_auto_next_us == INT64_MAX) {
        s_auto_next_us = now_us() + AUTO_RETRY_MIN_US;   /* failing: keep trying on our own, not just at the next press */
    }
    return false;
}

/* ---- Resampling ---- */

static void resampler_init(resampler_t *r, int in_rate, int out_rate)
{
    r->step = (uint32_t)(((uint64_t)in_rate << 16) / out_rate);
    r->pos = 0;
    r->prev = 0;
}

/* Linear interpolation; state carries across calls. out must hold n*out/in + 2. */
static size_t resample(resampler_t *r, const int16_t *in, size_t n, int16_t *out)
{
    size_t o = 0;
    if (!n) {
        return 0;
    }
    /* Position 0 is the previous call's last sample, k is in[k-1]. */
    while ((r->pos >> 16) < n) {
        size_t i = r->pos >> 16;
        int32_t a = i ? in[i - 1] : r->prev;
        int32_t b = in[i];
        /* (b - a) spans 17 bits and the fraction 16, so the product needs 64. */
        out[o++] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * (int64_t)(r->pos & 0xffff)) >> 16));
        r->pos += r->step;
    }
    r->pos -= n << 16;
    r->prev = in[n - 1];
    return o;
}

/* ---- Hermes: voice goes to a relay on the user's computer, which answers in text ---- */

#define HERMES_MAX_BYTES NOTE_MAX_BYTES

extern "C" bool muse_ui_react(const char *name);   /* muse_ui.h has no C++ guards */
/* main/image_fetch.c: downloads a JPEG and draws it on the screen. */
extern "C" bool image_fetch_buffer(const uint8_t *jpeg, size_t len, int row, void (*done)(const void *, void *),
                                   void *user, const char **code, const char **message);
static void image_done(const void *, void *) {}

/* SHODAN is deep and slow; Hermes speaks at the voice's own pitch. */
static int pitch_pct(void)
{
    return hermes_mode() ? 100 : CONFIG_MUSE_TTS_PITCH_PCT;
}

static char s_hreact[24];     /* Hermes's mood for this reply, acted out when its speech starts */

static uint8_t *s_hbuf;      /* the note as IMA ADPCM: HERMES_MAX_BYTES of PCM in a quarter of that (PSRAM) */
static muse_adpcm_t s_hnote;  /* its encoder, from the press */

/* ---- Turn: dictation ---- */

static void turn_reset_streams(void)
{
    send_reset(s_turn.dict_id);
    send_reset(s_turn.chat_id);
    for (auto &s : s_streams) {
        if (s.kind == K_TTS) {
            send_reset(s.id);
        }
    }
}

static void hermes_up_close(void);

static void turn_finish(void)
{
    turn_reset_streams();
    hermes_up_close();   /* a note still streaming up */
    if (s_hstream.h) {   /* a Hermes streaming turn in flight: close it mid-reply (the client is kept, s_keep_stream) */
        esp_http_client_close(s_hstream.h);
        s_hstream.h = nullptr;
        heap_caps_free(s_hstream.line);
        s_hstream.line = nullptr;
    }
    s_turn.phase = P_IDLE;
    s_turn.dict_id = s_turn.chat_id = 0;
    s_turn.tts_msg = -1;
    s_turn.silent = false;
    s_turn.mp3_len = 0;
    s_turn.tts_more = false;
}

static void turn_fail(const char *why)
{
    ESP_LOGW(TAG, "turn failed: %s", why);
    if (s_turn.text) {
        muse_hatch_console("error", why, nullptr);
    }
    emit(MUSE_HATCH_EV_ERROR, why);
    turn_finish();
}

/* Ends the turn once its reply is in; `complete` is false when it was cut off. */
static void turn_done(bool complete)
{
    if (s_turn.text) {
        muse_hatch_console("done", nullptr, "\"messages\":%d,\"complete\":%s", s_turn.nmsgs,
                           complete ? "true" : "false");
    }
    emit(MUSE_HATCH_EV_DONE, nullptr);
    turn_finish();
}

/* Ends any turn in progress and starts a fresh one. False if Hatch can't be reached. */
static bool turn_start(uint32_t gen, bool text)
{
    if (s_turn.phase != P_IDLE) {
        if (s_turn.text) {
            turn_fail("INTERRUPTED");
        } else {
            turn_finish();
        }
    }
    uint8_t *chunk = s_turn.chunk, *mp3 = s_turn.mp3, *note = s_turn.note;
    char *texts = s_turn.texts;
    s_turn = turn_t{};
    s_turn.chunk = chunk;
    s_turn.mp3 = mp3;
    s_turn.note = note;
    s_turn.texts = texts;
    s_turn.gen = gen;
    s_turn.text = text;
    s_turn.hermes = hermes_mode();
    s_turn.tts_msg = -1;
    memset(s_marks, 0, sizeof(s_marks));
    s_reply_shown[0] = '\0';
    s_turn.start_us = now_us();
    resampler_init(&s_turn.up, MIC_RATE, DICT_RATE);
    if (!ensure_connected()) {
        turn_fail(muse_hatch_configured() ? "CAN'T REACH MUSE" : "MUSE NOT SET UP");
        return false;
    }
    return true;
}

static void turn_begin(uint32_t gen)
{
    if (!turn_start(gen, false)) {
        return;
    }
    if (s_turn.hermes) {   /* route by the pinned backend; a live mode flip must not reroute this turn */
        if (!s_hbuf) {
            s_hbuf = static_cast<uint8_t *>(psram_alloc(HERMES_MAX_BYTES / 4));
        }
        if (!s_hbuf) {
            turn_fail("OUT OF MEMORY");
            return;
        }
        s_turn.pcm_bytes = 0;
        s_hnote = {};
        s_turn.up_started = false;
        s_turn.phase = P_LISTEN;
        return;
    }
    if (VOICE_NOTE) {
        if (!open_note()) {
            disconnect("chat open failed");
            turn_fail("CAN'T REACH MUSE");
            return;
        }
        s_turn.phase = P_LISTEN;
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), "/api/voice/dictation?sample_rate_hz=%d", DICT_RATE);
    s_turn.dict_id = open_stream(K_DICT, "POST", path, nullptr, "application/x-ndjson", nullptr, false);
    if (!s_turn.dict_id) {
        disconnect("dictation open failed");
        turn_fail("CAN'T REACH MUSE");
        return;
    }
    s_turn.phase = P_LISTEN;
}

/* Moves mic audio to the dictation stream, paced like a live mic. */
static bool pump_mic(void)
{
    static int16_t in[MIC_RATE / 50];
    static int16_t up[MIC_RATE / 50 * DICT_RATE / MIC_RATE + 4];
    bool did = false;
    for (;;) {
        /* Never run more than a second ahead of real time: the ASR upstream drops floods. */
        double elapsed = (now_us() - s_turn.start_us) / 1e6;
        if ((s_turn.sent24 + DICT_CHUNK_BYTES / 2) / (double)DICT_RATE > 1.0 + 1.5 * elapsed) {
            return did;
        }
        size_t got = xStreamBufferReceive(s_in, in, sizeof(in), 0) / sizeof(int16_t);
        if (!got) {
            break;
        }
        did = true;
        size_t n = resample(&s_turn.up, in, got, up);
        const uint8_t *p = reinterpret_cast<const uint8_t *>(up);
        size_t bytes = n * sizeof(int16_t);
        while (bytes) {
            size_t take = DICT_CHUNK_BYTES - s_turn.chunk_len < bytes ? DICT_CHUNK_BYTES - s_turn.chunk_len : bytes;
            memcpy(s_turn.chunk + s_turn.chunk_len, p, take);
            s_turn.chunk_len += take;
            p += take;
            bytes -= take;
            if (s_turn.chunk_len == DICT_CHUNK_BYTES) {
                if (!send_body(s_turn.dict_id, s_turn.chunk, DICT_CHUNK_BYTES, false)) {
                    return false;
                }
                s_turn.sent24 += DICT_CHUNK_BYTES / 2;
                s_turn.chunk_len = 0;
            }
        }
    }
    if (s_turn.end_requested && !s_turn.end_sent) {
        if (s_turn.chunk_len && !send_body(s_turn.dict_id, s_turn.chunk, s_turn.chunk_len, false)) {
            return false;
        }
        s_turn.sent24 += s_turn.chunk_len / 2;
        s_turn.chunk_len = 0;
        if (!send_body(s_turn.dict_id, nullptr, 0, true)) {
            return false;
        }
        s_turn.end_sent = true;
        s_turn.end_sent_us = now_us();
        s_turn.phase = P_WAIT_FINAL;
        ESP_LOGI(TAG, "sent %.2fs of speech", (double)s_turn.sent24 / DICT_RATE);
    }
    return true;
}

/* ---- Turn: voice note ---- */

/* Base64-encodes the staged PCM into one body chunk; `last` pads and closes the request. */
static bool send_note_part(bool last)
{
    char *o = reinterpret_cast<char *>(s_turn.chunk);
    o += muse_hatch_base64(s_turn.note, s_turn.note_len, o);
    if (last) {
        memcpy(o, MUSE_HATCH_NOTE_TAIL, sizeof(MUSE_HATCH_NOTE_TAIL) - 1);
        o += sizeof(MUSE_HATCH_NOTE_TAIL) - 1;
    }
    size_t n = o - reinterpret_cast<char *>(s_turn.chunk);
    s_turn.body_sent += n;
    s_turn.note_len = 0;
    return send_body(s_turn.chat_id, s_turn.chunk, n, last);
}

/*
 * Opens the chat request at the press and streams the note while it's
 * recorded, so only the last chunk goes out after the release. The length
 * isn't known until the end, so the WAV header gives the streaming "unknown"
 * size and the server reads to the end of the data.
 */
static bool open_note(void)
{
    s_turn.chat_id = open_stream(K_CHAT, "POST", "/chat/stream", "application/json", nullptr, nullptr, false);
    if (!s_turn.chat_id) {
        return false;
    }
    s_turn.body_sent = sizeof(MUSE_HATCH_NOTE_HEAD) - 1;
    if (!send_body(s_turn.chat_id, reinterpret_cast<const uint8_t *>(MUSE_HATCH_NOTE_HEAD), sizeof(MUSE_HATCH_NOTE_HEAD) - 1, false)) {
        return false;
    }
    muse_hatch_wav_header(s_turn.note, MIC_RATE);
    s_turn.note_len = MUSE_HATCH_WAV_HEADER;
    return true;
}

/* Moves the mic into the note request; on release, sends the rest and waits for the reply. */
static bool record_note(void)
{
    for (;;) {
        size_t room = NOTE_PART_BYTES - s_turn.note_len;
        size_t left = NOTE_MAX_BYTES - s_turn.pcm_bytes;
        room = (room < left ? room : left) & ~(size_t)1;
        size_t got = room ? xStreamBufferReceive(s_in, s_turn.note + s_turn.note_len, room, 0) : 0;
        if (!got) {
            break;
        }
        s_turn.note_len += got;
        s_turn.pcm_bytes += got;
        if (s_turn.note_len == NOTE_PART_BYTES && !send_note_part(false)) {
            return false;
        }
    }
    if (!s_turn.end_requested && s_turn.pcm_bytes < NOTE_MAX_BYTES) {
        return true;
    }
    mark(M_RELEASE);
    double secs = (double)s_turn.pcm_bytes / (MIC_RATE * 2);
    if (secs < 0.3) {
        turn_fail("DIDN'T CATCH THAT");   /* resets the half-sent request */
        return true;
    }
    if (!send_note_part(true)) {
        return false;
    }
    ESP_LOGI(TAG, "voice note: %.2fs, %u byte request", secs, (unsigned)s_turn.body_sent);
    mark(M_SENT);
    s_turn.chat_posted = true;
    s_turn.chat_us = s_turn.last_event_us = now_us();
    s_turn.phase = P_WAIT_REPLY;
    return true;
}

/*
 * Posts `text` to the chat and waits for the reply. A long message goes up in
 * parts, since each frame has to fit SCRATCH.
 */
static void send_chat(const char *text, const char *modality)
{
    s_turn.chat_posted = true;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "message", text);
    cJSON_AddStringToObject(body, "output_modality", modality);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    size_t len = json ? strlen(json) : 0;
    bool whole = len <= CHAT_PART;
    s_turn.chat_id = json ? open_stream(K_CHAT, "POST", "/chat/stream", "application/json", nullptr,
                                        whole ? json : nullptr, whole) : 0;
    bool ok = s_turn.chat_id != 0;
    for (size_t off = 0; ok && !whole && off < len; off += CHAT_PART) {
        size_t n = len - off < CHAT_PART ? len - off : CHAT_PART;
        ok = send_body(s_turn.chat_id, reinterpret_cast<const uint8_t *>(json) + off, n, off + n == len);
    }
    cJSON_free(json);
    if (!ok) {
        disconnect("chat/stream failed");
        turn_fail("CAN'T REACH MUSE");
        return;
    }
    s_turn.body_sent = len;
    s_turn.chat_us = s_turn.last_event_us = now_us();
    s_turn.phase = P_WAIT_REPLY;
}

static void post_chat(const char *text)
{
    if (s_turn.chat_posted) {
        return;
    }
    s_turn.chat_posted = true;
    send_reset(s_turn.dict_id);
    s_turn.dict_id = 0;
    while (*text == ' ') {
        text++;
    }
    if (!*text) {
        turn_fail("DIDN'T CATCH THAT");
        return;
    }
    ESP_LOGI(TAG, "heard: \"%s\"", text);
    emit(MUSE_HATCH_EV_HEARD, text);
    send_chat(text, "text");
}

/* A typed turn: the text goes straight to the chat. */
static void text_begin(const char *text)
{
    if (!turn_start(0, true)) {
        return;
    }
    ESP_LOGI(TAG, "typed turn: %u bytes", (unsigned)strlen(text));
    send_chat(text, "text");
    if (s_turn.phase == P_WAIT_REPLY) {
        mark(M_SENT);
        muse_hatch_console("sent", nullptr, "\"bytes\":%u", (unsigned)strlen(text));
    }
}

static void on_dictation_line(cJSON *line)
{
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(line, "type"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(line, "text"));
    if (!type) {
        return;
    }
    char heard[1024];
    if (!strcmp(type, "partial") && text) {
        strlcpy(s_turn.partial, text, sizeof(s_turn.partial));
        snprintf(heard, sizeof(heard), "%s%s%s", s_turn.committed, s_turn.committed[0] ? " " : "", text);
        emit(MUSE_HATCH_EV_HEARD, heard);
    } else if (!strcmp(type, "final")) {
        snprintf(heard, sizeof(heard), "%s%s%s", s_turn.committed, s_turn.committed[0] && text ? " " : "",
                 text ? text : "");
        s_turn.partial[0] = '\0';
        if (s_turn.end_sent) {
            post_chat(heard);
        } else {
            /* A mid-utterance segment; the rest follows. */
            strlcpy(s_turn.committed, heard, sizeof(s_turn.committed));
            emit(MUSE_HATCH_EV_HEARD, heard);
        }
    } else if (!strcmp(type, "error")) {
        char *s = cJSON_PrintUnformatted(line);
        ESP_LOGW(TAG, "dictation: %s", s ? s : "error");
        cJSON_free(s);
    }
}

static void on_dictation_end(bool ok)
{
    s_turn.dict_id = 0;
    if (s_turn.chat_posted || s_turn.phase == P_IDLE) {
        return;
    }
    if (!s_turn.end_sent) {
        turn_fail(ok ? "MUSE STOPPED LISTENING" : "MUSE COULDN'T LISTEN");
        return;
    }
    char heard[1024];
    snprintf(heard, sizeof(heard), "%s%s%s", s_turn.committed, s_turn.committed[0] ? " " : "", s_turn.partial);
    post_chat(heard);
}

/* ---- Turn: reply ---- */

static int find_msg(const char *id)
{
    for (int i = 0; i < s_turn.nmsgs; i++) {
        if (!strcmp(s_turn.msgs[i].id, id)) {
            return i;
        }
    }
    return -1;
}

static bool is_user_id(const char *id)
{
    return id && id[0] && (!strcmp(id, s_turn.user_ids[0]) || !strcmp(id, s_turn.user_ids[1]));
}

/* The message `id` if it belongs to this turn, binding it on first sight; else -1. */
static int bind_msg(const char *id, cJSON *payload)
{
    if (muse_chat_is_rejected(&s_turn.rejected, id)) {
        return -1;
    }
    int i = find_msg(id);
    if (i >= 0 || s_turn.phase != P_WAIT_REPLY) {
        return i;
    }
    const char *parent = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "reply_to_message_id"));
    if (!parent) {
        parent = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "parent_message_id"));
    }
    /* Once the ack names our message, replies to anything else are someone else's. */
    if (parent && parent[0] && s_turn.acked && !is_user_id(parent) && find_msg(parent) < 0) {
        muse_chat_reject(&s_turn.rejected, id);
        return -1;
    }
    if ((!parent || !parent[0]) && s_turn.rejected.overflow) {
        return -1;
    }
    if (s_turn.nmsgs == MAX_MSGS) {
        return -1;
    }
    msg_t &m = s_turn.msgs[s_turn.nmsgs];
    m = msg_t{};
    strlcpy(m.id, id, sizeof(m.id));
    return s_turn.nmsgs++;
}

static void append_text(msg_t &m, const char *text)
{
    size_t add = strlen(text);
    if (s_turn.texts) {
        char *full = s_turn.texts + (&m - s_turn.msgs) * TEXT_MAX;
        if (!m.len) {
            full[0] = '\0';
        }
        strlcat(full, text, TEXT_MAX);
    }
    m.len += add;
    size_t have = strlen(m.tail);
    if (add >= sizeof(m.tail) - 1) {
        strlcpy(m.tail, text + add - (sizeof(m.tail) - 1), sizeof(m.tail));
        return;
    }
    if (have + add >= sizeof(m.tail)) {
        size_t drop = have + add - (sizeof(m.tail) - 1);
        memmove(m.tail, m.tail + drop, have - drop + 1);
    }
    strlcat(m.tail, text, sizeof(m.tail));
}

/*
 * Shows a reply that hasn't started speaking: its opening lines, which the
 * speech starts with, so they can be read while the audio is on its way.
 * The spoken captions carry on from there.
 */
static void show_reply_start(const msg_t &m)
{
    char line[EV_TEXT];
    if (s_turn.texts) {
        if (!muse_hatch_caption_at(s_turn.texts + (&m - s_turn.msgs) * TEXT_MAX, 0, line, sizeof(line))) {
            return;
        }
    } else {
        muse_hatch_tail_words(m.tail, line, sizeof(line));
    }
    if (strcmp(line, s_reply_shown) != 0) {
        strlcpy(s_reply_shown, line, sizeof(s_reply_shown));
        emit(MUSE_HATCH_EV_REPLY, line);
    }
}

static void message_done(int i, const char *final_text)
{
    msg_t &m = s_turn.msgs[i];
    if (m.done) {
        return;
    }
    m.done = true;
    mark(M_DONE);
    if (s_turn.text) {
        /* The whole text if the pieces didn't add up to it (a line skipped, say): the reader uses it instead. */
        size_t n = m.len;
        if (final_text && final_text[0] && strlen(final_text) != m.len) {
            n = strlen(final_text);
            muse_hatch_console("final", final_text, "\"msg\":%d", i);
        }
        muse_hatch_console("message_done", nullptr, "\"msg\":%d,\"bytes\":%u", i, (unsigned)n);
        ESP_LOGI(TAG, "message %s done (%u chars)", m.id, (unsigned)n);
        return;
    }
    if (!m.len && final_text && final_text[0]) {
        append_text(m, final_text);
    }
    if (m.len && s_turn.texts && !s_turn.text) {
        m.store_want = true;   /* the reading view's add runs in start_tts, same task */
    }
    if (m.len && m.tts == TTS_NONE) {
        m.tts = TTS_QUEUED;
    }
    ESP_LOGI(TAG, "message %s done (%u chars)", m.id, (unsigned)m.len);
}

static const char *msg_id(cJSON *payload, cJSON *event)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "message_id"));
    if (!id || !id[0]) {
        id = cJSON_GetStringValue(cJSON_GetObjectItem(event, "message_id"));
    }
    if (!id || !id[0]) {
        id = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "id"));
    }
    return id && id[0] ? id : nullptr;
}

static void on_event(cJSON *line)
{
    if (strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(line, "type")) ?: "", "event") != 0) {
        return;   /* the subscription ack */
    }
    cJSON *seq = cJSON_GetObjectItem(line, "seq");
    if (cJSON_IsNumber(seq)) {
        int64_t v = (int64_t)seq->valuedouble;
        if (v > 0 && v <= s_last_seq) {
            return;
        }
        s_last_seq = v > s_last_seq ? v : s_last_seq;
    }
    if (s_turn.phase != P_WAIT_REPLY) {
        return;
    }
    const char *event = cJSON_GetStringValue(cJSON_GetObjectItem(line, "event")) ?: "";
    cJSON *payload = cJSON_GetObjectItem(line, "payload");

    if (!strcmp(event, "agent.status")) {
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "activity_code"));
        const char *status = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "status"));
        bool was = s_turn.agent_busy;
        if (code) {
            s_turn.agent_busy = code[0] && strcmp(code, "online") && strcmp(code, "idle");
        } else if (status) {
            s_turn.agent_busy = status[0] && strcmp(status, "completed") && strcmp(status, "failed");
        }
        if (s_turn.text && s_turn.agent_busy != was) {
            muse_hatch_console("busy", nullptr, "\"on\":%s", s_turn.agent_busy ? "true" : "false");
        }
        s_turn.last_event_us = now_us();
        return;
    }
    bool start = !strcmp(event, "delta.message_start");
    bool append = !strcmp(event, "delta.text_append");
    bool done = !strcmp(event, "delta.message_done");
    bool full = !strcmp(event, "message.assistant");
    if (!start && !append && !done && !full) {
        return;
    }
    const char *id = msg_id(payload, line);
    int i = id ? bind_msg(id, payload) : -1;
    if (i < 0) {
        return;
    }
    s_turn.last_event_us = s_turn.last_content_us = now_us();
    msg_t &m = s_turn.msgs[i];
    if (append) {
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "text"));
        if (text && text[0] && s_turn.text) {
            mark(M_TEXT);
            m.len += strlen(text);
            muse_hatch_console("text", text, "\"msg\":%d", i);
        } else if (text && text[0]) {
            mark(M_TEXT);
            append_text(m, text);
            show_reply_start(m);   /* ignored once the speech starts */
            /* Enough text for a first piece: queue it now so the speech fetch runs
             * while the rest of the message is still streaming in. */
            if (m.tts == TTS_NONE && m.len >= TTS_CHUNK_MIN) {
                m.tts = TTS_QUEUED;
            }
        }
    } else if (done || full) {
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "display_text"));
        if (!text) {
            text = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "content"));
        }
        cJSON *ready = cJSON_GetObjectItem(payload, "display_text_ready");
        if (done || !cJSON_IsFalse(ready)) {
            message_done(i, text);
        }
    }
}

static void on_chat_ack(stream_t *s)
{
    s->line[s->len] = '\0';
    cJSON *root = cJSON_Parse(s->line);
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *obj = cJSON_IsObject(result) ? result : root;
    const char *keys[] = { "message_id", "reply_to_message_id" };
    for (int k = 0; k < 2; k++) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(obj, keys[k]));
        if (id) {
            strlcpy(s_turn.user_ids[k], id, sizeof(s_turn.user_ids[k]));
        }
    }
    s_turn.acked = true;
    mark(M_ACK);
    ESP_LOGI(TAG, "chat/stream ack: user message %s", s_turn.user_ids[0]);
    cJSON_Delete(root);
    emit(MUSE_HATCH_EV_SENT, nullptr);
}

/* ---- Turn: speech ---- */

static void tts_data(const uint8_t *data, size_t len);

/* ---- Spoken replies: Google Cloud Text-to-Speech (REST, MP3 in base64 JSON) ---- */

static const char TTS_ROOTS[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYDVQQGEwJVUzEi\n"
    "MCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQw\n"
    "HhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZ\n"
    "R29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjO\n"
    "PQIBBgUrgQQAIgNiAATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzu\n"
    "hXyiQHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvRHYqjQjBA\n"
    "MA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBSATNbrdP9JNqPV2Py1\n"
    "PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/C\n"
    "r8deVl5c1RxYIigL9zC2L7F8AjEA8GE8p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh\n"
    "4rsUecrNIdSUtUlD\n"
    "-----END CERTIFICATE-----\n";


struct tts_buf_t {
    char *p;
    size_t len, cap;
    bool over;
};

static int s_http_connects;   /* new connections made by the clients below (their TLS handshakes) */

static esp_err_t tts_http_evt(esp_http_client_event_t *e)
{
    s_http_connects += e->event_id == HTTP_EVENT_ON_CONNECTED;
    auto *b = static_cast<tts_buf_t *>(e->user_data);
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        if (b->len + e->data_len < b->cap) {
            memcpy(b->p + b->len, e->data, e->data_len);
            b->len += e->data_len;
        } else {
            b->over = true;
        }
    }
    return ESP_OK;
}

/* Decodes base64 in place (output never outruns input); returns the byte count. */
static size_t b64_decode_inplace(char *s, size_t n)
{
    static int8_t T[256];
    if (!T[(int)'B']) {
        memset(T, -1, sizeof(T));
        const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) {
            T[(uint8_t)a[i]] = (int8_t)i;
        }
    }
    uint8_t *out = reinterpret_cast<uint8_t *>(s);
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = T[(uint8_t)s[i]];
        if (v < 0) {
            continue;   /* '=' padding, stray whitespace */
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o;
}

/* The relay's funnel has a Let's Encrypt ECDSA chain. Checked from the bundle's ISRG Root X2 it
 * takes three P-384 signature checks in software, 3-4 s of every fresh handshake (the Mac does it
 * in 0.3 s). Trusting the funnel's issuer (YE1, to 2028) and its root (Root YE, to 2032) directly
 * stops after one or two. A certificate from anywhere else is refused, and from then on (this
 * boot) the relay is checked against the full bundle again. Public certificates, not secrets. */
static const char RELAY_CA_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICizCCAhGgAwIBAgIQXd1w3TH4AchcGGp6BLgK/jAKBggqhkjOPQQDAzAuMQsw\n"
    "CQYDVQQGEwJVUzENMAsGA1UEChMESVNSRzEQMA4GA1UEAxMHUm9vdCBZRTAeFw0y\n"
    "NTA5MDMwMDAwMDBaFw0yODA5MDIyMzU5NTlaMDMxCzAJBgNVBAYTAlVTMRYwFAYD\n"
    "VQQKEw1MZXQncyBFbmNyeXB0MQwwCgYDVQQDEwNZRTEwdjAQBgcqhkjOPQIBBgUr\n"
    "gQQAIgNiAAQHZVB1/mimla2hfSurylScjPMZaOJXLz/NnAc2sylm8WDyhU9Ccp+z\n"
    "ASQi5vSwGGJjSGklkD9fdPR8GpyDIOIjCEfrnbt/v+ZSEPLLEGbaM6EccDbN7p9x\n"
    "teIm2Avf+ryjge4wgeswDgYDVR0PAQH/BAQDAgGGMBMGA1UdJQQMMAoGCCsGAQUF\n"
    "BwMBMBIGA1UdEwEB/wQIMAYBAf8CAQAwHQYDVR0OBBYEFLsgykcL/tflnPmPCSqj\n"
    "jDdFsbzYMB8GA1UdIwQYMBaAFKPIJlqOoUzQNWP8myPIOq5W809WMDIGCCsGAQUF\n"
    "BwEBBCYwJDAiBggrBgEFBQcwAoYWaHR0cDovL3llLmkubGVuY3Iub3JnLzATBgNV\n"
    "HSAEDDAKMAgGBmeBDAECATAnBgNVHR8EIDAeMBygGqAYhhZodHRwOi8veWUuYy5s\n"
    "ZW5jci5vcmcvMAoGCCqGSM49BAMDA2gAMGUCMQDgjUEahFT/h3DRakqiPZpLvPgf\n"
    "Zwkt6K2EOMmh1nvEzl83eMLYcod4GCl3b0J1Nn0CMBNYmEQJb4CEG5WoOe7aRn/L\n"
    "VKu6saHmHEynI7ysIPd8zQsK1HdmhlHKlw9Z5GpGvA==\n"
    "-----END CERTIFICATE-----\n"
    " 2 s:C=US, O=ISRG, CN=Root YE\n"
    "   i:C=US, O=Internet Security Research Group, CN=ISRG Root X2\n"
    "   a:PKEY: EC, (secp384r1); sigalg: ecdsa-with-SHA384\n"
    "   v:NotBefore: May 13 00:00:00 2026 GMT; NotAfter: Sep  2 23:59:59 2032 GMT\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIICpjCCAiugAwIBAgIRAIchZfw0tuX7qK3Vs3BftTowCgYIKoZIzj0EAwMwTzEL\n"
    "MAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2VhcmNo\n"
    "IEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDIwHhcNMjYwNTEzMDAwMDAwWhcN\n"
    "MzIwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzENMAsGA1UEChMESVNSRzEQMA4G\n"
    "A1UEAxMHUm9vdCBZRTB2MBAGByqGSM49AgEGBSuBBAAiA2IABDwS/6vhrcVqcbBo\n"
    "+wgdI3fwn9x7DNJJOY/lTOti0vkwuRN87RhEhTH17E7XyFjWsPYhIPt/wzOqxTd2\n"
    "b+4ZJNy9ID04YywF9U5zasDVyGSNErVNtz8uSGh5izW87j77GaOB6zCB6DAOBgNV\n"
    "HQ8BAf8EBAMCAQYwEwYDVR0lBAwwCgYIKwYBBQUHAwEwDwYDVR0TAQH/BAUwAwEB\n"
    "/zAdBgNVHQ4EFgQUo8gmWo6hTNA1Y/ybI8g6rlbzT1YwHwYDVR0jBBgwFoAUfEKW\n"
    "rt5LSDv6kviejM9ti6lyN5UwMgYIKwYBBQUHAQEEJjAkMCIGCCsGAQUFBzAChhZo\n"
    "dHRwOi8veDIuaS5sZW5jci5vcmcvMBMGA1UdIAQMMAowCAYGZ4EMAQIBMCcGA1Ud\n"
    "HwQgMB4wHKAaoBiGFmh0dHA6Ly94Mi5jLmxlbmNyLm9yZy8wCgYIKoZIzj0EAwMD\n"
    "aQAwZgIxAMU19WCtmxVND8UHBZRoma49Z7jPs64Dma0eTu1OChVbB/2J7GV3nvYK\n"
    "Ax54uk1G9QIxAO0miLVJu8PLNiXXXkiE/gsK3CTRTF/aeo4bMX42Zw40csRU6AC2\n"
    "6hSW1/IWaas6dg==\n"
    "-----END CERTIFICATE-----\n";
static bool s_relay_ca_off;

static void relay_tls(esp_http_client_config_t *cfg)
{
    cfg->cert_pem = s_relay_ca_off ? nullptr : RELAY_CA_PEM;
    cfg->crt_bundle_attach = s_relay_ca_off ? esp_crt_bundle_attach : nullptr;
}

/* After a failed call: true if it was the pinned issuers that refused the relay's certificate,
 * with cfg switched to the full bundle for a retry. */
static bool relay_tls_refused(esp_http_client_handle_t h, esp_http_client_config_t *cfg)
{
    int code = 0, flags = 0;
    esp_http_client_get_and_clear_last_tls_error(h, &code, &flags);
    if (!cfg->cert_pem || !flags) {
        return false;
    }
    ESP_LOGW(TAG, "relay certificate isn't from Let's Encrypt YE (flags 0x%x): full bundle from now on", flags);
    s_relay_ca_off = true;
    relay_tls(cfg);
    return true;
}

/* Fetches speech for text and queues its MP3 for decode(). False leaves nothing queued. */
/* Kept-alive HTTPS for the calls that repeat (speech pieces, the relay's polls). Each used to open
 * a fresh TLS connection, and the handshake's software crypto starved the display for 0.5-2.4 s
 * at a time through the cache both cores share. A kept client stays connected between calls; if
 * the far end has closed it, the call is made once more on a fresh one, whose saved session
 * ticket makes that handshake short. Session task only, one call at a time per slot. */
struct keep_t {
    esp_http_client_handle_t h;
    char base[72];   /* scheme://host[:port] it is connected to */
};

static void keep_drop(keep_t *k)
{
    if (k->h) {
        esp_http_client_cleanup(k->h);
        k->h = nullptr;
    }
}

static esp_http_client_handle_t keep_get(keep_t *k, esp_http_client_config_t *cfg)
{
    const char *u = cfg->url, *sch = strstr(u, "://"), *path = sch ? strchr(sch + 3, '/') : nullptr;
    size_t blen = path ? (size_t)(path - u) : strlen(u);
    if (k->h && (blen >= sizeof(k->base) || strncmp(k->base, u, blen) != 0 || k->base[blen])) {
        keep_drop(k);   /* another server (the relay's LAN door or its funnel): start over */
    }
    if (k->h) {
        esp_http_client_set_url(k->h, cfg->url);
        esp_http_client_set_method(k->h, cfg->method);
        esp_http_client_set_timeout_ms(k->h, cfg->timeout_ms);
        esp_http_client_set_user_data(k->h, cfg->user_data);
        esp_http_client_set_post_field(k->h, nullptr, 0);
        return k->h;
    }
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    cfg->save_client_session = true;
#endif
    k->h = esp_http_client_init(cfg);
    if (k->h) {
        size_t n = blen < sizeof(k->base) - 1 ? blen : sizeof(k->base) - 1;
        memcpy(k->base, u, n);
        k->base[n] = 0;
    }
    return k->h;
}

/* One call on the slot's client: prep(h) sets its headers and body and resets the reply buffer
 * (it runs again for the retry). *status is the HTTP status, 0 when the call failed. */
template <typename Prep>
static esp_err_t keep_perform(keep_t *k, esp_http_client_config_t *cfg, Prep prep, int *status)
{
    *status = 0;
    for (int attempt = 0; attempt < 3; attempt++) {   /* stale socket, then refused pin */
        bool reused = k->h != nullptr;
        esp_http_client_handle_t h = keep_get(k, cfg);
        if (!h) {
            return ESP_ERR_NO_MEM;
        }
        prep(h);
        int connects = s_http_connects;
        int64_t t0 = now_us();
        esp_err_t err = esp_http_client_perform(h);
        if (s_http_connects != connects) {
            ESP_LOGI(TAG, "new connection to %s (%lld ms with the call)", k->base, (long long)((now_us() - t0) / 1000));
        }
        if (err == ESP_OK) {
            *status = esp_http_client_get_status_code(h);
            return err;
        }
        bool refused = relay_tls_refused(h, cfg);
        keep_drop(k);
        if (refused) {
            continue;
        }
        if (!reused) {
            return err;   /* a fresh connection failed: the server, not a stale socket */
        }
    }
    return ESP_FAIL;
}

static keep_t s_keep_tts;     /* texttospeech.googleapis.com */
static keep_t s_keep_relay;   /* the relay's GETs: notifications, their history, the glance cards */
static keep_t s_keep_stream;  /* the turn's client; each turn connects afresh (esp_http_client_open always does) */

/* Chirp 3 HD pacing and pronunciation (docs.cloud.google.com/text-to-speech/docs/chirp3-hd):
 * pauses come from `[pause]` / `[pause short]` in the markup input, names from an IPA table
 * beside it. Every phrase must be unique after case-folding; the API 400s duplicates. */
static const char TTS_PRON_JSON[] =
    ",\"customPronunciations\":{\"pronunciations\":["
    "{\"phrase\":\"BEEPBOP\",\"phoneticEncoding\":\"PHONETIC_ENCODING_IPA\",\"pronunciation\":\"ˈbiːpbɒp\"},"
    "{\"phrase\":\"Muse\",\"phoneticEncoding\":\"PHONETIC_ENCODING_IPA\",\"pronunciation\":\"mjuːz\"},"
    "{\"phrase\":\"Hermes\",\"phoneticEncoding\":\"PHONETIC_ENCODING_IPA\",\"pronunciation\":\"ˈhɜːrmiːz\"},"
    "{\"phrase\":\"Oleg\",\"phoneticEncoding\":\"PHONETIC_ENCODING_IPA\",\"pronunciation\":\"oʊˈlɛɡ\"},"
    "{\"phrase\":\"Zharsky\",\"phoneticEncoding\":\"PHONETIC_ENCODING_IPA\",\"pronunciation\":\"ˈʒɑːrski\"},"
    "{\"phrase\":\"Etsy\",\"phoneticEncoding\":\"PHONETIC_ENCODING_IPA\",\"pronunciation\":\"ˈɛtsi\"},"
    "{\"phrase\":\"Kalshi\",\"phoneticEncoding\":\"PHONETIC_ENCODING_IPA\",\"pronunciation\":\"ˈkælʃi\"},"
    "{\"phrase\":\"SHODAN\",\"phoneticEncoding\":\"PHONETIC_ENCODING_IPA\",\"pronunciation\":\"ˈʃoʊdæn\"},"
    "{\"phrase\":\"Ollama\",\"phoneticEncoding\":\"PHONETIC_ENCODING_IPA\",\"pronunciation\":\"oʊˈlɑːmə\"},"
    "{\"phrase\":\"Gemini\",\"phoneticEncoding\":\"PHONETIC_ENCODING_IPA\",\"pronunciation\":\"ˈdʒɛmɪnaɪ\"}"
    "]}";

static const char *tts_pause_tag(const char *text)
{
    size_t n = strlen(text);
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\n')) {
        n--;
    }
    if (n >= 3 && !strncmp(text + n - 3, "...", 3)) {
        return " [pause]";
    }
    if (n && (text[n - 1] == '.' || text[n - 1] == '!' || text[n - 1] == '?')) {
        return " [pause short]";
    }
    return "";
}

/* The request body; `fancy` adds the pause tag and the pronunciation table. Returns its length. */
static size_t tts_body(char *body, size_t cap, const char *text, const char *voice, int lang_len,
                       double rate, bool fancy)
{
    size_t n = snprintf(body, cap, "{\"input\":{\"%s\":\"", fancy ? "markup" : "text");
    for (const char *c = text; *c && n + 8 < cap; c++) {
        if (*c == '"' || *c == '\\') {
            body[n++] = '\\';
            body[n++] = *c;
        } else if (*c == '*' || *c == '#') {
            continue;   /* markdown: not worth saying */
        } else if ((uint8_t)*c < 0x20) {
            body[n++] = ' ';
        } else {
            body[n++] = *c;
        }
    }
    /* the pause tag belongs INSIDE the string: "text [pause short]" */
    snprintf(body + n, cap - n, "%s\"%s},\"voice\":{\"languageCode\":\"%.*s\",\"name\":\"%s\"},"
             "\"audioConfig\":{\"audioEncoding\":\"MP3\",\"speakingRate\":%.2f}}",
             fancy ? tts_pause_tag(text) : "", fancy ? TTS_PRON_JSON : "", lang_len, voice, voice, rate);
    return strlen(body);
}

static uint32_t tts_billed_chars(const char *text)
{
    uint32_t n = 0;
    for (const char *c = text; *c; c++) {
        if (*c != '*' && *c != '#' && ((uint8_t)*c & 0xC0) != 0x80) {
            n++;   /* a character, not each of its UTF-8 bytes */
        }
    }
    return n;
}

static bool fetch_tts(const char *text, bool last = true, bool first = true)
{
    const size_t CAP = 192 * 1024;   /* one 90-char piece is ~66 KB of JSON; 900 KB was the whole-message era */
    char *body = static_cast<char *>(psram_alloc(TEXT_MAX * 2 + 1400));
    char *resp = static_cast<char *>(psram_alloc(CAP));
    bool ok = false;
    uint32_t billed = 0;   /* characters actually sent, as Google counts them */
    /* The language is the voice name's first two parts: en-US-Chirp3-HD-Aoede -> en-US. */
    int lang_len = 0;
    const char *voice = hermes_mode() ? CONFIG_MUSE_HERMES_TTS_VOICE : CONFIG_MUSE_TTS_GOOGLE_VOICE;
    for (int dashes = 0; voice[lang_len]; lang_len++) {
        if (voice[lang_len] == '-' && ++dashes == 2) {
            break;
        }
    }
    if (!body || !resp) {
        goto out;
    }
    {
        size_t cap = TEXT_MAX * 2 + 1400;
        const double rate = hermes_mode() ? 1.0 : CONFIG_MUSE_TTS_RATE_PCT / 100.0;
        billed = tts_billed_chars(text);
        static const char URL[] = "https://texttospeech.googleapis.com/v1/text:synthesize?key=" CONFIG_MUSE_TTS_GOOGLE_KEY;
        tts_buf_t buf = { resp, 0, CAP, false };
        esp_http_client_config_t cfg = {};
        cfg.url = URL;
        cfg.method = HTTP_METHOD_POST;
        /* Pinned, not esp_crt_bundle: the bundle parses ~150 KB of certs and this board
         * has ~27 KB internal free. Pin the root Google chains to NOW -- GTS Root R4
         * (the old GTS Root R1 pin broke when Google rotated, -0x2700). */
        cfg.cert_pem = TTS_ROOTS;
        cfg.timeout_ms = 8000;   /* this task is blocked meanwhile; if Google is slow, show the text rather than wait */
        cfg.event_handler = tts_http_evt;
        cfg.user_data = &buf;
        int64_t t0 = now_us();
        int status = 0;
        esp_err_t err = ESP_FAIL;
        for (int attempt = 0; attempt < 2; attempt++) {
            bool fancy = attempt == 0;   /* a voice/locale that rejects markup or the table gets plain text */
            tts_body(body, cap, text, voice, lang_len, rate, fancy);
            err = keep_perform(&s_keep_tts, &cfg, [&](esp_http_client_handle_t h) {
                buf.len = 0;
                buf.over = false;
                esp_http_client_set_header(h, "Content-Type", "application/json");
                esp_http_client_set_post_field(h, body, strlen(body));
            }, &status);
            if (err == ESP_OK && status == 200 && !buf.over) {
                break;
            }
            ESP_LOGW(TAG, "tts: %s request failed (err %s, HTTP %d%s)", fancy ? "markup" : "plain",
                     esp_err_to_name(err), status, buf.over ? ", too big" : "");
            if (fancy && status != 200 && buf.len) {
                buf.p[buf.len < 200 ? buf.len : 199] = 0;   /* Google's error text */
                ESP_LOGW(TAG, "tts: %s", buf.p);
            }
        }
        if (err != ESP_OK || status != 200 || buf.over) {
            goto out;
        }
        buf.p[buf.len] = 0;
        char *a = strstr(buf.p, "\"audioContent\"");
        a = a ? strchr(a + 14, '"') : nullptr;
        char *end = a ? strchr(a + 1, '"') : nullptr;
        if (!end) {
            ESP_LOGW(TAG, "tts: no audioContent in reply");
            goto out;
        }
        size_t mp3 = b64_decode_inplace(a + 1, (size_t)(end - (a + 1)));
        if (first) {
            s_turn.mp3_len = 0;   /* a fresh message: drop the previous message's tail */
        }
        s_turn.mp3_ended = false;
        tts_data(reinterpret_cast<uint8_t *>(a + 1), mp3);
        s_turn.mp3_ended = last;   /* chunked replies: only the LAST piece ends the message */
        muse_settings_tts_add(billed);   /* what Google counts against the free allowance */
        ESP_LOGI(TAG, "tts: %u chars -> %u bytes MP3 in %lld ms", (unsigned)strlen(text), (unsigned)mp3, (long long)((now_us() - t0) / 1000));
        ok = true;
    }
out:
    heap_caps_free(body);
    heap_caps_free(resp);
    return ok;
}


/* The last reply, whole, for the reading view (a long press on the avatar) — one per agent:
 * the show picks the active agent's own store, so switching agents never shows the other's text. */
#define LAST_REPLY_MAX 3072
static EXT_RAM_BSS_ATTR char s_last_reply[2][LAST_REPLY_MAX];

static int last_reply_idx(void)
{
    return muse_settings_relay_configured() && muse_settings_agent_hermes() ? 1 : 0;
}

extern "C" const char *muse_hatch_last_reply(void)
{
    return s_last_reply[last_reply_idx()];
}

static void last_reply_add(int msg_index, const char *text)
{
    char *store = s_last_reply[last_reply_idx()];
    if (msg_index == 0) {
        store[0] = 0;
    }
    if (store[0]) {
        strlcat(store, "\n\n", LAST_REPLY_MAX);
    }
    strlcat(store, text, LAST_REPLY_MAX);
}

/* Next slice of a message's text for one TTS fetch: prefers a sentence end, never
 * shorter than TTS_CHUNK_MIN (a piece only pays off if it out-plays its own fetch),
 * never longer than TTS_CHUNK_MAX, falling back to the last space in the window.
 * Mid-stream (`done` false) only a word boundary counts -- a message's tail is
 * fetched once it actually ends. 0 = nothing fetchable yet. */
static size_t tts_chunk_len(const char *t, size_t from, size_t len, bool done)
{
    size_t last_space = 0;
    for (size_t k = from; k < len; k++) {
        char c = t[k];
        if ((c == '.' || c == '!' || c == '?') && k + 1 - from >= TTS_CHUNK_MIN) {
            return k + 1 - from;
        }
        if (c == ' ' || c == '\n' || c == '\t') {
            last_space = k + 1;
        }
        if (k + 1 - from >= TTS_CHUNK_MAX) {
            if (last_space - from >= TTS_CHUNK_MIN) {
                return last_space - from;
            }
            return done ? TTS_CHUNK_MAX : 0;
        }
    }
    if (done) {
        return len > from ? len - from : 0;
    }
    return last_space - from >= TTS_CHUNK_MIN ? last_space - from : 0;
}

/* True if a slice has anything to say: a letter or digit (or any non-ASCII byte -- UTF-8 text). */
static bool speakable(const char *t, size_t n)
{
    for (size_t k = 0; k < n; k++) {
        unsigned char c = (unsigned char)t[k];
        if (isalnum(c) || c >= 0x80) {
            return true;
        }
    }
    return false;
}

/* fetch_tts on one NUL-free slice of the texts buffer. */
static bool fetch_tts_range(char *t, size_t from, size_t cl, bool last, bool first)
{
    char saved = t[from + cl];
    t[from + cl] = 0;
    bool ok = fetch_tts(t + from, last, first);
    t[from + cl] = saved;
    return ok;
}

/* Muse path, every pump pass: while the speaking message still has text to fetch,
 * pull the next piece once decode() has drained the previous one. The voice task
 * keeps playing from the 2 s reply buffer during the fetch, so speech is continuous
 * as long as each piece out-plays its own fetch (that is the chunk sizing above). */
static void tts_refill(void)
{
    if (!s_turn.tts_more || s_turn.silent || !s_turn.texts) {
        return;
    }
    if (s_turn.mp3_len > MP3_HOLD) {
        return;   /* decode still has frames for the speaker */
    }
    int i = s_turn.tts_msg;
    if (i < 0 || i >= s_turn.nmsgs) {
        s_turn.tts_more = false;
        return;
    }
    msg_t &m = s_turn.msgs[i];
    char *t = s_turn.texts + i * TEXT_MAX;
    size_t tlen = strlen(t);
    size_t cl = tts_chunk_len(t, s_turn.tts_off, tlen, m.done);
    if (!cl) {
        if (m.done) {
            s_turn.tts_more = false;
            s_turn.mp3_ended = true;
        }
        return;   /* mid-word tail while the text is still growing: wait */
    }
    bool last = m.done && s_turn.tts_off + cl >= tlen;
    /* A piece with nothing to say (a trailing newline or space after the last sentence) is
     * skipped: Google answers it with 400 "does not support empty input", and treating that as
     * a failure used to end the speech mid-reply. Any other failed piece is skipped too, not the
     * rest of the reply. */
    if (!speakable(t + s_turn.tts_off, cl)) {
        s_turn.tts_off += cl;
        s_turn.tts_more = !last;
        if (last) {
            s_turn.mp3_ended = true;
        }
        return;
    }
    if (!fetch_tts_range(t, s_turn.tts_off, cl, last, false)) {
        ESP_LOGW(TAG, "tts: a piece failed (%u chars); skipping to the next", (unsigned)cl);
        if (++s_turn.tts_fails >= 3 || last) {   /* the service is down, or nothing follows */
            if (!last) {
                ESP_LOGW(TAG, "tts: three pieces failed; the speech ends early");
            }
            s_turn.tts_more = false;
            s_turn.mp3_ended = true;
            return;
        }
    }
    s_turn.tts_off += cl;
    s_turn.tts_more = !last;
}

static void start_tts(void)
{
    for (int i = 0; i < s_turn.nmsgs; i++) {
        msg_t &m = s_turn.msgs[i];
        if (m.store_want && s_turn.texts) {
            m.store_want = false;
            if (!hermes_mode()) {   /* Hermes filled its own store as the reply arrived */
                last_reply_add(i, s_turn.texts + i * TEXT_MAX);
            }
        }
    }
    if (s_turn.tts_msg >= 0) {
        tts_refill();
        return;
    }
    for (int i = 0; i < s_turn.nmsgs; i++) {
        msg_t &m = s_turn.msgs[i];
        if (m.tts != TTS_QUEUED) {
            continue;
        }
        /*
         * Replies are text, shown at reading pace: silence in place of speech
         * paces the captions and ends the turn. To speak them instead, send
         * the message's text (s_turn.texts + i * TEXT_MAX, if texts was
         * allocated; up to TEXT_MAX - 1 bytes) to a TTS API of your choice and
         * play the MP3 it returns. In place of the silence below: keep
         * m.tts = TTS_ACTIVE and s_turn.tts_msg = i, set s_turn.silent = false,
         * m.pcm_start = s_turn.pcm_out, m.pcm_frames = 0, s_turn.mp3_len = 0,
         * s_turn.mp3_ended = false, s_turn.kbps = 0, s_turn.down_rate = 0 and
         * mp3dec_init(&s_turn.dec). Then, on this task, pass the MP3 to
         * tts_data() as it arrives (it buffers up to MP3_BUF and drops the
         * rest, so hold off while it's full) and set s_turn.mp3_ended at the
         * end. decode() plays it at the speaker's volume, captions following,
         * and finishes the message once it's drained.
         */
        if (CONFIG_MUSE_REPLY_MAX_CHARS > 0 && s_turn.texts && m.len > CONFIG_MUSE_REPLY_MAX_CHARS) {
            /* Keep to the last sentence end within the limit, else the last word. */
            char *t = s_turn.texts + i * TEXT_MAX;
            int cut = -1, space = -1;
            for (int k = 0; k < CONFIG_MUSE_REPLY_MAX_CHARS; k++) {
                if ((t[k] == '.' || t[k] == '!' || t[k] == '?') && (t[k + 1] == ' ' || t[k + 1] == '\n')) {
                    cut = k + 1;
                } else if (t[k] == ' ') {
                    space = k;
                }
            }
            cut = cut > CONFIG_MUSE_REPLY_MAX_CHARS / 3 ? cut : space > 0 ? space : CONFIG_MUSE_REPLY_MAX_CHARS;
            t[cut] = 0;
            m.len = (uint32_t)cut;
        }
        /* First sentence-sized piece. An early-queued message (text still streaming)
         * whose text has no fetchable boundary yet stays QUEUED for a later pass. */
        size_t tlen = s_turn.texts ? strlen(s_turn.texts + i * TEXT_MAX) : 0;
        bool will_fetch = CONFIG_MUSE_TTS_GOOGLE_KEY[0] && s_turn.texts && muse_settings_speaker_on();
        size_t cl = will_fetch ? tts_chunk_len(s_turn.texts + i * TEXT_MAX, 0, tlen, m.done) : 0;
        if (!cl && !m.done) {
            continue;
        }
        m.pcm_start = s_turn.pcm_out;
        m.pcm_frames = (uint32_t)(m.len * MIC_RATE / TEXT_CHARS_PER_S);
        m.tts = TTS_ACTIVE;
        s_turn.tts_msg = i;
        s_turn.silent = true;
        s_turn.tts_off = 0;
        s_turn.tts_more = false;
        s_turn.tts_fails = 0;
        if (will_fetch && cl &&
            fetch_tts_range(s_turn.texts + i * TEXT_MAX, 0, cl, m.done && cl >= tlen, true)) {
            /* First piece fetched: decode() starts the speech, tts_refill() fetches
             * the rest while it plays. */
            s_turn.tts_off = cl;
            s_turn.tts_more = !(m.done && cl >= tlen);
            s_turn.silent = false;
            m.pcm_frames = 0;
            s_turn.kbps = 0;
            s_turn.down_rate = 0;
            mp3dec_init(&s_turn.dec);
        }
        if (s_hreact[0]) {
            muse_ui_react(s_hreact);   /* now, not when the reply arrived: the speech fetch above takes seconds */
            s_hreact[0] = '\0';
        }
        ESP_LOGI(TAG, "showing message %s (%u chars)", m.id, (unsigned)m.len);
        show_reply_start(m);
        return;
    }
}

static void tts_data(const uint8_t *data, size_t len)
{
    if (s_turn.mp3_len + len > MP3_BUF) {
        ESP_LOGW(TAG, "MP3 buffer full, dropping %u bytes", (unsigned)len);
        len = MP3_BUF - s_turn.mp3_len;
    }
    mark(M_MP3);
    memcpy(s_turn.mp3 + s_turn.mp3_len, data, len);
    s_turn.mp3_len += len;
}

static void tts_end(stream_t *s, bool ok)
{
    int i = s->msg;
    close_stream(s);
    if (i < 0 || i != s_turn.tts_msg) {
        return;
    }
    if (ok) {
        s_turn.mp3_ended = true;   /* decode() drains the rest, then finishes */
    } else {
        s_turn.msgs[i].tts = TTS_FINISHED;
        s_turn.tts_msg = -1;
    }
}

/* Speaker off: queues the shown message's silence while the reply buffer has room. */
static void pace_silently(void)
{
    static const int16_t zeros[256] = {};
    msg_t &m = s_turn.msgs[s_turn.tts_msg];
    uint32_t end = m.pcm_start + m.pcm_frames + TEXT_HOLD_S * MIC_RATE;
    while (s_turn.pcm_out < end && xStreamBufferSpacesAvailable(s_out) >= sizeof(zeros)) {
        uint32_t n = end - s_turn.pcm_out < 256 ? end - s_turn.pcm_out : 256;
        xStreamBufferSend(s_out, zeros, n * sizeof(int16_t), 0);
        s_turn.pcm_out += n;
    }
    if (s_turn.pcm_out >= end) {
        m.tts = TTS_FINISHED;
        s_turn.tts_msg = -1;
        s_turn.silent = false;
    }
}

/* Decodes buffered MP3 while the reply buffer has room. */
static void decode(void)
{
    if (s_turn.tts_msg < 0) {
        return;
    }
    if (s_turn.silent) {
        pace_silently();
        return;
    }
    /*
     * minimp3 only takes a frame once it can see the next one's header. Given
     * less, it resets and says to skip all of it, which drops speech and clicks.
     * So until the stream ends, leave the last MP3_HOLD bytes for more to arrive.
     */
    size_t hold = s_turn.mp3_ended ? 0 : MP3_HOLD;
    size_t off = 0;
    while (s_turn.mp3_len - off > hold &&
           xStreamBufferSpacesAvailable(s_out) >= (MINIMP3_MAX_SAMPLES_PER_FRAME / 2 + 8) * sizeof(int16_t)) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&s_turn.dec, s_turn.mp3 + off, s_turn.mp3_len - off, s_pcm, &info);
        if (!info.frame_bytes) {
            if (s_turn.mp3_ended) {
                off = s_turn.mp3_len;   /* trailing junk */
            }
            break;
        }
        off += info.frame_bytes;
        if (!samples) {
            continue;
        }
        if (info.channels == 2) {
            for (int k = 0; k < samples; k++) {
                s_pcm[k] = (s_pcm[2 * k] + s_pcm[2 * k + 1]) / 2;
            }
        }
        if (s_turn.down_rate != info.hz) {
            ESP_LOGI(TAG, "reply audio: %d Hz, %d ch, %d kbps", info.hz, info.channels, info.bitrate_kbps);
            s_turn.down_rate = info.hz;
            resampler_init(&s_turn.down, info.hz * pitch_pct() / 100, MIC_RATE);   /* < 100: deeper and slower */
        }
        size_t n = resample(&s_turn.down, s_pcm, samples, s_pcm16);
        if (s_turn.gen == s_gen.load()) {
            mark(M_AUDIO);
            xStreamBufferSend(s_out, s_pcm16, n * sizeof(int16_t), 0);
        }
        s_turn.pcm_out += n;
        s_turn.kbps = info.bitrate_kbps;
    }
    if (off) {
        memmove(s_turn.mp3, s_turn.mp3 + off, s_turn.mp3_len - off);
        s_turn.mp3_len -= off;
    }
    msg_t &m = s_turn.msgs[s_turn.tts_msg];
    if (s_turn.mp3_ended && s_turn.kbps > 0) {
        /* All of it is here: what's decoded plus what the bitrate says the rest holds. */
        uint32_t rest = (uint32_t)((uint64_t)s_turn.mp3_len * 8 * MIC_RATE * 100 / (s_turn.kbps * 1000ull * pitch_pct()));
        m.pcm_frames = s_turn.pcm_out - m.pcm_start + rest;
    }
    if (s_turn.mp3_ended && !s_turn.mp3_len) {
        m.pcm_frames = s_turn.pcm_out - m.pcm_start;
        m.tts = TTS_FINISHED;
        s_turn.tts_msg = -1;
    }
}

/* Ends the turn once the reply is complete and spoken, or on timeouts. */
static void check_turn(void)
{
    int64_t t = now_us();
    if (s_turn.phase == P_WAIT_FINAL && t - s_turn.end_sent_us > FINAL_TIMEOUT_US) {
        on_dictation_end(true);
        return;
    }
    if (s_turn.phase != P_WAIT_REPLY) {
        return;
    }
    bool text = s_turn.text;
    if (t - s_turn.start_us > (text ? TEXT_TURN_CAP_US : TURN_CAP_US)) {
        ESP_LOGW(TAG, "turn hit the time cap");
        /* A voice turn that waited out the cap on a busy agent got no reply at all. */
        if (!text && !s_turn.nmsgs) {
            turn_fail("NO REPLY FROM MUSE");
            return;
        }
        turn_done(false);
        return;
    }
    if (!s_turn.nmsgs) {
        /* Wait as long as the agent says it's working; the turn cap still applies. */
        if (t - s_turn.chat_us > (text ? TEXT_REPLY_TIMEOUT_US : REPLY_TIMEOUT_US) && !s_turn.agent_busy) {
            turn_fail("NO REPLY FROM MUSE");
        }
        return;
    }
    for (int i = 0; i < s_turn.nmsgs; i++) {
        if (!s_turn.msgs[i].done || s_turn.msgs[i].tts == TTS_QUEUED || s_turn.msgs[i].tts == TTS_ACTIVE) {
            return;
        }
    }
    if (t - s_turn.last_event_us < SETTLE_US) {
        return;
    }
    if (s_turn.agent_busy && t - s_turn.last_content_us < (text ? TEXT_BUSY_HOLD_US : BUSY_HOLD_US)) {
        return;
    }
    ESP_LOGI(TAG, "turn done: %d message(s) in %.1fs", s_turn.nmsgs, (t - s_turn.start_us) / 1e6);
    log_marks();
    turn_done(true);
}

/* The device's IPv4 and netmask (host byte order), or 0 when it has none yet. */
static uint32_t hermes_local_ip(uint32_t *mask_out)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!n) {
        return 0;
    }
    esp_netif_ip_info_t ip = {};
    if (esp_netif_get_ip_info(n, &ip) != ESP_OK) {
        return 0;
    }
    if (mask_out) {
        *mask_out = ntohl(ip.netmask.addr);
    }
    return ntohl(ip.ip.addr);
}

static int hermes_relay_port(void)
{
    return 8788;
}

/* The home relay's base URL, honouring a stored LAN address (bench: ">relay.lan=").
 * When the gadget moves networks the compiled-in CONFIG_MUSE_HERMES_URL goes stale; the relay
 * telling us its current address lets every turn open straight onto the LAN instead of paying
 * the Tailscale funnel. Falls back to the config when nothing has been announced. */
static const char *hermes_home_url(void)
{
    static char url[80];
    char lan[64];
    muse_settings_relay_lan(lan);
    if (lan[0]) {
        if (url[0] == 0 || strstr(url, lan) == nullptr) {
            snprintf(url, sizeof(url), "http://%s:%d", lan, hermes_relay_port());
        }
        return url;
    }
    return CONFIG_MUSE_HERMES_URL;
}

/* The relay's remote (TLS) URL: one saved from the setup page wins over the compiled-in
 * default, so the browser alone can point the gadget at a relay from anywhere. Falls back
 * to CONFIG_MUSE_HERMES_URL_REMOTE ("" when neither exists). */
static const char *hermes_remote_url(void)
{
    static char url[MUSE_RELAY_URL_MAX + 1];
    char saved[MUSE_RELAY_URL_MAX + 1];
    muse_settings_relay_remote(saved);
    if (!saved[0]) {
        return CONFIG_MUSE_HERMES_URL_REMOTE;
    }
    if (strcmp(url, saved) != 0) {
        strlcpy(url, saved, sizeof(url));
    }
    return url;
}

/* The X-Gadget-Token to present: a token saved from the setup page beats the compiled-in
 * one. Same single-static pattern as hermes_home_url's buffer above. */
static const char *relay_token(void)
{
    static char tok[MUSE_RELAY_TOKEN_MAX + 1];
    if (muse_settings_relay_token_len()) {
        muse_settings_relay_token(tok);
        return tok;
    }
    return CONFIG_MUSE_HERMES_TOKEN;
}

/* True if the home relay's port answers within 1.5 s (a plain TCP connect to "http://a.b.c.d:port").
 *
 * CACHED for a second and a half, because the turn path asks several times per turn and each uncached miss costs the full 1.5 s timeout --
 * measured as ~7 s of dead air on a voice turn when the relay is on another subnet.
 *
 * Also short-circuits the common miss: when the device and the relay are on different /24s a
 * TCP connect is not going to land, so the answer is known without paying the timeout. Only the
 * NEGATIVE case is short-cut; a probe that would succeed still runs. */
static bool hermes_home_reachable(void)
{
    static int64_t checked_us;
    static bool last;
    static char checked_url[96];
    int64_t now = esp_timer_get_time();
    const char *home = hermes_home_url();
    if (checked_us && now - checked_us < 1500000LL && !strcmp(checked_url, home)) {
        return last;   /* one verdict per turn's worth of work */
    }

    char host[64];
    int port = 80;
    const char *p = strstr(home, "://");
    p = p ? p + 3 : home;
    size_t n = strcspn(p, ":");
    if (n >= sizeof(host)) {
        return false;
    }
    memcpy(host, p, n);
    host[n] = 0;
    if (p[n] == ':') {
        port = atoi(p + n + 1);
    }
    struct sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        return true;   /* a name, not an address: just try it */
    }
    /* Different subnet (per the ACTUAL netmask, not an assumed /24): the connect is not
     * going to land, so skip it and its 1.5 s timeout. Only the negative case is short-cut. */
    uint32_t mask = 0;
    uint32_t me = hermes_local_ip(&mask);
    uint32_t them = ntohl(sa.sin_addr.s_addr);
    if (me && mask && ((me ^ them) & mask) != 0) {
        checked_us = now;
        strlcpy(checked_url, home, sizeof(checked_url));
        last = false;
        return false;
    }
    int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return true;
    }
    lwip_fcntl(fd, F_SETFL, lwip_fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    lwip_connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa));
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    timeval tv = { 1, 500000 };
    bool ok = lwip_select(fd + 1, nullptr, &w, nullptr, &tv) > 0;
    int err = 1;
    socklen_t len = sizeof(err);
    if (ok) {
        lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    }
    lwip_close(fd);
    bool up = ok && err == 0;
    checked_us = now;
    strlcpy(checked_url, home, sizeof(checked_url));
    last = up;
    return up;
}

static void hermes_react_show(void)
{
    if (s_hreact[0]) {
        muse_ui_react(s_hreact);
        s_hreact[0] = '\0';
    }
}

/* ---- Gadget approvals: the relay pushes {"ask":{...}} into the turn stream and waits for
 * this card's tap (POST /approvals/<id>). The relay denies on its own timeout, so a card
 * nobody answers is a "no". ---- */

static const char *relay_base_url(bool *remote);
extern "C" bool muse_ui_choose(const char *prompt, const char *const *options, int n, int timeout_ms,
                               void (*done)(int, void *), void *user);
extern "C" bool muse_ui_choice_answer(int idx);
extern "C" void muse_input_poke(void);
extern "C" void muse_state_set_asleep(bool asleep);

/* ---- Gadget approvals: the relay pushes {"ask":{...}} into the turn stream and waits for
 * this card's tap (POST /approvals/<id>). The relay denies on its own timeout, so a card
 * nobody answers is a "no". ---- */

static SemaphoreHandle_t s_ask_sem;
static volatile int s_ask_pick;
static char s_ask_id[16];
static char s_ask_choice[4][8];
static int s_ask_n;

static void ask_done_cb(int idx, void *user)
{
    (void)user;
    s_ask_pick = idx;
    if (s_ask_sem) {
        xSemaphoreGive(s_ask_sem);
    }
}

static const char *ask_label(const char *c)
{
    if (!strcmp(c, "once")) return "ALLOW ONCE";
    if (!strcmp(c, "session")) return "ALLOW SESSION";
    if (!strcmp(c, "always")) return "ALLOW ALWAYS";
    return "DENY";
}

static void approval_post(const char *id, const char *choice)
{
    char url[MUSE_RELAY_URL_MAX + 48];
    bool remote = false;
    snprintf(url, sizeof(url), "%s/approvals/%s", relay_base_url(&remote), id);
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        relay_tls(&cfg);
    }
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 8000;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        return;
    }
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_header(h, "X-Gadget-Token", relay_token());
    char body[48];
    int n = snprintf(body, sizeof(body), "{\"choice\":\"%s\"}", choice);
    esp_http_client_set_post_field(h, body, n);
    /* One retry: a funnel hiccup must not silently drop the user's tap (the relay
     * then denies and the card was a lie). */
    esp_err_t err = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "ask %s: verdict post failed (%s %d), retrying once", id, esp_err_to_name(err), status);
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_http_client_set_post_field(h, body, n);
        err = esp_http_client_perform(h);
        status = esp_http_client_get_status_code(h);
    }
    esp_http_client_cleanup(h);
    ESP_LOGI(TAG, "ask %s -> %s (%s %d)", id, choice, esp_err_to_name(err), status);
}

static void handle_ask(cJSON *ask)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(ask, "id"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(ask, "text"));
    double tsec = 45;
    cJSON *tj = cJSON_GetObjectItem(ask, "timeout");
    if (cJSON_IsNumber(tj)) {
        tsec = tj->valuedouble;
    }
    if (tsec < 8) tsec = 8;
    if (tsec > 120) tsec = 120;
    if (!id || !id[0]) {
        return;
    }
    const char *opts[4];
    s_ask_n = 0;
    cJSON *it = nullptr;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(ask, "choices")) {
        const char *c = cJSON_GetStringValue(it);
        if (!c || s_ask_n >= 4) {
            continue;
        }
        strlcpy(s_ask_choice[s_ask_n], c, sizeof(s_ask_choice[s_ask_n]));
        opts[s_ask_n] = ask_label(s_ask_choice[s_ask_n]);
        s_ask_n++;
    }
    if (s_ask_n < 2) {
        strlcpy(s_ask_choice[0], "once", 8);
        opts[0] = "ALLOW";
        strlcpy(s_ask_choice[1], "deny", 8);
        opts[1] = "DENY";
        s_ask_n = 2;
    }
    strlcpy(s_ask_id, id, sizeof(s_ask_id));
    if (!s_ask_sem) {
        s_ask_sem = xSemaphoreCreateBinary();
    }
    s_ask_pick = -1;
    char prompt[240];
    snprintf(prompt, sizeof(prompt), "ALLOW THIS?\n%.150s", text ? text : "");
    /* The UI task PAUSES while asleep: wake first or the card never draws (and the
     * console stops reading, so a bench answer would wait for the wake). */
    muse_state_set_asleep(false);
    muse_input_poke();
    if (!muse_ui_choose(prompt, opts, s_ask_n, (int)((tsec + 5) * 1000), ask_done_cb, nullptr)) {
        ESP_LOGW(TAG, "ask %s: no UI for the card", id);
        return;
    }
    /* Logged AFTER the dialog exists: the bench hook matches this line and answers at once. */
    ESP_LOGI(TAG, "ask %s: %s", id, text ? text : "");
    int idx = -1;
    if (s_ask_sem && xSemaphoreTake(s_ask_sem, pdMS_TO_TICKS((int)((tsec + 6) * 1000))) == pdTRUE) {
        idx = s_ask_pick;
    }
    if (idx < 0 || idx >= s_ask_n) {
        ESP_LOGI(TAG, "ask %s: no tap (relay denies)", id);
        return;
    }
    approval_post(s_ask_id, s_ask_choice[idx]);
}

/* Bench: answer the open approval card as if tapped ("once"/"always"/"deny"/...). */
extern "C" bool muse_chat_ask_answer(const char *choice)
{
    int idx = -1;
    for (int i = 0; i < s_ask_n; i++) {
        if (!strcmp(s_ask_choice[i], choice)) {
            idx = i;
            break;
        }
    }
    return idx >= 0 && muse_ui_choice_answer(idx);
}

/* NDJSON line -> one action. Lines: heard / status / tts (+ the final reply/react/image line). */
static void hermes_line(cJSON *j)
{
    if (s_turn.gen != s_gen.load()) {
        return;
    }
    cJSON *askj = cJSON_GetObjectItem(j, "ask");
    if (cJSON_IsObject(askj)) {
        handle_ask(askj);
        return;
    }
    const char *heard = cJSON_GetStringValue(cJSON_GetObjectItem(j, "heard"));
    if (heard && !s_hstream.got_heard) {
        s_hstream.got_heard = true;
        emit(MUSE_HATCH_EV_HEARD, heard);
        ESP_LOGI(TAG, "hermes heard: \"%s\"", heard);
        return;
    }
    const char *status = cJSON_GetStringValue(cJSON_GetObjectItem(j, "status"));
    if (status && status[0]) {
        emit(MUSE_HATCH_EV_STATUS, status);
        return;
    }
    const char *tts = cJSON_GetStringValue(cJSON_GetObjectItem(j, "tts"));
    if (tts && tts[0]) {
        /* First speech: become the speaking message now (start_tts would have). */
        if (s_turn.tts_msg < 0) {
            msg_t &m = s_turn.msgs[0];
            m = msg_t{};
            strlcpy(m.id, "hermes", sizeof(m.id));
            s_turn.nmsgs = 1;
            s_turn.chat_posted = true;
            s_turn.chat_us = s_turn.last_event_us = s_turn.last_content_us = now_us();
            s_turn.phase = P_WAIT_REPLY;
            m.tts = TTS_ACTIVE;
            s_turn.tts_msg = 0;
            s_turn.silent = false;
            m.pcm_start = s_turn.pcm_out;
            m.pcm_frames = 0;
            s_turn.mp3_len = 0;
            s_turn.mp3_ended = false;
            s_turn.kbps = 0;
            s_turn.down_rate = 0;
            mp3dec_init(&s_turn.dec);
            hermes_react_show();   /* the mood, as the speech starts */
        }
        if (s_turn.tts_msg >= 0) {
            char *copy = static_cast<char *>(psram_alloc(strlen(tts) + 1));
            if (copy) {
                strcpy(copy, tts);
                size_t n = b64_decode_inplace(copy, strlen(copy));
                tts_data(reinterpret_cast<uint8_t *>(copy), n);
                heap_caps_free(copy);
            }
        }
        return;
    }
    /* the final line (or a compat single-JSON reply) */
    const char *reply = cJSON_GetStringValue(cJSON_GetObjectItem(j, "reply"));
    const char *react = cJSON_GetStringValue(cJSON_GetObjectItem(j, "react"));
    char *image = cJSON_GetStringValue(cJSON_GetObjectItem(j, "image_b64"));
    if (reply && reply[0]) {
        s_hstream.got_reply = true;
        muse_hatch_hermes_link_note(true);   /* the relay answered: Hermes is reachable */
        msg_t &m = s_turn.msgs[0];
        if (s_turn.nmsgs == 0) {   /* a reply with no TTS chunks (speaker off / synth failed) */
            m = msg_t{};
            strlcpy(m.id, "hermes", sizeof(m.id));
            s_turn.nmsgs = 1;
            s_turn.chat_posted = true;
            s_turn.chat_us = s_turn.last_event_us = s_turn.last_content_us = now_us();
            s_turn.phase = P_WAIT_REPLY;
        }
        append_text(m, reply);
        s_last_reply[last_reply_idx()][0] = 0;   /* this agent's reading view is replaced */
        if (s_turn.tts_msg < 0) {   /* silent (no TTS streamed): captions from the text */
            show_reply_start(m);
        } else {
            last_reply_add(0, reply);   /* spoken: the reading view keeps the whole reply */
        }
        message_done(0, reply);     /* done in both paths: check_turn waits on it */
        hermes_react_show();
        strlcpy(s_hreact, react && react[0] ? react : "", sizeof(s_hreact));
        if (image && image[0]) {
            const char *code = "", *msg2 = "";
            size_t n = b64_decode_inplace(image, strlen(image));
            if (!image_fetch_buffer(reinterpret_cast<uint8_t *>(image), n, -1, image_done, nullptr, &code, &msg2)) {
                ESP_LOGW(TAG, "hermes: image not shown: %s (%s)", msg2, code);
            }
        }
        if (s_turn.tts_msg < 0) {
            s_turn.msgs[0].tts = TTS_FINISHED;   /* nothing to drain */
            s_turn.tts_msg = -1;
        } else {
            s_turn.mp3_ended = true;             /* decode() drains the buffered frames, then finishes */
        }
        s_hstream.ended = true;
    }
}

/* A complete NDJSON line arrived: parse and dispatch. */
static void hermes_dispatch_line(char *line, size_t len)
{
    if (s_hstream.overflow) {
        ESP_LOGW(TAG, "hermes stream: skipped a line over %u bytes", (unsigned)s_hstream.cap);
        s_hstream.overflow = false;
        s_hstream.len = 0;
        return;
    }
    line[len] = 0;
    cJSON *j = cJSON_ParseWithLength(line, len);
    s_hstream.len = 0;
    if (j) {
        hermes_line(j);
        cJSON_Delete(j);
    }
}

/* Starts a streamed turn: opens the HTTP exchange; hermes_stream_pump() drives it. */
/* A client for the turn, on its own kept slot (esp_http_client_open always makes a new connection,
 * so nothing is gained by sharing one), with the note's headers. len -1: a chunked body. On a
 * failed open the refused-pin case is tried once more against the full bundle. Null: no relay. */
static esp_http_client_handle_t hermes_open(int len)
{
    char url[256];
    bool remote = false;
    snprintf(url, sizeof(url), "%s/turn?stream=1%s", relay_base_url(&remote), agent_profile_query());
    if (strstr(url + 8, "://") || !strstr(url, "://")) {
        ESP_LOGE(TAG, "hermes: bad relay address (%s)", url);
        return nullptr;
    }
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        relay_tls(&cfg);
    }
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 10000;   /* connect + TLS: ~2 s through the funnel; the mic backlog holds 8 s */
    cfg.buffer_size = 4096;
    for (int attempt = 0; attempt < 2; attempt++) {
        esp_http_client_handle_t h = keep_get(&s_keep_stream, &cfg);
        if (!h) {
            return nullptr;
        }
        esp_http_client_set_header(h, "Content-Type", "application/octet-stream");
        esp_http_client_set_header(h, "X-Audio", "ima-adpcm");
        esp_http_client_set_header(h, "X-Gadget-Token", relay_token());
        esp_http_client_close(h);   /* nothing may still be open on it (see hermes_stream_pump) */
        int64_t t0 = now_us();
        esp_err_t err = esp_http_client_open(h, len);   /* connects, then sends the headers */
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "hermes: relay %s connected in %lld ms", remote ? "(remote)" : "(home)",
                     (long long)((now_us() - t0) / 1000));
            return h;
        }
        ESP_LOGW(TAG, "hermes: relay connect failed (%s) after %lld ms", esp_err_to_name(err),
                 (long long)((now_us() - t0) / 1000));
        bool refused = relay_tls_refused(h, &cfg);
        keep_drop(&s_keep_stream);
        if (!refused) {
            break;
        }
    }
    return nullptr;
}

/* The note streams up while it is spoken: the connection opens at the press and the speech goes
 * as it comes, chunked, in IMA ADPCM (a quarter of the bytes). At release only its last fraction
 * is left to send -- the ~2 s TLS handshake through the funnel and the upload both happen while
 * the user talks. If the stream can't open or breaks, the whole note goes at release instead. */
#define UP_BYTES 2048   /* ADPCM per chunk: a quarter second */
static struct {
    esp_http_client_handle_t h;   /* open, chunked body under way; null: not streaming */
    size_t sent;                  /* bytes of the note (ADPCM) already written */
    uint8_t out[UP_BYTES + 16];   /* one chunk: its size line, the ADPCM, CRLF */
} s_up;

static void hermes_up_close(void)
{
    if (s_up.h) {
        esp_http_client_close(s_up.h);   /* the client stays in s_keep_stream */
        s_up.h = nullptr;
    }
}

/* Writes what has been recorded, a chunk at a time (all of it, and the body's end, when last).
 * False: the stream broke and is closed. */
static bool hermes_up_push(bool last)
{
    size_t avail = s_turn.pcm_bytes / 4 - s_up.sent;
    while (s_up.h && (avail >= UP_BYTES || (last && avail))) {
        size_t take = avail < UP_BYTES ? avail : UP_BYTES;
        int n = snprintf(reinterpret_cast<char *>(s_up.out), 12, "%x\r\n", (unsigned)take);
        memcpy(s_up.out + n, s_hbuf + s_up.sent, take);
        n += (int)take;
        s_up.out[n++] = '\r';
        s_up.out[n++] = '\n';
        if (esp_http_client_write(s_up.h, reinterpret_cast<const char *>(s_up.out), n) != n) {
            ESP_LOGW(TAG, "hermes: note stream broke after %u bytes; it goes whole at release", (unsigned)s_up.sent);
            hermes_up_close();
            return false;
        }
        s_up.sent += take;
        avail -= take;
    }
    if (last && s_up.h && esp_http_client_write(s_up.h, "0\r\n\r\n", 5) != 5) {
        hermes_up_close();
        return false;
    }
    return s_up.h != nullptr;
}

/* At the press: opens the turn's connection and starts the note's body. */
static void hermes_up_begin(void)
{
    s_up.h = hermes_open(-1);
    s_up.sent = 0;
    if (s_up.h) {
        esp_http_client_set_timeout_ms(s_up.h, 5000);   /* a write mustn't hold the mic backlog long */
    }
}

/* At release: the rest of the note, then the reply's headers. Falls back to sending the whole note
 * on a fresh connection once if the stream broke. False: the turn failed (already reported). */
static bool hermes_request_stream(void)
{
    char *line = static_cast<char *>(psram_alloc(160 * 1024));   /* a long sentence's b64 MP3 can be ~60-100K */
    if (!line) {
        hermes_up_close();
        turn_fail("OUT OF MEMORY");
        return false;
    }
    s_hstream.line = line;
    s_hstream.cap = 160 * 1024;
    s_hstream.len = 0;
    s_hstream.overflow = false;
    s_hstream.headers_done = false;
    s_hstream.ended = false;
    s_hstream.got_heard = false;
    s_hstream.got_reply = false;
    s_hstream.react[0] = '\0';

    int64_t t0 = now_us(), hdr = -1;
    esp_http_client_handle_t h = nullptr;
    for (int attempt = 0; attempt < 2 && hdr < 0; attempt++) {
        if (attempt == 0 && hermes_up_push(true)) {
            h = s_up.h;
            s_up.h = nullptr;
            ESP_LOGI(TAG, "hermes: note streamed (%u bytes of ADPCM), the last in %lld ms", (unsigned)s_up.sent,
                     (long long)((now_us() - t0) / 1000));
        } else {
            /* the whole note at once, on a fresh connection */
            int body = (int)(s_turn.pcm_bytes / 4);
            h = hermes_open(body);
            if (h && esp_http_client_write(h, reinterpret_cast<const char *>(s_hbuf), body) != body) {
                esp_http_client_close(h);
                h = nullptr;
            }
            if (!h) {
                continue;
            }
            ESP_LOGI(TAG, "hermes: note sent whole (%d bytes) in %lld ms", body, (long long)((now_us() - t0) / 1000));
        }
        /* The note has left the device: tell the voice task, so it can say "note sent, waiting"
         * while the agent thinks. */
        if (s_turn.gen == s_gen.load()) {
            emit(MUSE_HATCH_EV_SENT, nullptr);
        }
        esp_http_client_set_timeout_ms(h, 30000);   /* the relay answers with headers before the agent starts */
        hdr = esp_http_client_fetch_headers(h);
        if (hdr < 0 || esp_http_client_get_status_code(h) == 0) {
            ESP_LOGW(TAG, "hermes: no reply on that connection; once more with the whole note");
            esp_http_client_close(h);
            hdr = -1;
        }
    }
    if (hdr < 0) {
        heap_caps_free(line);
        s_hstream.line = nullptr;
        turn_fail("CAN'T REACH HERMES");
        return false;
    }
    ESP_LOGI(TAG, "hermes stream: reply headers %lld ms after release", (long long)((now_us() - t0) / 1000));
    s_hstream.h = h;
    /* From here the pump reads in short waits: the agent can take minutes (or hang), and this
     * task has to stay free to take the next press, which ends this reply and sends a new note. */
    esp_http_client_set_timeout_ms(h, 100);
    s_turn.chat_us = s_turn.last_event_us = now_us();
    return true;
}

/* Pumps the streamed response; true while the exchange continues (call again next tick). */
static bool hermes_stream_pump(void)
{
    hermes_stream_t &hs = s_hstream;
    if (!hs.h) {
        return false;
    }
    uint8_t chunk[2048];
    int n = 0, reads = 0;
    /* A few reads a pass, then back to the task's loop (commands, the next press): a reply that
     * trickles status lines, or none at all, must not hold the task. */
    while (reads++ < 8 && (n = esp_http_client_read(hs.h, reinterpret_cast<char *>(chunk), sizeof(chunk))) > 0) {
        for (int i = 0; i < n && !hs.ended; i++) {
            char ch = static_cast<char>(chunk[i]);
            if (ch == '\n') {
                if (hs.len || hs.overflow) {
                    hermes_dispatch_line(hs.line, hs.len);
                }
            } else if (ch != '\r') {
                if (hs.len == hs.cap - 1) {
                    hs.overflow = true;
                } else {
                    hs.line[hs.len++] = ch;
                }
            }
        }
        if (hs.ended) {
            break;
        }
    }
    if (!hs.ended && (n > 0 || n == -ESP_ERR_HTTP_EAGAIN)) {
        return true;   /* more to come (or nothing yet): next pass */
    }
    if (hs.ended || n <= 0) {   /* ended, or the server closed (read returned 0/-1) */
        int status = esp_http_client_get_status_code(hs.h);
        /* Closed even when cleanly finished: esp_http_client_open always connects anew, and over a
         * connection left open its TLS layer starts a new session without freeing the old one --
         * ~39 KB of PSRAM lost per Hermes turn, until Muse couldn't connect any more. */
        esp_http_client_close(hs.h);
        hs.h = nullptr;
        heap_caps_free(hs.line);
        hs.line = nullptr;
        if (hs.overflow) {
            ESP_LOGW(TAG, "hermes stream: trailing partial line dropped");
        }
        if (status != 200 && status != 0) {
            ESP_LOGW(TAG, "hermes stream: HTTP %d", status);
            return false;
        }
        return hs.got_reply;
    }
    return true;
}

/* Collects the mic while the button is held, then asks the relay. */
static void hermes_step(void)
{
    if (s_turn.phase == P_LISTEN) {
        if (!s_hbuf) {   /* allocated at turn_begin; missing means OOM -- fail the turn, never write */
            ESP_LOGE(TAG, "hermes: mic buffer missing at listen; failing the turn");
            turn_fail("OUT OF MEMORY");
            return;
        }
        if (!s_turn.up_started) {
            s_turn.up_started = true;
            hermes_up_begin();   /* blocks for the handshake; the mic backlog holds 8 s meanwhile */
        }
        static int16_t stage[1024];   /* the mic backlog, a few KB at a time, into the ADPCM note */
        for (;;) {
            size_t want = xStreamBufferBytesAvailable(s_in) & ~(size_t)3;   /* whole sample pairs */
            size_t room = HERMES_MAX_BYTES - s_turn.pcm_bytes;
            want = want < sizeof(stage) ? want : sizeof(stage);
            want = want < room ? want : room & ~(size_t)3;
            size_t got = want ? xStreamBufferReceive(s_in, stage, want, 0) & ~(size_t)3 : 0;
            if (!got) {
                break;
            }
            muse_adpcm_encode_block(&s_hnote, stage, got / 2, s_hbuf + s_turn.pcm_bytes / 4);
            s_turn.pcm_bytes += got;
        }
        if (!s_turn.end_requested && s_turn.pcm_bytes < HERMES_MAX_BYTES) {
            hermes_up_push(false);
            return;
        }
        mark(M_RELEASE);
        if (s_turn.pcm_bytes < MIC_RATE * 2 * 3 / 10) {
            hermes_up_close();
            turn_fail("DIDN'T CATCH THAT");
            return;
        }
        ESP_LOGI(TAG, "hermes: sending %.2fs of speech", (double)s_turn.pcm_bytes / (MIC_RATE * 2));
        mark(M_SENT);
        if (!hermes_request_stream()) {
            return;   /* it failed the turn already */
        }
        s_turn.phase = P_WAIT_REPLY;
    } else if (s_turn.phase == P_WAIT_REPLY) {
        if (s_hstream.h) {
            if (!hermes_stream_pump()) {   /* the exchange is over (fine or failed) */
                if (!s_hstream.got_reply && s_turn.gen == s_gen.load()) {
                    if (s_hstream.got_heard) {
                        /* The relay answered (it told us what it heard) but sent no reply:
                         * a silent/no-speech press, not a dead link. Do not flag Hermes. */
                        muse_hatch_hermes_link_note(true);
                        turn_fail("DIDN'T CATCH THAT");
                    } else {
                        /* The relay did not answer at all: record the fault so the UI
                         * can show Hermes's own link state (it is not Muse's problem). */
                        muse_hatch_hermes_link_note(false);
                        turn_fail("CAN'T REACH HERMES");
                    }
                }
            }
        }
        start_tts();
        decode();
        check_turn();
    }
}

/* ---- Inbound dispatch ---- */

/* Splits NDJSON; calls fn for each complete line. */
static void feed_lines(stream_t *s, const uint8_t *data, size_t len, void (*fn)(cJSON *))
{
    int64_t id = s->id;
    for (size_t i = 0; i < len; i++) {
        char ch = data[i];
        if (ch != '\n') {
            if (s->len == s->cap - 1 && !s->overflow && s->cap < SUB_LINE_MAX) {
                /* A long reply's done event carries its whole text. */
                char *grown = static_cast<char *>(heap_caps_realloc(s->line, s->cap * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (grown) {
                    s->line = grown;
                    s->cap *= 2;
                }
            }
            if (s->len < s->cap - 1) {
                s->line[s->len++] = ch;
            } else {
                s->overflow = true;
            }
            continue;
        }
        if (s->overflow) {
            ESP_LOGW(TAG, "stream %lld: skipped a line over %u bytes", (long long)s->id, (unsigned)s->cap);
        } else if (s->len) {
            cJSON *j = cJSON_ParseWithLength(s->line, s->len);
            if (j) {
                fn(j);
                cJSON_Delete(j);
            }
        }
        if (!alive(s, id)) {
            return;   /* fn ended the stream */
        }
        s->len = 0;
        s->overflow = false;
    }
}

static void stream_data(stream_t *s, ConstByteSpan data)
{
    if (data.empty()) {
        return;
    }
    switch (s->kind) {
    case K_SUB:
        feed_lines(s, data.data(), data.size(), on_event);
        break;
    case K_DICT:
        feed_lines(s, data.data(), data.size(), on_dictation_line);
        break;
    case K_CHAT: {
        size_t take = data.size() < s->cap - 1 - s->len ? data.size() : s->cap - 1 - s->len;
        memcpy(s->line + s->len, data.data(), take);
        s->len += take;
        break;
    }
    case K_TTS:
        if (s->msg == s_turn.tts_msg) {
            tts_data(data.data(), data.size());
        }
        break;
    default:
        break;
    }
}

/* Returns false when the connection has to go. */
static bool stream_end(stream_t *s, bool ok)
{
    switch (s->kind) {
    case K_SUB:
        ESP_LOGW(TAG, "subscription ended");
        return false;
    case K_DICT:
        close_stream(s);
        on_dictation_end(ok);
        break;
    case K_CHAT:
        if (ok) {
            on_chat_ack(s);
        }
        close_stream(s);
        s_turn.chat_id = 0;
        if (!ok) {
            turn_fail("MUSE DIDN'T TAKE IT");
        }
        break;
    case K_TTS:
        tts_end(s, ok);
        break;
    default:
        break;
    }
    return true;
}

static bool on_http_error(stream_t *s, const ApplicationResponseView &resp)
{
    char body[160];
    size_t n = resp.body.size() < sizeof(body) - 1 ? resp.body.size() : sizeof(body) - 1;
    memcpy(body, resp.body.data(), n);
    body[n] = '\0';
    ESP_LOGW(TAG, "stream %lld: HTTP %d %s", (long long)s->id, (int)resp.status, body);
    return stream_end(s, false);
}

static bool on_frame(const DecodedServiceFrame &f)
{
    stream_t *s = find_stream(f.stream_id);
    if (!s) {
        return true;
    }
    switch (f.kind) {
    case ServiceFrameKind::Response:
        s->status = f.response.status;
        if (f.response.status >= 400) {
            return on_http_error(s, f.response);
        }
        stream_data(s, f.response.body);
        if (f.response.end_body && alive(s, f.stream_id)) {
            return stream_end(s, true);
        }
        break;
    case ServiceFrameKind::BodyChunk:
        stream_data(s, f.body_chunk.data);
        if (f.body_chunk.end_body && alive(s, f.stream_id)) {
            return stream_end(s, true);
        }
        break;
    case ServiceFrameKind::Reset:
        ESP_LOGW(TAG, "stream %lld reset: %.*s", (long long)f.stream_id, (int)f.reset.reason.size(),
                 f.reset.reason.data());
        return stream_end(s, false);
    default:
        break;
    }
    return true;
}

/* Handles whatever the server sent. Returns false if the connection failed. */
static bool poll_socket(void)
{
    static HeaderView hdrs[16];
    for (int budget = 0; budget < 8; budget++) {
        /* Hold off while the MP3 buffer is nearly full: TCP pushes back on the VM. */
        if (s_turn.tts_msg >= 0 && MP3_BUF - s_turn.mp3_len < MP3_POLL_ROOM) {
            return true;
        }
        ssize_t n = ws_recv(s_conn.tls, s_conn.rx, SCRATCH, false);
        if (n == -2) {
            return true;
        }
        if (n == 0 || n == -1) {
            ESP_LOGW(TAG, "%s", n ? "receive error" : "server closed the connection");
            return false;
        }
        s_conn.last_rx_us = now_us();
        if (n == -3) {
            continue;
        }
        auto in = s_conn.session->ProcessInboundWebSocketPayload(ConstByteSpan(s_conn.rx, n),
                                                                 ByteSpan(s_conn.tf, SCRATCH),
                                                                 ByteSpan(s_conn.sr, SCRATCH),
                                                                 Span<HeaderView>(hdrs, 16));
        if (!in.ok()) {
            ESP_LOGW(TAG, "inbound frame: %s", in.status.str());
            return false;
        }
        if (in.frame_status == InboundFrameStatus::Complete && !on_frame(in.frame)) {
            return false;
        }
        if (!s_connected) {
            return false;   /* a handler hit a send failure */
        }
    }
    return true;
}

/* ---- Task ---- */

static void drop_connection(const char *why)
{
    bool in_turn = s_turn.phase != P_IDLE;
    disconnect(why);
    s_auto_next_us = now_us() + s_auto_backoff_us;
    if (in_turn) {
        turn_fail("LOST CONNECTION TO MUSE");
    }
    muse_hatch_report(MUSE_HATCH_UNTESTED, "");
}

static void handle(const cmd_t &cmd)
{
    switch (cmd.type) {
    case CMD_CONNECT:
        if (s_turn.phase == P_IDLE) {
            disconnect("reconnect requested");
            ensure_connected();
        }
        break;
    case CMD_FORGET:
        if (s_turn.phase != P_IDLE) {
            turn_fail("SETTINGS CHANGED");
        }
        disconnect("settings changed");
        forget_vm();
        s_auto_next_us = 0;
        s_auto_backoff_us = AUTO_RETRY_MIN_US;
        break;
    case CMD_AGENT:   /* Muse <-> Hermes: drop the Muse link, keep its VM details, reconnect on demand */
        if (s_turn.phase != P_IDLE) {
            turn_fail("SWITCHED AGENT");
        }
        disconnect("agent switch");
        s_auto_next_us = 0;
        s_auto_backoff_us = AUTO_RETRY_MIN_US;
        break;
    case CMD_BEGIN:
        if (cmd.gen == s_gen.load()) {
            turn_begin(cmd.gen);
        }
        break;
    case CMD_END:
        if (cmd.gen == s_turn.gen && !s_turn.text && s_turn.phase == P_LISTEN) {
            s_turn.end_requested = true;
        }
        break;
    case CMD_CANCEL:
        if (cmd.gen == s_turn.gen && !s_turn.text && s_turn.phase != P_IDLE) {
            ESP_LOGI(TAG, "turn cancelled");
            turn_finish();
        }
        break;
    case CMD_TEXT:
        if (hermes_mode()) {
            muse_hatch_console("error", "HERMES: VOICE ONLY", nullptr);
        } else if (s_turn.phase != P_IDLE && !s_turn.text) {
            muse_hatch_console("error", "BUSY WITH A VOICE TURN", nullptr);
        } else {
            text_begin(cmd.text);
        }
        free(cmd.text);
        break;
    case CMD_TEXT_CANCEL:
        if (s_turn.text && s_turn.phase != P_IDLE) {
            turn_fail("CANCELLED");
        }
        break;
    case CMD_WAKE:   /* only ends hatch_task's resting wait */
        break;
    }
}

/* ---- Notifications: the REVERSE channel (relay -> device, unprompted) ----
 *
 * The relay hands the gadget a LINE to present on its own, labelled with the EXACT
 * agent that raised it. When one arrives and the gadget is idle, it looks up, plays its mood,
 * chirps and prints the line; tapping the face OPENS that agent -- switching to the right slot
 * (and, for an ALT profile, loading its name) so the next thing you say reaches the agent that
 * notified, never a generic default. Gated by s_notif_auto (bench: ">notif.auto off").
 *
 * The slot ("which agent") is decided on the RELAY (notifications.resolve_agent) and arrives as
 * is_default: True -> the Hermes slot, False -> the ALT slot loaded with `agent`. */

/* Runtime guard for the notification poll (bench: ">notif.auto off"). */
static bool s_notif_auto = true;

/* The relay's base URL; *remote selects the TLS-verified remote address when home is
 * unreachable. Shared by the notification calls. */
static const char *relay_base_url(bool *remote)
{
    const char *rem = hermes_remote_url();
    *remote = false;
    if (rem[0] && !hermes_home_reachable()) {
        *remote = true;
        return rem;
    }
    return hermes_home_url();
}
/* The text a nudge wants spoken, set by notif_present and consumed by the voice task. Kept
 * short: a nudge is a line, and the speaker is the voice task's alone (same law as mp3test). */
#define NOTIF_SPEAK_MAX 96
static char s_notif_speak[NOTIF_SPEAK_MAX];

struct notif_pcm_t { TaskHandle_t caller; int16_t *pcm; size_t n; };

/* Google TTS for one line -> 16 kHz mono PCM (PSRAM). Mirrors the reply's fetch_tts + decode,
 * but self-contained so it never touches the turn state machine. 0 on any failure. */
static size_t notif_decode(const char *text, int16_t **pcm_out)
{
    *pcm_out = nullptr;
    if (!fetch_tts(text) || s_turn.mp3_len == 0) {
        return 0;
    }
    int16_t *out = (int16_t *)heap_caps_malloc(256 * 1024, MALLOC_CAP_SPIRAM);
    if (!out) {
        return 0;
    }
    /* 6.6 KB of decoder state, used only while a notification is synthesized: PSRAM, not
     * internal DRAM (a plain static claimed internal RAM and cost ~6.6 KB of the scarce pool). */
    static EXT_RAM_BSS_ATTR mp3dec_t dec;
    mp3dec_init(&dec);
    resampler_t rs;
    bool rs_init = false;
    size_t off = 0, frames = 0;
    while (s_turn.mp3_len - off > 0 && frames < 96 * 1024) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&dec, s_turn.mp3 + off, s_turn.mp3_len - off, s_pcm, &info);
        if (!info.frame_bytes) {
            break;
        }
        off += info.frame_bytes;
        if (!samples) {
            continue;
        }
        if (info.channels == 2) {
            for (int k = 0; k < samples; k++) {
                s_pcm[k] = (s_pcm[2 * k] + s_pcm[2 * k + 1]) / 2;
            }
        }
        if (!rs_init) {
            resampler_init(&rs, info.hz, MIC_RATE);
            rs_init = true;
        }
        frames += resample(&rs, s_pcm, samples, out + frames);
    }
    s_turn.mp3_len = 0;
    if (!frames) {
        heap_caps_free(out);
        return 0;
    }
    *pcm_out = out;
    return frames;
}

/* Runs the decode on its own PSRAM-stack task (Google TLS + minimp3 need the stack), like the
 * mp3 selftest: the voice task calls it and gets back the PCM to write. */
extern "C" size_t muse_hatch_notif_pcm(int16_t **pcm_out)
{
    notif_pcm_t st = { xTaskGetCurrentTaskHandle(), nullptr, 0 };
    auto body = [](void *arg) {
        auto *st = (notif_pcm_t *)arg;
        st->n = notif_decode(s_notif_speak, &st->pcm);
        xTaskNotifyGive(st->caller);
        vTaskSuspend(NULL);
    };
    TaskHandle_t task;
    if (xTaskCreatePinnedToCoreWithCaps(body, "notif_pcm", 40 * 1024, &st, 5, &task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        return 0;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDeleteWithCaps(task);
    *pcm_out = st.pcm;
    return st.n;
}

#define NOTIF_POLL_US (15 * 1000000LL)     /* idle nudge poll (matches the task cadence) */
#define NOTIF_MIN_GAP_US (45 * 1000000LL)  /* at most one presented nudge every 45 s */
#define NOTIF_PENDING_TTL_US (120 * 1000000LL)  /* un-tapped: stop blocking + stop being stale */
#define NOTIF_TEXT_MAX 90                       /* a nudge is a SHORT line (caption is ~2 lines) */

struct rel_notif_t {
    char id[40];
    char agent[MUSE_AGENT_PROFILE_MAX + 1];
    bool is_default;
    char label[40];
    char mood[24];
    char text[240];
    bool valid;
};

static int64_t s_notif_poll_us;
static int s_notif_fail_streak;
/* A presented nudge waiting for a tap. While one is pending we neither present another (no
 * stacking) nor re-present this one (it is acked on arrival). */
static bool s_notif_pending;
static int64_t s_notif_until;   /* when an un-tapped nudge stops being pending */
static bool s_notif_default;
static char s_notif_agent[MUSE_AGENT_PROFILE_MAX + 1];
static char s_notif_label[40];
static int64_t s_notif_gap_us;   /* earliest time the next nudge may be presented */

/* ---- Notification inbox: the settings page's history snapshot ----
 *
 * The LVGL task renders rows; only this (session) task fetches or mutates. A refresh fills the
 * buffer the readers do NOT hold (1 - s_hist_active), then publishes by flipping s_hist_active
 * and bumping s_hist_gen; the page rebuilds when the gen changes. A reader may in the worst
 * case see one row mid-install, for one frame -- acceptable for a list (same spirit as
 * s_last_reply's flip). All in PSRAM: 2 x 12 rows. */
#define NOTIF_HIST_MAX 12
static EXT_RAM_BSS_ATTR muse_notif_row_t s_hist[2][NOTIF_HIST_MAX];
static volatile int s_hist_n[2];
static volatile int s_hist_active;
static volatile uint32_t s_hist_gen;
static volatile int s_hist_state;      /* muse_notif_hist_t, kept as int for the lock-free read */
static volatile bool s_hist_want;      /* a fetch is queued (UI asks, session serves) */
static char s_hist_cancel[40];         /* id to delete on the relay (session task only) */
static volatile bool s_hist_cancel_want;
static volatile bool s_hist_clear_want;   /* wipe history (keep un-fired reminders) */
static int64_t s_hist_next_us;         /* spacing between inbox network operations */

extern "C" void muse_ui_agent_switch(bool hermes);   /* muse_ui.h has no C++ guards */
extern "C" void muse_input_poke(void);              /* muse_input.h has no C++ guards either */
extern "C" bool muse_input_dnd(void);               /* face-down quiet mode (muse_input.c) */

/* GET /notifications -> the newest unread nudge, or false. *reachable: the relay answered. */
static bool poll_notification(rel_notif_t *out, bool *reachable)
{
    out->valid = false;
    if (reachable) {
        *reachable = false;
    }
    char url[MUSE_RELAY_URL_MAX + 48];
    bool remote = false;
    snprintf(url, sizeof(url), "%s/notifications", relay_base_url(&remote));
    const size_t CAP = 8 * 1024;
    char *resp = static_cast<char *>(psram_alloc(CAP));
    if (!resp) {
        return false;
    }
    tts_buf_t buf = { resp, 0, CAP - 1, false };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        relay_tls(&cfg);
    }
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 2500;
    cfg.event_handler = tts_http_evt;
    cfg.user_data = &buf;
    int status;
    esp_err_t err = keep_perform(&s_keep_relay, &cfg, [&](esp_http_client_handle_t h) {
        buf.len = 0;
        buf.over = false;
        esp_http_client_set_header(h, "X-Gadget-Token", relay_token());
    }, &status);
    bool ok = false;
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "notif: GET /notifications failed (err %s, HTTP %d)", esp_err_to_name(err), status);
    } else {
        if (reachable) {
            *reachable = true;
        }
        resp[buf.len] = '\0';
        cJSON *root = cJSON_Parse(resp);
        cJSON *arr = root ? cJSON_GetObjectItem(root, "notifications") : nullptr;
        cJSON *first = cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, 0) : nullptr;
        if (cJSON_IsObject(first)) {
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(first, "id"));
            const char *agent = cJSON_GetStringValue(cJSON_GetObjectItem(first, "agent"));
            const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(first, "label"));
            const char *mood = cJSON_GetStringValue(cJSON_GetObjectItem(first, "mood"));
            const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(first, "text"));
            cJSON *isd = cJSON_GetObjectItem(first, "is_default");
            if (id && id[0] && text && text[0]) {
                strlcpy(out->id, id, sizeof(out->id));
                strlcpy(out->agent, agent ? agent : "", sizeof(out->agent));
                strlcpy(out->label, label ? label : "", sizeof(out->label));
                strlcpy(out->mood, mood ? mood : "", sizeof(out->mood));
                strlcpy(out->text, text, sizeof(out->text));
                out->is_default = cJSON_IsTrue(isd);
                out->valid = true;
                ok = true;
            }
        }
        cJSON_Delete(root);
    }
    heap_caps_free(resp);
    return ok;
}

/* POST /notifications/ack {id}: mark it presented so the next poll returns the one behind it. */
static void notif_ack(const char *id)
{
    cJSON *body = cJSON_CreateObject();
    if (!body) {
        return;
    }
    cJSON_AddStringToObject(body, "id", id);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) {
        return;
    }
    char url[MUSE_RELAY_URL_MAX + 48];
    bool remote = false;
    snprintf(url, sizeof(url), "%s/notifications/ack", relay_base_url(&remote));
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        relay_tls(&cfg);
    }
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 8000;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h) {
        esp_http_client_set_header(h, "Content-Type", "application/json");
        esp_http_client_set_header(h, "X-Gadget-Token", relay_token());
        esp_http_client_set_post_field(h, json, strlen(json));
        (void)esp_http_client_perform(h);
        esp_http_client_cleanup(h);
    }
    free(json);
}

/* ---- Notification inbox: fetch / cancel / publish ---- */

/* GET /notifications/history -> fill the inactive buffer, then publish it. Session task only. */
static void notif_hist_fetch(void)
{
    char url[MUSE_RELAY_URL_MAX + 48];
    bool remote = false;
    snprintf(url, sizeof(url), "%s/notifications/history?limit=%d", relay_base_url(&remote), NOTIF_HIST_MAX);
    const size_t CAP = 20 * 1024;
    char *resp = static_cast<char *>(psram_alloc(CAP));
    if (!resp) {
        s_hist_state = MUSE_NOTIF_HIST_FAIL;
        return;
    }
    tts_buf_t buf = { resp, 0, CAP - 1, false };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        relay_tls(&cfg);
    }
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 4000;
    cfg.event_handler = tts_http_evt;
    cfg.user_data = &buf;
    bool ok = false;
    {
        int status;
        esp_err_t err = keep_perform(&s_keep_relay, &cfg, [&](esp_http_client_handle_t h) {
            buf.len = 0;
            buf.over = false;
            esp_http_client_set_header(h, "X-Gadget-Token", relay_token());
        }, &status);
        if (err == ESP_OK && status == 200) {
            resp[buf.len] = '\0';
            cJSON *root = cJSON_Parse(resp);
            cJSON *arr = root ? cJSON_GetObjectItem(root, "notifications") : nullptr;
            if (cJSON_IsArray(arr)) {
                int slot = 1 - s_hist_active;
                int n = 0;
                cJSON *it = nullptr;
                cJSON_ArrayForEach(it, arr) {
                    if (n >= NOTIF_HIST_MAX || !cJSON_IsObject(it)) {
                        break;
                    }
                    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(it, "id"));
                    if (!id || !id[0]) {
                        continue;
                    }
                    muse_notif_row_t *r = &s_hist[slot][n];
                    memset(r, 0, sizeof(*r));
                    strlcpy(r->id, id, sizeof(r->id));
                    const char *agent = cJSON_GetStringValue(cJSON_GetObjectItem(it, "agent"));
                    const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(it, "label"));
                    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(it, "text"));
                    strlcpy(r->agent, agent ? agent : "", sizeof(r->agent));
                    strlcpy(r->label, label ? label : "", sizeof(r->label));
                    strlcpy(r->text, text ? text : "", sizeof(r->text));
                    cJSON *due = cJSON_GetObjectItem(it, "due");
                    cJSON *ts = cJSON_GetObjectItem(it, "ts");
                    cJSON *isd = cJSON_GetObjectItem(it, "is_default");
                    r->due = cJSON_IsNumber(due) ? (long)cJSON_GetNumberValue(due) : 0;
                    r->ts = cJSON_IsNumber(ts) ? (long)cJSON_GetNumberValue(ts) : 0;
                    r->is_default = cJSON_IsTrue(isd);
                    r->read = cJSON_IsTrue(cJSON_GetObjectItem(it, "read"));
                    n++;
                }
                cJSON_Delete(root);
                /* Publish ONLY when the content actually changed. A rebuild is a full
                 * delete+recreate of the page's rows, and one landing between a finger's
                 * down and up eats the tap: an unchanged 2 s poll must not touch the list. */
                int a = s_hist_active;
                bool same = (n == s_hist_n[a]);
                for (int i = 0; same && i < n; i++) {
                    const muse_notif_row_t *x = &s_hist[slot][i];
                    const muse_notif_row_t *y = &s_hist[a][i];
                    same = !strcmp(x->id, y->id) && x->read == y->read && x->due == y->due &&
                           !strcmp(x->text, y->text) && !strcmp(x->label, y->label);
                }
                if (!same) {
                    s_hist_n[slot] = n;
                    s_hist_active = slot;   /* publish */
                    s_hist_gen = s_hist_gen + 1;
                }
                s_hist_state = MUSE_NOTIF_HIST_OK;
                ok = true;
            } else {
                cJSON_Delete(root);
            }
        }
    }
    heap_caps_free(resp);
    if (!ok) {
        s_hist_state = MUSE_NOTIF_HIST_FAIL;
    }
}

/* POST /notifications/cancel {id}: delete it on the relay (a cancelled reminder must not
 * fire later). Best-effort: a failure leaves the row; the next refresh tells the truth. */
static void notif_hist_cancel(const char *id)
{
    cJSON *body = cJSON_CreateObject();
    if (!body) {
        return;
    }
    cJSON_AddStringToObject(body, "id", id);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) {
        return;
    }
    char url[MUSE_RELAY_URL_MAX + 48];
    bool remote = false;
    snprintf(url, sizeof(url), "%s/notifications/cancel", relay_base_url(&remote));
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        relay_tls(&cfg);
    }
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 4000;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    bool ok = false;
    if (h) {
        esp_http_client_set_header(h, "Content-Type", "application/json");
        esp_http_client_set_header(h, "X-Gadget-Token", relay_token());
        esp_http_client_set_post_field(h, json, strlen(json));
        ok = esp_http_client_perform(h) == ESP_OK && esp_http_client_get_status_code(h) == 200;
        esp_http_client_cleanup(h);
    }
    free(json);
    ESP_LOGI(TAG, "notif: cancel %s %s", id, ok ? "ok" : "failed");
    if (ok) {
        s_hist_want = true;   /* re-fetch so the page drops the row */
    }
}

/* POST /notifications/clear: wipe the history, but the RELAY keeps un-fired reminders (a
 * clear must not silently cancel a commitment set minutes ago). Re-fetch after, so the
 * page shows what survived. Session task only. */
static void notif_hist_clear(void)
{
    char url[MUSE_RELAY_URL_MAX + 48];
    bool remote = false;
    snprintf(url, sizeof(url), "%s/notifications/clear", relay_base_url(&remote));
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        relay_tls(&cfg);
    }
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 4000;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    bool ok = false;
    if (h) {
        esp_http_client_set_header(h, "Content-Type", "application/json");
        esp_http_client_set_header(h, "X-Gadget-Token", relay_token());
        esp_http_client_set_post_field(h, "{}", 2);
        ok = esp_http_client_perform(h) == ESP_OK && esp_http_client_get_status_code(h) == 200;
        esp_http_client_cleanup(h);
    }
    ESP_LOGI(TAG, "notif: clear %s", ok ? "ok" : "failed");
    s_hist_want = true;   /* show the result either way */
}

/* The inbox's idle work: one fetch or cancel at a time, never during a turn. Session task. */
static void notif_hist_tick(void)
{
    if (!muse_settings_relay_configured() || !muse_settings_agent_hermes()) {
        return;
    }
    if (s_turn.phase != P_IDLE || s_resting.load() || now_us() < s_hist_next_us) {
        return;
    }
    if (s_hist_clear_want) {
        s_hist_clear_want = false;
        notif_hist_clear();
        s_hist_next_us = now_us() + 2 * 1000000LL;
        return;
    }
    if (s_hist_cancel_want) {
        s_hist_cancel_want = false;
        notif_hist_cancel(s_hist_cancel);
        s_hist_next_us = now_us() + 2 * 1000000LL;
        return;
    }
    if (s_hist_want && muse_wifi_connected() && muse_hatch_configured()) {
        s_hist_want = false;
        notif_hist_fetch();
        s_hist_next_us = now_us() + 2 * 1000000LL;
    }
    /* While the page is open the LVGL task asks again about once a second (muse_hatch_notif_
     * refresh), so a new nudge appears without any tap; the 2 s spacing above bounds the rate. */
}

/* ---- Public: the settings inbox page (any task; only the flags cross) ---- */

extern "C" void muse_hatch_notif_refresh(void)
{
    if (muse_settings_relay_configured() && muse_settings_agent_hermes() &&
        !s_hist_want && !s_hist_cancel_want) {
        s_hist_want = true;
        if (s_hist_state != MUSE_NOTIF_HIST_OK) {
            s_hist_state = MUSE_NOTIF_HIST_WAIT;
        }
    }
}

extern "C" muse_notif_hist_t muse_hatch_notif_hist_state(void)
{
    return (muse_notif_hist_t)s_hist_state;
}

extern "C" uint32_t muse_hatch_notif_hist_gen(void)
{
    return s_hist_gen;
}

extern "C" int muse_hatch_notif_hist_count(void)
{
    int a = s_hist_active;
    return s_hist_n[a];
}

extern "C" bool muse_hatch_notif_hist(int i, muse_notif_row_t *out)
{
    int a = s_hist_active;
    if (i < 0 || i >= s_hist_n[a]) {
        return false;
    }
    *out = s_hist[a][i];
    return true;
}

extern "C" void muse_hatch_notif_cancel(const char *id)
{
    if (!id || !id[0] || s_hist_cancel_want) {
        return;
    }
    strlcpy(s_hist_cancel, id, sizeof(s_hist_cancel));
    s_hist_cancel_want = true;
}

extern "C" void muse_hatch_notif_clear(void)
{
    s_hist_clear_want = true;
}

/* Show one nudge on the glass: look up, its mood, a chirp, and the line with a tap hint. */
static void notif_present(const rel_notif_t *n)
{
    /* Wake the screen so the nudge is SEEN. On battery the UI task is PAUSED while asleep, and
     * only the input task resumes it -- so poke that task too, or the display stays paused while
     * "awake" and the UI stall watchdog aborts the board. */
    muse_state_set_asleep(false);
    muse_input_poke();
    s_notif_pending = true;
    s_notif_until = now_us() + NOTIF_PENDING_TTL_US;
    s_notif_default = n->is_default;
    strlcpy(s_notif_agent, n->agent, sizeof(s_notif_agent));
    strlcpy(s_notif_label, n->label[0] ? n->label : n->agent, sizeof(s_notif_label));
    if (n->mood[0]) {
        (void)muse_ui_react(n->mood);   /* an unknown mood just returns false, no harm */
    }
    muse_voice_request_notif();   /* the notification chime (notif.pcm) */
    if (strlen(n->text) <= NOTIF_SPEAK_MAX - 1) {
        strlcpy(s_notif_speak, n->text, sizeof(s_notif_speak));
        muse_voice_request_notif_speak();   /* the voice task speaks it (owns the speaker) */
    }
    char cap[300];
    /* The caption box is TWO lines of ~16 chars. A short nudge keeps the hint on its OWN line
     * (a wide "text | TAP TO OPEN" wrapped mid-word and read like a truncated sentence); a
     * longer one shows the words alone and relies on reading mode. */
    if (strlen(n->text) <= 16) {
        snprintf(cap, sizeof(cap), "%s\nTAP TO OPEN", n->text);
    } else {
        snprintf(cap, sizeof(cap), "%.*s", NOTIF_TEXT_MAX, n->text);
    }
    muse_state_set_caption("%s", cap);
    muse_state_poke();
    notif_ack(n->id);   /* one presentation per nudge */
    s_notif_gap_us = now_us() + NOTIF_MIN_GAP_US;
    ESP_LOGI(TAG, "notif from %s (%s): %s", n->agent, n->is_default ? "hermes" : "alt", n->text);
}

/* ---- Glance cards: GET /glance every few minutes while idle; the UI cycles them with the clock.
 * Same double buffer as the inbox: fill the inactive half, then flip s_glance_active. */
static EXT_RAM_BSS_ATTR muse_glance_t s_glance[2][MUSE_GLANCE_MAX];
static volatile int s_glance_n[2];
static volatile int s_glance_active;
static volatile uint32_t s_glance_gen;
static volatile int64_t s_glance_ok_us;   /* last successful fetch */
static int64_t s_glance_next_us;

static void glance_fetch(void)
{
    char url[MUSE_RELAY_URL_MAX + 48];
    bool remote = false;
    snprintf(url, sizeof(url), "%s/glance", relay_base_url(&remote));
    const size_t CAP = 4 * 1024;
    char *resp = static_cast<char *>(psram_alloc(CAP));
    if (!resp) {
        return;
    }
    tts_buf_t buf = { resp, 0, CAP - 1, false };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        relay_tls(&cfg);
    }
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 2500;
    cfg.event_handler = tts_http_evt;
    cfg.user_data = &buf;
    int status;
    esp_err_t err = keep_perform(&s_keep_relay, &cfg, [&](esp_http_client_handle_t h) {
        buf.len = 0;
        buf.over = false;
        esp_http_client_set_header(h, "X-Gadget-Token", relay_token());
    }, &status);
    if (err == ESP_OK && status == 200) {
        resp[buf.len] = '\0';
        cJSON *root = cJSON_Parse(resp);
        int fill = 1 - s_glance_active, n = 0;
        cJSON *it = nullptr;
        cJSON *cards = root ? cJSON_GetObjectItem(root, "cards") : nullptr;
        cJSON_ArrayForEach(it, cards) {
            const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(it, "kind"));
            const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(it, "title"));
            const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(it, "text"));
            const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(it, "value"));
            const char *lbl = cJSON_GetStringValue(cJSON_GetObjectItem(it, "label"));
            const char *detail = cJSON_GetStringValue(cJSON_GetObjectItem(it, "detail"));
            const char *icon = cJSON_GetStringValue(cJSON_GetObjectItem(it, "icon"));
            cJSON *prog = cJSON_GetObjectItem(it, "progress");
            if (n >= MUSE_GLANCE_MAX || !text || !text[0]) {
                continue;
            }
            muse_glance_t *g = &s_glance[fill][n++];
            memset(g, 0, sizeof(*g));   /* whole-struct compare below: no stale bytes past the NULs */
            strlcpy(g->kind, kind ? kind : "note", sizeof(g->kind));
            strlcpy(g->title, title ? title : "", sizeof(g->title));
            strlcpy(g->text, text, sizeof(g->text));
            strlcpy(g->value, value ? value : "", sizeof(g->value));
            strlcpy(g->label, lbl ? lbl : "", sizeof(g->label));
            strlcpy(g->detail, detail && detail[0] ? detail : text, sizeof(g->detail));
            strlcpy(g->icon, icon ? icon : "", sizeof(g->icon));
            g->progress = cJSON_IsNumber(prog) ? (int8_t)prog->valueint : -1;
            cJSON *st = nullptr;
            cJSON *stats = cJSON_GetObjectItem(it, "stats");
            cJSON_ArrayForEach(st, stats) {
                if (g->nstats >= 3) {
                    break;
                }
                const char *l = cJSON_GetStringValue(cJSON_GetObjectItem(st, "l"));
                const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(st, "v"));
                cJSON *sp = cJSON_GetObjectItem(st, "p");
                if (!v) {
                    continue;
                }
                strlcpy(g->stats[g->nstats].l, l ? l : "", sizeof(g->stats[0].l));
                strlcpy(g->stats[g->nstats].v, v, sizeof(g->stats[0].v));
                g->stats[g->nstats].p = cJSON_IsNumber(sp) ? (int8_t)sp->valueint : -1;
                g->nstats++;
            }
        }
        s_glance_ok_us = now_us();
        /* Publish only a real change, so the page does not rebuild (and flicker) every poll. */
        int cur = s_glance_active;
        bool same = s_glance_n[cur] == n &&
                    memcmp(s_glance[cur], s_glance[fill], sizeof(muse_glance_t) * (size_t)n) == 0;
        s_glance_n[fill] = n;
        if (!same) {
            s_glance_active = fill;
            s_glance_gen = s_glance_gen + 1;
        }
        cJSON_Delete(root);
    } else {
        ESP_LOGW(TAG, "glance: GET failed (err %s, HTTP %d)", esp_err_to_name(err), status);
    }
    heap_caps_free(resp);
}

/* Idle and awake (or on USB): every 3 min, the first a few seconds after the link is up. */
static void glance_tick(void)
{
    int64_t now = now_us();
    if (now < s_glance_next_us || !muse_settings_relay_configured() || s_turn.phase != P_IDLE ||
        s_resting.load() || !muse_wifi_connected()) {
        return;
    }
    s_glance_next_us = now + (s_glance_n[s_glance_active] ? 180 : 60) * 1000000LL;   /* empty: retry sooner */
    glance_fetch();
}

extern "C" void muse_notif_poll_now(void)
{
    s_notif_poll_us = 0;
    s_notif_gap_us = 0;
    s_glance_next_us = 0;
}

extern "C" int muse_glance_age_s(void)
{
    return s_glance_ok_us ? (int)((now_us() - s_glance_ok_us) / 1000000LL) : -1;
}

extern "C" uint32_t muse_glance_gen(void)
{
    return s_glance_gen;
}

extern "C" void muse_glance_refresh(void)
{
    if (now_us() + 2 * 1000000LL < s_glance_next_us - 150 * 1000000LL) {
        return;   /* fetched under 30 s ago: fresh enough */
    }
    s_glance_next_us = 0;
}

extern "C" int muse_glance_count(void)
{
    return s_glance_n[s_glance_active];
}

extern "C" bool muse_glance_get(int i, muse_glance_t *out)
{
    int a = s_glance_active;
    if (i < 0 || i >= s_glance_n[a]) {
        return false;
    }
    *out = s_glance[a][i];
    return true;
}

/* Idle poll: only when a poll can act on the result (idle, awake, un-resting, hermes mode). */
static void notif_tick(void)
{
    if (!s_notif_auto || !muse_settings_relay_configured() || !muse_settings_agent_hermes()) {
        return;
    }
    int64_t now = now_us();
    /* Poll while asleep-but-not-resting (USB power, or within the rest grace): a nudge can
     * still wake the screen. On battery the rest gate below holds the poll off by design --
     * the radio costs more than the nudge is worth there; it lands on the next wake. */
    if (s_notif_pending && now > s_notif_until) {
        s_notif_pending = false;   /* never tapped: stop blocking; a face tap is a pet again */
        ESP_LOGI(TAG, "notif: %s went stale (untapped)", s_notif_label);
    }
    if (s_notif_pending) {
        return;   /* one at a time: a nudge on screen waits for the tap */
    }
    if (muse_input_dnd()) {
        return;   /* face-down: leave them on the relay; picking it up polls at once */
    }
    if (now < s_notif_poll_us || now < s_notif_gap_us || s_turn.phase != P_IDLE || s_resting.load() ||
        !muse_wifi_connected() || !muse_hatch_configured()) {
        return;
    }
    /* Awake: the snappy cadence. Asleep: slower, so leaving the radio up to catch a nudge is
     * not a battery tax (a nudge then wakes the screen within ~a minute). */
    s_notif_poll_us = now + (muse_state_asleep() ? 60 * 1000000LL : NOTIF_POLL_US);
    rel_notif_t n;
    bool reachable = false;
    if (!poll_notification(&n, &reachable)) {
        if (!reachable) {
            s_notif_fail_streak = s_notif_fail_streak < 4 ? s_notif_fail_streak + 1 : 4;
            s_notif_poll_us = now + NOTIF_POLL_US * (1LL << s_notif_fail_streak);
        }
        return;
    }
    s_notif_fail_streak = 0;
    notif_present(&n);
}

/* Called from the face-tap. -> true when a nudge was waiting (its agent was opened), false to
 * let the tap be an ordinary pet. Opening switches the SLOT and loads the profile for ALT, so
 * the turn you speak next reaches the exact agent that notified. */
extern "C" bool muse_notif_open_pending(void)
{
    if (!s_notif_pending) {
        return false;
    }
    s_notif_pending = false;
    if (s_notif_default) {
        muse_settings_set_agent(MUSE_AGENT_HERMES);
    } else {
        muse_settings_set_agent_profile(s_notif_agent);
        muse_settings_set_agent(MUSE_AGENT_HERMES_ALT);
    }
    muse_ui_agent_switch(true);
    muse_state_set_caption("OPENED %s", s_notif_label);
    muse_state_poke();
    ESP_LOGI(TAG, "notif: opened '%s' (%s)", s_notif_label, s_notif_default ? "hermes" : "alt");
    return true;
}

/* The face acts out "working" from the moment a turn waits for its reply until the
 * reply's own [react] mood owns it (first audio). The blend system re-fires the same mood
 * every 1.2 s, so it reads as one sustained state rather than a one-shot reaction.
 * "scan" is supported by every face (muse_pixel, golem, stone); no new art. */
static void think_tick(void)
{
    if (s_turn.phase != P_WAIT_FINAL && s_turn.phase != P_WAIT_REPLY) {
        return;
    }
    if (s_marks[M_AUDIO]) {
        return;   /* speech started: the reply's mood is in charge now */
    }
    int64_t now = now_us();
    if (now - s_think_poke_us >= 1200000) {
        s_think_poke_us = now;
        muse_ui_react("scan");
    }
}

static void hatch_task(void *arg)
{
    (void)arg;
    for (;;) {
        cmd_t cmd;
        /* Resting, the socket's keepalive (PING_US) needs no quicker polls.
         * Resting unconnected, Wi-Fi may nap (muse_wifi_nap): wait for a
         * turn or for waking (CMD_WAKE) instead of watching it. */
        int wait_ms = hermes_mode() ? (s_turn.phase != P_IDLE ? 2 : 100)
                    : !s_connected ? (s_resting ? -1 : 200) : s_turn.phase != P_IDLE ? 2 : s_resting ? 500 : 20;
        TickType_t wait = wait_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);
        if (xQueueReceive(s_cmds, &cmd, wait) == pdTRUE) {
            handle(cmd);
            while (xQueueReceive(s_cmds, &cmd, 0) == pdTRUE) {
                handle(cmd);
            }
        }
        think_tick();   /* the working face, both backends */
        notif_tick();
        notif_hist_tick();   /* the inbox's fetch/cancel, when the settings page asked */
        glance_tick();       /* the idle screen's cards */
        if (hermes_mode()) {
            /* Look up the Muse VM now, while there is RAM for the account API's TLS: later, with Hermes in use,
             * it can't be done, and switching to Muse would find no credentials ("Can't reach Muse's server"). */
            static int64_t s_vm_try_us;
            if (!s_vm.vm_token && s_turn.phase == P_IDLE && muse_hatch_configured() && muse_wifi_connected() &&
                now_us() >= s_vm_try_us) {
                char err[48] = "";
                bool got = resolve_vm(err, sizeof(err));
                ESP_LOGI(TAG, "Muse VM lookup while on Hermes: %s (int free %uK)", got ? "ok" : err,
                         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
                s_vm_try_us = now_us() + 30 * 1000000LL;
            }
            if (s_turn.hermes) {
                hermes_step();   /* only its own backend steps a turn; a live flip waits for CMD_AGENT */
            }
            continue;
        }
        /* Hermes's note buffer (160 KB) waits for its next turn instead of sitting in Muse's way. */
        if (s_hbuf && s_turn.phase == P_IDLE) {
            heap_caps_free(s_hbuf);
            s_hbuf = nullptr;
        }
        heal_check();
        /* A network change (or loss) leaves the old socket dead without telling us: drop it now and
         * reconnect as soon as Wi-Fi is back, rather than discovering it a turn or a minute later. */
        static bool s_wifi_was = true;
        bool wifi_now = muse_wifi_connected();
        if (wifi_now != s_wifi_was) {
            s_wifi_was = wifi_now;
            ESP_LOGI(TAG, "Wi-Fi %s: %s the Muse link", wifi_now ? "joined" : "lost", s_connected ? "dropping" : "retrying");
            if (s_turn.phase != P_IDLE) {
                turn_fail("WI-FI CHANGED");
            }
            if (s_connected) {
                disconnect("Wi-Fi changed");
            }
            s_auto_next_us = 0;
            s_auto_backoff_us = AUTO_RETRY_MIN_US;
        }
        if (!s_connected) {
            if (!muse_wifi_connected()) {
                s_auto_next_us = 0;
                s_auto_backoff_us = AUTO_RETRY_MIN_US;
            } else if (muse_hatch_configured() && now_us() >= s_auto_next_us) {
                if (ensure_connected()) {
                    s_auto_next_us = INT64_MAX;   /* until it drops */
                    s_auto_backoff_us = AUTO_RETRY_MIN_US;
                } else {
                    s_auto_next_us = now_us() + s_auto_backoff_us;
                    s_auto_backoff_us = s_auto_backoff_us * 2 < AUTO_RETRY_MAX_US ? s_auto_backoff_us * 2
                                                                                   : AUTO_RETRY_MAX_US;
                }
            }
            continue;
        }

        /* Resting, Wi-Fi may nap; don't wait for the server to go quiet. */
        if (s_resting && !muse_wifi_connected()) {
            drop_connection("Wi-Fi down");
            continue;
        }
        if (!poll_socket()) {
            drop_connection("receive failed");
            continue;
        }
        bool sent = true;
        if (s_turn.phase == P_LISTEN && !s_turn.hermes) {
            sent = VOICE_NOTE ? record_note() : pump_mic();
        }
        if (!sent) {
            drop_connection("send failed");
            continue;
        }
        if (s_turn.phase == P_WAIT_REPLY) {
            start_tts();
            decode();
        }
        if (!s_connected) {
            continue;
        }
        check_turn();

        int64_t t = now_us();
        if (t - s_conn.last_rx_us > DEAD_US) {
            drop_connection("server went quiet");
        } else if (s_turn.phase == P_IDLE && t - s_conn.last_use_us > IDLE_CLOSE_US) {
            disconnect("idle");
            muse_hatch_report(MUSE_HATCH_UNTESTED, "");
            s_auto_next_us = INT64_MAX;   /* the next turn connects */
        } else if (t - s_conn.last_ping_us > PING_US) {
            s_conn.last_ping_us = t;
            if (!ws_send_ping(s_conn.tls)) {
                drop_connection("ping failed");
            }
        }
    }
}

/* ---- Public API ---- */

static void post(cmd_type_t type, uint32_t gen)
{
    if (s_cmds) {
        cmd_t cmd{ type, gen, nullptr };
        xQueueSend(s_cmds, &cmd, pdMS_TO_TICKS(100));
    }
}

static void drain_out(void)
{
    static EXT_RAM_BSS_ATTR int16_t junk[256];
    while (xStreamBufferReceive(s_out, junk, sizeof(junk), 0)) {
    }
}

extern "C" void muse_hatch_start(void)
{
    if (s_cmds) {
        return;
    }
    cJSON_Hooks hooks = { json_alloc, heap_caps_free };
    cJSON_InitHooks(&hooks);
    /* Keep the command/event queues out of internal DRAM, which Wi-Fi, BLE and
     * mbedTLS can exhaust: when the allocation failed here hatch_task never
     * started and every turn was silently dropped. Fall back to internal RAM
     * on parts without usable PSRAM. */
    s_cmds = xQueueCreateWithCaps(16, sizeof(cmd_t), MALLOC_CAP_SPIRAM);
    if (!s_cmds) {
        s_cmds = xQueueCreate(16, sizeof(cmd_t));
    }
    s_events = xQueueCreateWithCaps(16, sizeof(ev_t), MALLOC_CAP_SPIRAM);
    if (!s_events) {
        s_events = xQueueCreate(16, sizeof(ev_t));
    }
    s_in = xStreamBufferCreateWithCaps(IN_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_out = xStreamBufferCreateWithCaps(OUT_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_turn.chunk = static_cast<uint8_t *>(psram_alloc(DICT_CHUNK_BYTES + sizeof(MUSE_HATCH_NOTE_TAIL)));
    s_turn.mp3 = static_cast<uint8_t *>(psram_alloc(MP3_BUF));
    s_turn.note = VOICE_NOTE ? static_cast<uint8_t *>(psram_alloc(NOTE_PART_BYTES)) : nullptr;
    s_turn.texts = static_cast<char *>(psram_alloc(MAX_MSGS * TEXT_MAX));   /* captions just stay untimed without it */
    s_pcm = static_cast<int16_t *>(psram_alloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t)));
    s_pcm16 = static_cast<int16_t *>(psram_alloc((MINIMP3_MAX_SAMPLES_PER_FRAME + 8) * sizeof(int16_t)));
    bool streams_ok = true;
    for (auto &s : s_streams) {
        s.line = static_cast<char *>(psram_alloc(NDJSON_LINE_MAX));
        s.cap = NDJSON_LINE_MAX;
        streams_ok = streams_ok && s.line != nullptr;
    }
    s_turn.tts_msg = -1;
    /* Stack in PSRAM: TLS, Noise and the MP3 decoder (~16 KB of scratch) all run here. */
    if (!s_cmds || !s_events || !s_in || !s_out || !s_turn.chunk || !s_turn.mp3 || (VOICE_NOTE && !s_turn.note) || !s_pcm || !s_pcm16 ||
        !streams_ok ||
        xTaskCreatePinnedToCoreWithCaps(hatch_task, "muse_chat", 48 * 1024, nullptr, 5, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "start failed");
    }
}

extern "C" void muse_hatch_chat_connect(void)
{
    post(CMD_CONNECT, 0);
}

extern "C" void muse_hatch_chat_agent_changed(void)
{
    post(CMD_AGENT, 0);
}

extern "C" void muse_hatch_chat_forget(void)
{
    post(CMD_FORGET, 0);
}

extern "C" bool muse_hatch_ready(void)
{
    if (hermes_mode()) {
        return s_cmds && muse_wifi_connected();
    }
    return s_cmds && muse_hatch_configured() && muse_wifi_connected();
}

extern "C" void muse_hatch_turn_begin(void)
{
    uint32_t gen = ++s_gen;
    xStreamBufferReset(s_in);
    drain_out();
    post(CMD_BEGIN, gen);
}

extern "C" void muse_hatch_turn_audio(const int16_t *pcm, size_t frames)
{
    size_t bytes = frames * sizeof(int16_t);
    if (xStreamBufferSend(s_in, pcm, bytes, 0) != bytes) {
        ESP_LOGW(TAG, "mic backlog full, dropped audio");
    }
}

/* Both ends move whole frames, so the buffer never splits one. */
extern "C" size_t muse_hatch_turn_audio_wait(const int16_t *pcm, size_t frames, int wait_ms)
{
    return xStreamBufferSend(s_in, pcm, frames * sizeof(int16_t), pdMS_TO_TICKS(wait_ms)) / sizeof(int16_t);
}

extern "C" void muse_hatch_turn_end(void)
{
    post(CMD_END, s_gen.load());
}

extern "C" void muse_hatch_turn_cancel(void)
{
    uint32_t gen = s_gen.load();
    ++s_gen;
    post(CMD_CANCEL, gen);
    drain_out();
}

extern "C" void muse_hatch_set_resting(bool resting)
{
    if (s_resting.exchange(resting) && !resting) {
        post(CMD_WAKE, 0);
    }
}

extern "C" void muse_hatch_text_turn(char *text)
{
    cmd_t cmd{ CMD_TEXT, 0, text };
    if (!s_cmds || !muse_hatch_configured()) {
        muse_hatch_console("error", "MUSE NOT SET UP", nullptr);
        free(text);
    } else if (xQueueSend(s_cmds, &cmd, pdMS_TO_TICKS(1000)) != pdTRUE) {
        muse_hatch_console("error", "BUSY", nullptr);
        free(text);
    }
}

extern "C" void muse_hatch_text_cancel(void)
{
    post(CMD_TEXT_CANCEL, 0);
}

/* Turns relay task polling/injection on or off at runtime (bench: "task.auto off"). */
extern "C" void muse_hatch_notif_auto(bool on)
{
    s_notif_auto = on;
}

extern "C" muse_hatch_ev_t muse_hatch_turn_event(char *text, size_t cap)
{
    ev_t ev;
    while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
        if (ev.gen == s_gen.load()) {
            strlcpy(text, ev.text, cap);
            return ev.type;
        }
    }
    return MUSE_HATCH_EV_NONE;
}

extern "C" bool muse_hatch_turn_caption(size_t played, char *out, size_t cap)
{
    /* The message being spoken: the last one whose speech has started. */
    const msg_t *m = nullptr;
    const char *text = nullptr;
    for (int i = 0; i < s_turn.nmsgs && s_turn.texts; i++) {
        const msg_t &c = s_turn.msgs[i];
        if (c.tts >= TTS_ACTIVE && c.len && c.pcm_start <= played) {
            m = &c;
            text = s_turn.texts + i * TEXT_MAX;
        }
    }
    if (!m) {
        /* Nothing said yet: the reply's opening page, to read while the speech is on its way. */
        for (int i = 0; i < s_turn.nmsgs && s_turn.texts; i++) {
            if (s_turn.msgs[i].len) {
                return muse_hatch_caption_at(s_turn.texts + i * TEXT_MAX, 0, out, cap);
            }
        }
        return false;
    }
    size_t len = strlen(text);
    uint32_t frames = m->pcm_frames ? m->pcm_frames : (uint32_t)(len * MIC_RATE / SPEECH_CHARS_PER_S);
    size_t at = frames ? (size_t)((uint64_t)(played - m->pcm_start) * len / frames) : 0;
    if (at >= len) {
        at = len ? len - 1 : 0;
    }

    return muse_hatch_caption_at(text, at, out, cap);
}

extern "C" size_t muse_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms)
{
    return xStreamBufferReceive(s_out, pcm, frames * sizeof(int16_t), pdMS_TO_TICKS(wait_ms)) / sizeof(int16_t);
}

/* Bench test: decodes the embedded test_reply.mp3 exactly as a reply is decoded. */
static size_t mp3_selftest(int16_t **pcm_out)
{
    extern const uint8_t mp3_start[] asm("_binary_test_reply_mp3_start");
    extern const uint8_t mp3_end[] asm("_binary_test_reply_mp3_end");
    size_t len = mp3_end - mp3_start;
    mp3dec_t *dec = (mp3dec_t *)psram_alloc(sizeof(mp3dec_t));
    int16_t *pcm = (int16_t *)psram_alloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    size_t cap = MIC_RATE * 10;
    int16_t *out = (int16_t *)psram_alloc(cap * sizeof(int16_t));
    if (!dec || !pcm || !out) {
        free(dec);
        free(pcm);
        free(out);
        return 0;
    }
    mp3dec_init(dec);
    resampler_t rs;
    int rate = 0, frames = 0;
    size_t off = 0, n = 0;
    int64_t t0 = now_us();
    while (off < len) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, mp3_start + off, len - off, pcm, &info);
        if (!info.frame_bytes) {
            break;
        }
        off += info.frame_bytes;
        if (!samples) {
            continue;
        }
        frames++;
        if (info.channels == 2) {
            for (int k = 0; k < samples; k++) {
                pcm[k] = (pcm[2 * k] + pcm[2 * k + 1]) / 2;
            }
        }
        if (rate != info.hz) {
            rate = info.hz;
            resampler_init(&rs, info.hz, MIC_RATE);
        }
        if (n + (size_t)samples * MIC_RATE / rate + 2 > cap) {
            break;
        }
        n += resample(&rs, pcm, samples, out + n);
    }
    ESP_LOGI(TAG, "mp3 selftest: %u bytes, %d frames at %d Hz -> %u samples (%.2f s) in %lld ms",
             (unsigned)len, frames, rate, (unsigned)n, n / (double)MIC_RATE, (now_us() - t0) / 1000);
    free(dec);
    free(pcm);
    *pcm_out = out;
    return n;
}

struct selftest_t {
    TaskHandle_t caller;
    int16_t *pcm;
    size_t n;
};

/* minimp3 wants ~16 KB of stack, more than the voice task has. */
extern "C" size_t muse_hatch_mp3_selftest(int16_t **pcm_out)
{
    selftest_t st = { xTaskGetCurrentTaskHandle(), nullptr, 0 };
    auto body = [](void *arg) {
        auto *st = (selftest_t *)arg;
        st->n = mp3_selftest(&st->pcm);
        xTaskNotifyGive(st->caller);
        vTaskSuspend(NULL);   /* the caller deletes it, which frees the PSRAM stack */
    };
    TaskHandle_t task;
    if (xTaskCreatePinnedToCoreWithCaps(body, "mp3_selftest", 32 * 1024, &st, 5, &task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        return 0;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDeleteWithCaps(task);
    *pcm_out = st.pcm;
    return st.n;
}

/* ---------------------------------------------------------------------------------------
 * Hermes link health, for the UI. The two agents are independent transports: Hermes talks
 * to the user's Mac relay, Muse to Meta's cloud. The status display used to show only
 * MUSE's link, so a Muse outage read as "Can't connect" even while Hermes kept working
 * (verified on hardware: a full Hermes turn ran with Muse offline). These let the UI show
 * each link on its own; nothing here makes a network call -- it records what the turn path
 * already saw, so the cost is zero when no Hermes turn is happening.
 * ------------------------------------------------------------------------------------- */
static volatile int s_hermes_link;          /* 0 unknown, 1 ok, 2 failed */
static volatile int64_t s_hermes_ok_us;     /* when a Hermes reply last succeeded */

extern "C" void muse_hatch_hermes_link_note(bool ok)
{
    s_hermes_link = ok ? 1 : 2;
    if (ok) {
        s_hermes_ok_us = esp_timer_get_time();
    }
}

/* 0 = no Hermes configured / not the active agent, 1 = reachable, 2 = failed recently. */
extern "C" int muse_hatch_hermes_link(void)
{
    if (!muse_settings_relay_configured() || !muse_settings_agent_hermes()) {
        return 0;
    }
    if (s_hermes_link == 2 && s_hermes_ok_us
        && (esp_timer_get_time() - s_hermes_ok_us) > 10 * 60 * 1000000LL) {
        return 1;   /* stale failure: stop reporting a fault we have not seen for 10 min */
    }
    return s_hermes_link;
}

static void bench_net_task(void *arg)
{
    int kb = (int)(intptr_t)arg;
    char url[MUSE_RELAY_URL_MAX + 16];
    snprintf(url, sizeof(url), "%s/bench", hermes_remote_url());
    size_t n = (size_t)kb * 1024;
    char *body = static_cast<char *>(psram_alloc(n ? n : 1));
    wifi_ps_type_t ps = WIFI_PS_NONE;
    esp_wifi_get_ps(&ps);
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    relay_tls(&cfg);
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 20000;
    esp_http_client_handle_t h = body ? esp_http_client_init(&cfg) : nullptr;
    if (h) {
        memset(body, 'x', n);
        int64_t t0 = now_us();
        esp_err_t err = esp_http_client_open(h, (int)n);
        int64_t t1 = now_us();
        int w = err == ESP_OK ? esp_http_client_write(h, body, (int)n) : -1;
        int64_t t2 = now_us();
        int64_t hdr = w >= 0 ? esp_http_client_fetch_headers(h) : -1;
        int64_t t3 = now_us();
        printf("@net %s%s: open+tls %lld ms, upload %d KB in %lld ms (%.1f KB/s), reply %d after %lld ms, ps=%d\n",
               err == ESP_OK && w >= 0 && hdr >= 0 ? "ok" : "failed", cfg.cert_pem ? " (pinned)" : " (bundle)",
               (long long)(t1 - t0) / 1000, kb,
               (long long)(t2 - t1) / 1000, (t2 - t1) > 0 ? kb * 1e6 / (double)(t2 - t1) : 0.0,
               esp_http_client_get_status_code(h), (long long)(t3 - t2) / 1000, (int)ps);
        esp_http_client_cleanup(h);
    } else {
        printf("@net failed: no memory\n");
    }
    fflush(stdout);
    heap_caps_free(body);
    vTaskDeleteWithCaps(nullptr);
}

void muse_hatch_bench_net(int kb, int prio)
{
    /* on the turn's core, by default at its priority too (hatch_task, 5) */
    xTaskCreatePinnedToCoreWithCaps(bench_net_task, "bench_net", 16 * 1024, (void *)(intptr_t)kb, prio > 0 ? prio : 5, nullptr, 0,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
