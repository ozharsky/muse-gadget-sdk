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
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

extern "C" {
#include "cJSON.h"
#include "minimp3.h"
#include "muse_account_api.h"
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

/* The relay task currently injected as a Hermes turn (empty id = none), declared here
 * because turn_done()/turn_fail() below record the outcome. task_finish_if_done() closes
 * the board card once the turn is over -- without this an injected task sits in `running`
 * forever, and without the `replied` flag a FAILED turn would be recorded as a success. */
static char s_injected_id[96];
static char s_injected_chain[160];      /* carried so a reply's [task:] can continue the chain */
static int  s_injected_hops;            /* the injected task's hop count */
static bool s_injected_busy;            /* the injected turn has actually left P_IDLE */
static bool s_injected_replied;         /* the turn produced a reply (not a failure) */


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
#define TASK_TEXT_MAX 1024                 /* the injected relay-task text cap */
#define MP3_BUF (512 * 1024)
#define MP3_HOLD (1441 + 4)               /* the largest MP3 frame and the next header */
#define MP3_POLL_ROOM (SCRATCH + 8192)     /* stop reading the socket below this much MP3 room */
#define IN_BYTES (MIC_RATE * 2 * 8)        /* 8 s of mic backlog while connecting */
#define OUT_BYTES (MIC_RATE * 2 * 2)       /* 2 s of decoded reply */
#define EV_TEXT 72
#define TEXT_MAX 1024                      /* a message's text, for captions timed to its speech */
#define SPEECH_CHARS_PER_S 14              /* until the speech's length is known */
#define TEXT_CHARS_PER_S 16                /* speaker off: reading pace, a little over speech */
#define TEXT_HOLD_S 2                      /* speaker off: how long a message's last lines stay up */

#define PING_US (20 * 1000000LL)
#define DEAD_US (60 * 1000000LL)           /* nothing from the server, pongs included */
#define IDLE_CLOSE_US (10 * 60 * 1000000LL)
#define AUTO_RETRY_MIN_US (5 * 1000000LL)  /* connecting without a turn, after a failure */
#define AUTO_RETRY_MAX_US (120 * 1000000LL)
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

static void forget_vm(void)
{
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
    int rc = muse_hatch_api_find_vm(token, want, &s_vm);
    if (rc == 0) {
        ESP_LOGI(TAG, "VM %s (%s)", s_vm.vm_id, s_vm.vm_name);
        free(token);
        return true;
    }
    /* The hatch slot may hold an identity-class token (the pairing's SDK token), which the
     * account API refuses as a Bearer. The pairing ALSO delivered the real access token —
     * the app layer stores + refreshes it: ask there once before giving up. */
    if (rc == MUSE_HATCH_API_AUTH) {
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
    int64_t t0 = now_us();
    if (esp_tls_conn_new_sync(s_host, strlen(s_host), NOISE_PORT, &cfg, c.tls) != 1) {
        snprintf(err, err_cap, "Can't reach %s", s_host);
        return false;
    }
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
    return CONFIG_MUSE_HERMES_URL[0] && muse_settings_agent_hermes();
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
            char detail[48] = "Connected to ";
            strlcat(detail, s_vm.vm_name[0] ? s_vm.vm_name : s_vm.vm_id, sizeof(detail));
            muse_hatch_report(MUSE_HATCH_REACHABLE, detail);
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
    ESP_LOGW(TAG, "connect failed: %s (int free %uK, largest %uK)", err,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    muse_hatch_report(MUSE_HATCH_UNREACHABLE, err);
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

static uint8_t *s_hbuf;      /* HERMES_MAX_BYTES of 16 kHz PCM, PSRAM, allocated on first use */

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

static void turn_finish(void)
{
    turn_reset_streams();
    if (s_hstream.h) {   /* a Hermes streaming turn in flight: close it */
        esp_http_client_close(s_hstream.h);
        esp_http_client_cleanup(s_hstream.h);
        s_hstream.h = nullptr;
        heap_caps_free(s_hstream.line);
        s_hstream.line = nullptr;
    }
    s_turn.phase = P_IDLE;
    s_turn.dict_id = s_turn.chat_id = 0;
    s_turn.tts_msg = -1;
    s_turn.silent = false;
    s_turn.mp3_len = 0;
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
    /* Remember whether an injected task's turn actually produced a reply, so the board
     * card is not closed as a success when the turn failed or was interrupted. */
    if (s_injected_id[0] && complete) {
        s_injected_replied = true;
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
    if (hermes_mode()) {
        if (!s_hbuf) {
            s_hbuf = static_cast<uint8_t *>(psram_alloc(HERMES_MAX_BYTES));
        }
        if (!s_hbuf) {
            turn_fail("OUT OF MEMORY");
            return;
        }
        s_turn.pcm_bytes = 0;
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

    if (!strcmp(event, "agent.status") || !strcmp(event, "task.status")) {
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
/* Google Trust Services GTS Root R1 (self-signed, valid to 2036). The server sends it
 * cross-signed by the old GlobalSign Root CA, which ESP-IDF's bundle lacks, so pin the root. */
static const char GTS_ROOT_R1[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw\n"
    "CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
    "MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw\n"
    "MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp\n"
    "Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA\n"
    "A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo\n"
    "27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w\n"
    "Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw\n"
    "TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl\n"
    "qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH\n"
    "szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8\n"
    "Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk\n"
    "MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92\n"
    "wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p\n"
    "aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN\n"
    "VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID\n"
    "AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E\n"
    "FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb\n"
    "C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe\n"
    "QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy\n"
    "h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4\n"
    "7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J\n"
    "ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef\n"
    "MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/\n"
    "Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT\n"
    "6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ\n"
    "0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm\n"
    "2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb\n"
    "bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
    "-----END CERTIFICATE-----\n";

struct tts_buf_t {
    char *p;
    size_t len, cap;
    bool over;
};

static esp_err_t tts_http_evt(esp_http_client_event_t *e)
{
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

/* Fetches speech for text and queues its MP3 for decode(). False leaves nothing queued. */
static bool fetch_tts(const char *text)
{
    const size_t CAP = 900 * 1024;
    char *body = static_cast<char *>(psram_alloc(TEXT_MAX * 2 + 256));
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
        size_t n = snprintf(body, 128, "{\"input\":{\"text\":\"");
        for (const char *c = text; *c && n < TEXT_MAX * 2 - 8; c++) {
            if (*c == '"' || *c == '\\') {
                body[n++] = '\\';
                body[n++] = *c;
                billed++;
            } else if (*c == '*' || *c == '#') {
                continue;   /* markdown: not worth saying */
            } else if ((uint8_t)*c < 0x20) {
                body[n++] = ' ';
                billed++;
            } else {
                body[n++] = *c;
                if (((uint8_t)*c & 0xC0) != 0x80) {
                    billed++;   /* a character, not each of its UTF-8 bytes */
                }
            }
        }
        snprintf(body + n, 200, "\"},\"voice\":{\"languageCode\":\"%.*s\",\"name\":\"%s\"},"
                 "\"audioConfig\":{\"audioEncoding\":\"MP3\",\"speakingRate\":%.2f}}", lang_len, voice, voice, hermes_mode() ? 1.0 : CONFIG_MUSE_TTS_RATE_PCT / 100.0);
    }
    {
        static const char URL[] = "https://texttospeech.googleapis.com/v1/text:synthesize?key=" CONFIG_MUSE_TTS_GOOGLE_KEY;
        tts_buf_t buf = { resp, 0, CAP, false };
        esp_http_client_config_t cfg = {};
        cfg.url = URL;
        cfg.method = HTTP_METHOD_POST;
        cfg.cert_pem = GTS_ROOT_R1;
        cfg.timeout_ms = 8000;   /* this task is blocked meanwhile; if Google is slow, show the text rather than wait */
        cfg.event_handler = tts_http_evt;
        cfg.user_data = &buf;
        esp_http_client_handle_t h = esp_http_client_init(&cfg);
        if (!h) {
            goto out;
        }
        esp_http_client_set_header(h, "Content-Type", "application/json");
        esp_http_client_set_post_field(h, body, strlen(body));
        int64_t t0 = now_us();
        esp_err_t err = esp_http_client_perform(h);
        int status = esp_http_client_get_status_code(h);
        esp_http_client_cleanup(h);
        if (err != ESP_OK || status != 200 || buf.over) {
            ESP_LOGW(TAG, "tts: request failed (err %s, HTTP %d%s)", esp_err_to_name(err), status, buf.over ? ", too big" : "");
            if (status != 200 && buf.len) {
                buf.p[buf.len < 300 ? buf.len : 299] = 0;   /* Google's error text */
                ESP_LOGW(TAG, "tts: %s", buf.p);
            }
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
        s_turn.mp3_len = 0;
        s_turn.mp3_ended = false;
        tts_data(reinterpret_cast<uint8_t *>(a + 1), mp3);
        s_turn.mp3_ended = true;
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
    return CONFIG_MUSE_HERMES_URL[0] && muse_settings_agent_hermes() ? 1 : 0;
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

/* ---- Relay tasks: [task:<agent> ...] tags in a reply ----
 *
 * An agent hands work to the other one with a [task:hermes ...] / [task:muse ...]
 * tag in its reply. The tag is pulled out before the reply is spoken or captioned
 * (strip_reply_tags, called from the main loop right before start_tts) and the
 * extracted task is posted back to the relay from task_tick(). A reply with no
 * tag is fine. The reply text is what this fixes; the relay must keep the tag out
 * of the speech it synthesizes (the firmware plays relay-made MP3, so it can't
 * un-say a tag that was already spoken). */
struct rel_task_t {
    char id[96];
    char to[16];
    char text[1024];
    char chain[160];
    char from[64];
    int hops;
    bool valid;
};

/* The first tag found in the current reply, waiting for task_tick() to post it. */
static EXT_RAM_BSS_ATTR rel_task_t s_reply_task;

/* Removes every [task:<agent> <body>] tag from `text` in place and records the
 * first in s_reply_task. Returns true if one was found. */
static bool strip_task_tags(char *text)
{
    bool found = false;
    char *w = text;
    for (char *r = text; *r;) {
        if (!strncmp(r, "[task:", 6)) {
            const char *agent = nullptr;
            char *p = r + 6;
            if (!strncmp(p, "hermes", 6)) {
                agent = "hermes";
                p += 6;
            } else if (!strncmp(p, "muse", 4)) {
                agent = "muse";
                p += 4;
            }
            char *ws = p;
            while (*ws == ' ' || *ws == '\t') {
                ws++;
            }
            char *end = (ws != p) ? strchr(ws, ']') : nullptr;
            if (end && end > ws) {   /* \s+ then a non-empty body */
                if (!found) {
                    s_reply_task.valid = true;
                    strlcpy(s_reply_task.to, agent, sizeof(s_reply_task.to));
                    size_t n = (size_t)(end - ws);
                    if (n >= sizeof(s_reply_task.text)) {
                        n = sizeof(s_reply_task.text) - 1;
                    }
                    memcpy(s_reply_task.text, ws, n);
                    s_reply_task.text[n] = '\0';
                    /* Provenance for the relay's idempotency key. Without a chain the
                     * relay keys every device-posted task on ":1", so the SECOND
                     * handoff (and every one after) collides with the first and
                     * silently reuses that card. Carry the chain of the task this
                     * turn is answering so the hop cap can brake a ping-pong;
                     * task_post() sends hops + 1 on the same chain. */
                    if (s_injected_id[0] && s_injected_chain[0]) {
                        strlcpy(s_reply_task.chain, s_injected_chain, sizeof(s_reply_task.chain));
                        s_reply_task.hops = s_injected_hops;     /* task_post() adds 1 */
                    } else {
                        uint32_t h = 2166136261u;                /* FNV-1a: a fresh chain */
                        for (size_t k = 0; k < n; k++) {
                            h = (h ^ (uint8_t)s_reply_task.text[k]) * 16777619u;
                        }
                        snprintf(s_reply_task.chain, sizeof(s_reply_task.chain), "dev%08lx", (unsigned long)h);
                        s_reply_task.hops = 0;
                    }
                } else {
                    ESP_LOGI(TAG, "task: extra [task:%s ...] tag in one reply ignored", agent);
                }
                found = true;
                r = end + 1;
                while (*r == ' ') {
                    r++;   /* swallow the gap the tag left behind */
                }
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
    if (found) {
        while (w > text && (w[-1] == ' ' || w[-1] == '\t')) {
            *--w = '\0';   /* a tag removed from the end: don't leave the gap */
        }
    }
    return found;
}

/* Strips task tags from every reply message of the current turn, before speech. */
static void strip_reply_tags(void)
{
    if (!s_turn.texts) {
        return;
    }
    for (int i = 0; i < s_turn.nmsgs; i++) {
        char *t = s_turn.texts + i * TEXT_MAX;
        if (!t[0]) {
            continue;
        }
        size_t before = s_turn.msgs[i].len;
        if (strip_task_tags(t)) {
            s_turn.msgs[i].len = (uint32_t)strlen(t);
            ESP_LOGI(TAG, "task: stripped tag from reply (msg %d: %u -> %u chars)", i,
                     (unsigned)before, (unsigned)s_turn.msgs[i].len);
        }
        (void)before;   /* used only by the log above (compiled out on host) */
    }
}

static void start_tts(void)
{
    if (s_turn.tts_msg >= 0) {
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
        if (s_turn.texts) {
            last_reply_add(i, s_turn.texts + i * TEXT_MAX);
        }
        m.pcm_start = s_turn.pcm_out;
        m.pcm_frames = (uint32_t)(m.len * MIC_RATE / TEXT_CHARS_PER_S);
        m.tts = TTS_ACTIVE;
        s_turn.tts_msg = i;
        s_turn.silent = true;
        if (CONFIG_MUSE_TTS_GOOGLE_KEY[0] && s_turn.texts && muse_settings_speaker_on() &&
            fetch_tts(s_turn.texts + i * TEXT_MAX)) {
            /* Speech fetched: decode() plays it, captions following. */
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
        turn_done(false);
        return;
    }
    if (!s_turn.nmsgs) {
        /* A typed turn waits as long as the agent says it's working. */
        if (t - s_turn.chat_us > (text ? TEXT_REPLY_TIMEOUT_US : REPLY_TIMEOUT_US) && !(text && s_turn.agent_busy)) {
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

/* True if the home relay's port answers within 1.5 s (a plain TCP connect to "http://a.b.c.d:port"). */
static bool hermes_home_reachable(void)
{
    char host[64];
    int port = 80;
    const char *p = strstr(CONFIG_MUSE_HERMES_URL, "://");
    p = p ? p + 3 : CONFIG_MUSE_HERMES_URL;
    size_t n = strcspn(p, ":/");
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
    return ok && err == 0;
}

/* A Hermes TEXT turn (an injected task): same streamed exchange as the voice path,
 * but the text rides the query string and there is no PCM body. Mirrors
 * hermes_request_stream() so the reply streams, TTS plays and [task:...] strips as usual. */
static bool hermes_request_stream_text(const char *text, size_t tlen)
{
    char url[TASK_TEXT_MAX * 3 + 128];
    bool remote = false;
    const char *base = CONFIG_MUSE_HERMES_URL;
    if (CONFIG_MUSE_HERMES_URL_REMOTE[0] && !hermes_home_reachable()) {
        base = CONFIG_MUSE_HERMES_URL_REMOTE;
        remote = true;
    }
    /* percent-encode (cap the text so the query can never overrun the URL buffer) */
    char enc[TASK_TEXT_MAX * 3 + 1];
    size_t e = 0;
    if (tlen > TASK_TEXT_MAX) {
        tlen = TASK_TEXT_MAX;
    }
    for (size_t i = 0; i < tlen && e + 4 < sizeof(enc); i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            enc[e++] = (char)c;
        } else if (c == ' ') {
            enc[e++] = '+';
        } else {
            snprintf(&enc[e], 4, "%%%02X", c);
            e += 3;
        }
    }
    enc[e] = '\0';
    snprintf(url, sizeof(url), "%s/turn?stream=1&text=%s%s", base, enc, agent_profile_query());

    char *line = static_cast<char *>(psram_alloc(160 * 1024));
    if (!line) {
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

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 100000;
    cfg.buffer_size = 4096;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        heap_caps_free(line);
        s_hstream.line = nullptr;
        turn_fail("OUT OF MEMORY");
        return false;
    }
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_header(h, "X-Gadget-Token", CONFIG_MUSE_HERMES_TOKEN);
    int64_t t0 = now_us();
    esp_err_t err = esp_http_client_open(h, 0);   /* no body: the text is in the query */
    if (err == ESP_OK) {
        (void)esp_http_client_fetch_headers(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "task: hermes text open failed (%s)", esp_err_to_name(err));
        esp_http_client_cleanup(h);
        heap_caps_free(line);
        s_hstream.line = nullptr;
        turn_fail("CAN'T REACH HERMES");
        return false;
    }
    ESP_LOGI(TAG, "task: hermes text turn opened (%lld ms)", (long long)((now_us() - t0) / 1000));
    s_hstream.h = h;
    s_turn.chat_us = s_turn.last_event_us = now_us();
    return true;
}

/* One turn against the relay (compat, non-stream): PCM up, {"heard","reply"} back. Kept for
 * a fallback if ?stream=1 ever misbehaves; not called by the streaming path. */
[[maybe_unused]] static bool hermes_request(void)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/turn%s", CONFIG_MUSE_HERMES_URL, agent_profile_query());
    bool remote = false;
    if (CONFIG_MUSE_HERMES_URL_REMOTE[0] && !hermes_home_reachable()) {
        snprintf(url, sizeof(url), "%s/turn%s", CONFIG_MUSE_HERMES_URL_REMOTE, agent_profile_query());
        remote = true;
        ESP_LOGI(TAG, "hermes: home relay unreachable, using the remote address");
    }
    const size_t RESP_CAP = 160 * 1024;   /* the reply, plus a base64 JPEG of up to ~100 KB */
    char *resp = static_cast<char *>(psram_alloc(RESP_CAP));
    if (!resp) {
        turn_fail("OUT OF MEMORY");
        return false;
    }
    tts_buf_t buf = { resp, 0, RESP_CAP - 1, false };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 100000;   /* this task waits; the relay gives Hermes 80 s */
    cfg.event_handler = tts_http_evt;
    cfg.user_data = &buf;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        heap_caps_free(resp);
        turn_fail("OUT OF MEMORY");
        return false;
    }
    esp_http_client_set_header(h, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(h, "X-Gadget-Token", CONFIG_MUSE_HERMES_TOKEN);
    esp_http_client_set_post_field(h, reinterpret_cast<const char *>(s_hbuf), s_turn.pcm_bytes);
    esp_err_t err = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    resp[buf.len] = 0;
    cJSON *j = (err == ESP_OK && status == 200) ? cJSON_Parse(resp) : nullptr;
    const char *heard = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "heard")) : nullptr;
    const char *reply = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "reply")) : nullptr;
    const char *react = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "react")) : nullptr;
    char *image = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "image_b64")) : nullptr;
    bool ok = false;
    if (s_turn.gen != s_gen.load()) {
        turn_finish();   /* cancelled while waiting */
    } else if (!j) {
        ESP_LOGW(TAG, "hermes: request failed (err %s, HTTP %d)", esp_err_to_name(err), status);
        turn_fail(err != ESP_OK ? "CAN'T REACH HERMES" : "HERMES ERROR");
    } else if (!reply || !reply[0]) {
        turn_fail("DIDN'T CATCH THAT");
    } else {
        ESP_LOGI(TAG, "hermes heard: \"%s\"", heard ? heard : "");
        emit(MUSE_HATCH_EV_HEARD, heard ? heard : "");
        msg_t &m = s_turn.msgs[0];
        m = msg_t{};
        strlcpy(m.id, "hermes", sizeof(m.id));
        s_turn.nmsgs = 1;
        s_turn.chat_posted = true;
        s_turn.chat_us = s_turn.last_event_us = s_turn.last_content_us = now_us();
        s_turn.phase = P_WAIT_REPLY;
        append_text(m, reply);
        show_reply_start(m);
        message_done(0, reply);
        strlcpy(s_hreact, react ? react : "", sizeof(s_hreact));   /* start_tts() plays it as the speech starts */
        if (image && image[0]) {
            const char *code = "", *msg = "";
            size_t n = b64_decode_inplace(image, strlen(image));
            if (!image_fetch_buffer(reinterpret_cast<uint8_t *>(image), n, -1, image_done, nullptr, &code, &msg)) {
                ESP_LOGW(TAG, "hermes: image not shown: %s (%s)", msg, code);
            }
        }
        ok = true;
    }
    cJSON_Delete(j);
    heap_caps_free(resp);
    return ok;
}

static void hermes_react_show(void)
{
    if (s_hreact[0]) {
        muse_ui_react(s_hreact);
        s_hreact[0] = '\0';
    }
}

/* NDJSON line -> one action. Lines: heard / status / tts (+ the final reply/react/image line). */
static void hermes_line(cJSON *j)
{
    if (s_turn.gen != s_gen.load()) {
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
static bool hermes_request_stream(void)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/turn?stream=1%s", CONFIG_MUSE_HERMES_URL, agent_profile_query());
    bool remote = false;
    if (CONFIG_MUSE_HERMES_URL_REMOTE[0] && !hermes_home_reachable()) {
        snprintf(url, sizeof(url), "%s/turn?stream=1%s", CONFIG_MUSE_HERMES_URL_REMOTE, agent_profile_query());
        remote = true;
        ESP_LOGI(TAG, "hermes: home relay unreachable, using the remote address");
    }
    char *line = static_cast<char *>(psram_alloc(160 * 1024));   /* a long sentence's b64 MP3 can be ~60-100K */
    if (!line) {
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

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 100000;
    cfg.buffer_size = 4096;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        heap_caps_free(line);
        s_hstream.line = nullptr;
        turn_fail("OUT OF MEMORY");
        return false;
    }
    esp_http_client_set_header(h, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(h, "X-Gadget-Token", CONFIG_MUSE_HERMES_TOKEN);
    esp_http_client_set_post_field(h, reinterpret_cast<const char *>(s_hbuf), s_turn.pcm_bytes);
    int64_t t0 = now_us();
    esp_err_t err = esp_http_client_open(h, s_turn.pcm_bytes);   /* headers only */
    if (err == ESP_OK) {
        /* write the PCM body: esp_http_client_open sends the headers, not the payload */
        int written = esp_http_client_write(h, reinterpret_cast<const char *>(s_hbuf), s_turn.pcm_bytes);
        if (written < 0) {
            err = ESP_FAIL;
        } else {
            ESP_LOGI(TAG, "hermes stream: body written (%d bytes, %lld ms)", written,
                     (long long)((now_us() - t0) / 1000));
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hermes stream: open/write failed (%s) after %lld ms", esp_err_to_name(err),
                 (long long)((now_us() - t0) / 1000));
        esp_http_client_cleanup(h);
        heap_caps_free(line);
        s_hstream.line = nullptr;
        turn_fail("CAN'T REACH HERMES");
        return false;
    }
    int64_t hdr = esp_http_client_fetch_headers(h);
    ESP_LOGI(TAG, "hermes stream: open ok, headers %lld bytes in %lld ms", (long long)hdr,
             (long long)((now_us() - t0) / 1000));
    s_hstream.h = h;
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
    int n;
    while ((n = esp_http_client_read(hs.h, reinterpret_cast<char *>(chunk), sizeof(chunk))) > 0) {
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
    if (hs.ended || n <= 0) {   /* ended, or the server closed (read returned 0/-1) */
        int status = esp_http_client_get_status_code(hs.h);
        esp_http_client_close(hs.h);
        esp_http_client_cleanup(hs.h);
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
        size_t room = HERMES_MAX_BYTES - s_turn.pcm_bytes;
        size_t got = room ? xStreamBufferReceive(s_in, s_hbuf + s_turn.pcm_bytes, room & ~(size_t)1, 0) : 0;
        s_turn.pcm_bytes += got;
        if (!s_turn.end_requested && s_turn.pcm_bytes < HERMES_MAX_BYTES) {
            return;
        }
        mark(M_RELEASE);
        if (s_turn.pcm_bytes < MIC_RATE * 2 * 3 / 10) {
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
        strip_reply_tags();   /* never speak a [task:...] tag */
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

/* ---- Relay tasks: pick up queued work and post tagged work back ----
 *
 * Every TASK_POLL_US, when the session is idle (a live turn holds the relay
 * connection), the agent is Hermes, Wi-Fi is up, the screen is awake and the
 * relay is configured, the gadget GETs its queue and injects the first ready
 * task as a normal typed turn. A turn with a [task:<agent> ...] tag in its reply
 * has the tag stripped (strip_reply_tags, pre-speech) and the task posted back
 * with the hop count bumped. The whole feature is behind s_task_auto (off with
 * the console's "task.auto off"), and every network call runs on the session
 * task so the reply/TTS path is untouched. */
#define TASK_POLL_US (15 * 1000000LL)

static bool s_task_auto = true;          /* runtime guard: no reflash to turn it off */
static int64_t s_task_poll_us;           /* when the queue may next be polled */
static int s_task_fail_streak;           /* consecutive unreachable polls, for the backoff */

/* The relay's base URL, the remote address when home is unreachable (TLS bundle). */
static const char *task_relay_url(bool *remote)
{
    *remote = false;
    if (CONFIG_MUSE_HERMES_URL_REMOTE[0] && !hermes_home_reachable()) {
        *remote = true;
        return CONFIG_MUSE_HERMES_URL_REMOTE;
    }
    return CONFIG_MUSE_HERMES_URL;
}

/* GET /tasks?to=<agent>&status=ready -> the first ready task, or false. *reachable
 * says the relay answered at all (HTTP 200), so the caller can back off only when it
 * really is unreachable -- an empty queue is a healthy poll, not a failure. */
static bool poll_task(const char *agent, rel_task_t *out, bool *reachable)
{
    out->valid = false;
    if (reachable) {
        *reachable = false;
    }
    char url[192];
    snprintf(url, sizeof(url), "%s/tasks?to=%s&status=ready%s", CONFIG_MUSE_HERMES_URL, agent,
             agent_profile_query());
    bool remote = false;
    if (CONFIG_MUSE_HERMES_URL_REMOTE[0] && !hermes_home_reachable()) {
        remote = true;
        snprintf(url, sizeof(url), "%s/tasks?to=%s&status=ready%s", CONFIG_MUSE_HERMES_URL_REMOTE,
                 agent, agent_profile_query());
    }
    const size_t CAP = 16 * 1024;
    char *resp = static_cast<char *>(psram_alloc(CAP));
    if (!resp) {
        return false;
    }
    tts_buf_t buf = { resp, 0, CAP - 1, false };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 2500;   /* a stalled poll holds the session task; fail fast */
    cfg.event_handler = tts_http_evt;
    cfg.user_data = &buf;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        heap_caps_free(resp);
        return false;
    }
    esp_http_client_set_header(h, "X-Gadget-Token", CONFIG_MUSE_HERMES_TOKEN);
    esp_err_t err = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    bool ok = false;
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "task: GET /tasks failed (err %s, HTTP %d)", esp_err_to_name(err), status);
    } else {
        if (reachable) {
            *reachable = true;
        }
        resp[buf.len] = '\0';
        cJSON *root = cJSON_Parse(resp);
        cJSON *tasks = root ? cJSON_GetObjectItem(root, "tasks") : nullptr;
        cJSON *first = cJSON_IsArray(tasks) ? cJSON_GetArrayItem(tasks, 0) : nullptr;
        if (cJSON_IsObject(first)) {
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(first, "id"));
            const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(first, "text"));
            const char *chain = cJSON_GetStringValue(cJSON_GetObjectItem(first, "chain"));
            const char *from = cJSON_GetStringValue(cJSON_GetObjectItem(first, "from"));
            cJSON *hops = cJSON_GetObjectItem(first, "hops");
            if (id && id[0] && text && text[0]) {
                strlcpy(out->id, id, sizeof(out->id));
                strlcpy(out->text, text, sizeof(out->text));
                strlcpy(out->chain, chain ? chain : "", sizeof(out->chain));
                strlcpy(out->from, from ? from : "", sizeof(out->from));
                out->hops = cJSON_IsNumber(hops) ? hops->valueint : 0;
                out->valid = true;
                ok = true;
            }
        }
        cJSON_Delete(root);
    }
    heap_caps_free(resp);
    return ok;
}

/* POST /task/take {id}: claims the task so the relay doesn't hand it out again. */
static void task_take(const char *id)
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
    char url[160];
    bool remote = false;
    snprintf(url, sizeof(url), "%s/task/take", task_relay_url(&remote));
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 8000;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h) {
        esp_http_client_set_header(h, "Content-Type", "application/json");
        esp_http_client_set_header(h, "X-Gadget-Token", CONFIG_MUSE_HERMES_TOKEN);
        esp_http_client_set_post_field(h, json, strlen(json));
        esp_err_t err = esp_http_client_perform(h);
        int status = esp_http_client_get_status_code(h);
        esp_http_client_cleanup(h);
        if (err != ESP_OK || status != 200) {
            ESP_LOGW(TAG, "task: POST /task/take failed (err %s, HTTP %d)", esp_err_to_name(err), status);
        }
    }
    cJSON_free(json);
}

/* POST /task/done {id, result}: closes an injected task once its turn has replied.
 * Without this an injected task stays `running` on the board forever. */
static void task_finish(const char *id, const char *result)
{
    cJSON *body = cJSON_CreateObject();
    if (!body) {
        return;
    }
    cJSON_AddStringToObject(body, "id", id);
    if (result && *result) {
        cJSON_AddStringToObject(body, "result", result);
    }
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) {
        return;
    }
    char url[160];
    bool remote = false;
    snprintf(url, sizeof(url), "%s/task/done", task_relay_url(&remote));
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 8000;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h) {
        esp_http_client_set_header(h, "Content-Type", "application/json");
        esp_http_client_set_header(h, "X-Gadget-Token", CONFIG_MUSE_HERMES_TOKEN);
        esp_http_client_set_post_field(h, json, strlen(json));
        esp_err_t err = esp_http_client_perform(h);
        int status = esp_http_client_get_status_code(h);
        esp_http_client_cleanup(h);
        if (err != ESP_OK || status != 200) {
            ESP_LOGW(TAG, "task: POST /task/done failed (err %s, HTTP %d)", esp_err_to_name(err), status);
        } else {
            ESP_LOGI(TAG, "task: closed %s on the board", id);
        }
    }
    cJSON_free(json);
}

/* POST /task/requeue {id}: returns a running card to ready so the next poll retries it.
 * Used when an injected turn produced no reply, so a failure is neither lost nor recorded
 * as a success. */
static void task_requeue(const char *id)
{
    cJSON *body = cJSON_CreateObject();
    if (!body) {
        return;
    }
    cJSON_AddStringToObject(body, "id", id);
    cJSON_AddStringToObject(body, "reason", "injected turn produced no reply");
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) {
        return;
    }
    char url[160];
    bool remote = false;
    snprintf(url, sizeof(url), "%s/task/requeue", task_relay_url(&remote));
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 8000;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h) {
        esp_http_client_set_header(h, "Content-Type", "application/json");
        esp_http_client_set_header(h, "X-Gadget-Token", CONFIG_MUSE_HERMES_TOKEN);
        esp_http_client_set_post_field(h, json, strlen(json));
        esp_err_t err = esp_http_client_perform(h);
        int status = esp_http_client_get_status_code(h);
        esp_http_client_cleanup(h);
        if (err != ESP_OK || status != 200) {
            ESP_LOGW(TAG, "task: POST /task/requeue failed (err %s, HTTP %d)", esp_err_to_name(err), status);
        } else {
            ESP_LOGI(TAG, "task: returned %s to the queue for a retry", id);
        }
    }
    cJSON_free(json);
}

/* POST /task: hands the other agent a task from a reply's [task:...] tag. */
static void task_post(const rel_task_t *t)
{
    cJSON *body = cJSON_CreateObject();
    if (!body) {
        return;
    }
    char from[32];
    strlcpy(from, muse_settings_agent_hermes() ? "hermes" : "muse", sizeof(from));
    cJSON_AddStringToObject(body, "to", t->to);
    cJSON_AddStringToObject(body, "text", t->text);
    /* Which relay profile originated it, so the board/agents.json can tell the slots
     * apart. The task bridge only knows muse|hermes (spec section 6), so this rides as
     * metadata, not as a new target. */
    char prof[MUSE_AGENT_PROFILE_MAX + 1];
    muse_settings_agent_profile(prof);
    if (prof[0]) {
        cJSON_AddStringToObject(body, "profile", prof);
    }
    cJSON_AddNumberToObject(body, "hops", t->hops + 1);
    cJSON_AddStringToObject(body, "chain", t->chain);
    cJSON_AddStringToObject(body, "from", from);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) {
        return;
    }
    char url[160];
    bool remote = false;
    snprintf(url, sizeof(url), "%s/task", task_relay_url(&remote));
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 8000;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h) {
        esp_http_client_set_header(h, "Content-Type", "application/json");
        esp_http_client_set_header(h, "X-Gadget-Token", CONFIG_MUSE_HERMES_TOKEN);
        esp_http_client_set_post_field(h, json, strlen(json));
        esp_err_t err = esp_http_client_perform(h);
        int status = esp_http_client_get_status_code(h);
        esp_http_client_cleanup(h);
        if (err != ESP_OK || status != 200) {
            ESP_LOGW(TAG, "task: POST /task failed (err %s, HTTP %d)", esp_err_to_name(err), status);
        } else {
            ESP_LOGI(TAG, "task: posted to %s (hops %d)", t->to, t->hops + 1);
        }
    }
    cJSON_free(json);
}

/* The agent this gadget is, for the queue query. */
static const char *task_agent(void)
{
    return muse_settings_agent_hermes() ? "hermes" : "muse";
}

/* Posts a reply's extracted task, if the tag produced one. Clears it either way. */
static void task_flush_reply(void)
{
    if (s_reply_task.valid) {
        if (s_reply_task.text[0]) {
            task_post(&s_reply_task);
        } else {
            ESP_LOGW(TAG, "task: [task:%s ...] tag had no text; not posted", s_reply_task.to);
        }
        s_reply_task.valid = false;
    }
}

/* Close the board card for the injected task once its turn has run and returned to idle.
 * s_injected_busy guards the gap between queueing the turn and it actually starting,
 * so a task is never closed on the same tick it was injected.
 * A turn that FAILED or was interrupted must not be recorded as a completed task: it is
 * put back to `ready` (reclaim) so the next poll retries it, with the attempt counted in
 * the log. Without this split, any failure path looked identical to success. */
static void task_finish_if_done(void)
{
    if (!s_injected_id[0]) {
        return;
    }
    if (s_turn.phase != P_IDLE) {
        s_injected_busy = true;          /* the injected turn is running */
        return;
    }
    if (!s_injected_busy) {
        return;                          /* not started yet */
    }
    if (s_injected_replied) {
        task_finish(s_injected_id, "");
    } else {
        /* Failed, interrupted, or timed out: release it back to the queue. */
        ESP_LOGW(TAG, "task: turn for %s produced no reply; returning it to the queue", s_injected_id);
        task_requeue(s_injected_id);
    }
    s_injected_id[0] = '\0';
    s_injected_chain[0] = '\0';
    s_injected_busy = false;
    s_injected_replied = false;
}

/* Injects a relay task as a Hermes text turn. Runs the same reply pipeline as a voice
 * turn (stream, TTS, tag strip) but with no microphone. Called from task_tick() on the
 * session task, so it must not block: it only opens the exchange.
 * Returns false WITHOUT side effects if the turn cannot start -- the caller then leaves
 * the card in `ready` for the next poll, which is why the card is taken only on success. */
static bool task_inject_turn(const rel_task_t *t)
{
    if (!hermes_mode()) {
        return false;                 /* tasks only ride the Hermes relay path */
    }
    if (s_turn.phase != P_IDLE) {
        return false;
    }
    if (!turn_start(s_gen.load(), true)) {   /* true = a text turn: the text reply budget, not the voice cap */
        return false;
    }
    size_t n = strlen(t->text);
    if (!hermes_request_stream_text(t->text, n)) {
        return false;
    }
    s_turn.phase = P_WAIT_REPLY;
    /* Remember the whole card so the reply can close it and continue its chain. */
    strlcpy(s_injected_id, t->id, sizeof(s_injected_id));
    strlcpy(s_injected_chain, t->chain, sizeof(s_injected_chain));
    s_injected_hops = t->hops;
    s_injected_busy = false;
    s_injected_replied = false;
    ESP_LOGI(TAG, "task: text turn opened (%u bytes, chain %s, hops %d)", (unsigned)n,
             s_injected_chain[0] ? s_injected_chain : "(new)", s_injected_hops);
    return true;
}

static bool notif_is_pending(void);   /* defined with the notification block below */

/* One poll tick: only idle, awake, un-resting, and past the cadence. */
static void task_tick(void)
{
    if (!s_task_auto || !CONFIG_MUSE_HERMES_URL[0] || !muse_settings_agent_hermes()) {
        return;
    }
    int64_t now = now_us();
    if (now < s_task_poll_us || s_turn.phase != P_IDLE || s_resting.load() || muse_state_asleep() ||
        notif_is_pending() ||   /* a nudge is on screen: don't clobber it with a task's reply */
        !muse_wifi_connected() || !muse_hatch_configured()) {
        return;
    }
    s_task_poll_us = now + TASK_POLL_US;
    rel_task_t task;
    bool reachable = false;
    if (!poll_task(task_agent(), &task, &reachable)) {
        if (!reachable) {
            /* The relay did not answer: widen the gap (30s -> 60 -> 120 -> 240 cap) so an
             * unreachable Mac cannot stall the session task every 15 s. */
            s_task_fail_streak = s_task_fail_streak < 4 ? s_task_fail_streak + 1 : 4;
            s_task_poll_us = now + TASK_POLL_US * (1LL << s_task_fail_streak);
            ESP_LOGW(TAG, "task: poll unreachable %d time(s); next try in %llds",
                     s_task_fail_streak,
                     (long long)(TASK_POLL_US * (1LL << s_task_fail_streak) / 1000000));
        }
        return;
    }
    s_task_fail_streak = 0;
    /* Inject FIRST, take only on success. Taking moves the card ready->running, and the
     * poll only lists `ready`, so a card taken and then failed to inject is stranded in
     * `running` with no path back. */
    if (!task_inject_turn(&task)) {
        ESP_LOGW(TAG, "task: turn start refused (busy?); left %s in ready for the next poll", task.id);
        return;
    }
    task_take(task.id);
    ESP_LOGI(TAG, "task: injected \"%s\" (id %s, hops %d, from %s)", task.text, task.id, task.hops,
             task.from[0] ? task.from : "?");
}

/* ---- Notifications: the REVERSE channel (relay -> device, unprompted) ----
 *
 * The task path is poll-for-WORK: the relay hands the gadget a job. This is the other
 * direction: the relay hands the gadget a LINE to present on its own, labelled with the EXACT
 * agent that raised it. When one arrives and the gadget is idle, it looks up, plays its mood,
 * chirps and prints the line; tapping the face OPENS that agent -- switching to the right slot
 * (and, for an ALT profile, loading its name) so the next thing you say reaches the agent that
 * notified, never a generic default. Behind the same s_task_auto guard as the task path.
 *
 * The slot ("which agent") is decided on the RELAY (notifications.resolve_agent) and arrives as
 * is_default: True -> the Hermes slot, False -> the ALT slot loaded with `agent`. */
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
    static mp3dec_t dec;
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

extern "C" void muse_ui_agent_switch(bool hermes);   /* muse_ui.h has no C++ guards */

/* GET /notifications -> the newest unread nudge, or false. *reachable: the relay answered. */
static bool poll_notification(rel_notif_t *out, bool *reachable)
{
    out->valid = false;
    if (reachable) {
        *reachable = false;
    }
    char url[160];
    bool remote = false;
    snprintf(url, sizeof(url), "%s/notifications", task_relay_url(&remote));
    const size_t CAP = 8 * 1024;
    char *resp = static_cast<char *>(psram_alloc(CAP));
    if (!resp) {
        return false;
    }
    tts_buf_t buf = { resp, 0, CAP - 1, false };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 2500;
    cfg.event_handler = tts_http_evt;
    cfg.user_data = &buf;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        heap_caps_free(resp);
        return false;
    }
    esp_http_client_set_header(h, "X-Gadget-Token", CONFIG_MUSE_HERMES_TOKEN);
    esp_err_t err = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
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
    char url[160];
    bool remote = false;
    snprintf(url, sizeof(url), "%s/notifications/ack", task_relay_url(&remote));
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    if (remote) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 8000;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h) {
        esp_http_client_set_header(h, "Content-Type", "application/json");
        esp_http_client_set_header(h, "X-Gadget-Token", CONFIG_MUSE_HERMES_TOKEN);
        esp_http_client_set_post_field(h, json, strlen(json));
        (void)esp_http_client_perform(h);
        esp_http_client_cleanup(h);
    }
    free(json);
}

/* Show one nudge on the glass: look up, its mood, a chirp, and the line with a tap hint. */
static void notif_present(const rel_notif_t *n)
{
    muse_state_set_asleep(false);   /* wake the screen: the nudge is meant to be SEEN */
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

static bool notif_is_pending(void)
{
    return s_notif_pending;
}

/* Idle poll: only when a poll can act on the result (idle, awake, un-resting, hermes mode). */
static void notif_tick(void)
{
    if (!s_task_auto || !CONFIG_MUSE_HERMES_URL[0] || !muse_settings_agent_hermes()) {
        return;
    }
    int64_t now = now_us();
    /* Poll while ASLEEP too -- that is the point: a nudge should wake the screen itself. The
     * power cost is bounded (a poll only every 15 s, and only when a poll could act). */
    if (s_notif_pending && now > s_notif_until) {
        s_notif_pending = false;   /* never tapped: stop blocking; a face tap is a pet again */
        ESP_LOGI(TAG, "notif: %s went stale (untapped)", s_notif_label);
    }
    if (s_notif_pending) {
        return;   /* one at a time: a nudge on screen waits for the tap */
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
        /* Relay tasks: poll the queue and hand back any tag a finished reply left.
         * Self-gated to idle/awake/15 s; a no-op unless a relay URL is set. */
        task_tick();
        notif_tick();
        task_finish_if_done();
        task_flush_reply();
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
            hermes_step();
            continue;
        }
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
        if (s_turn.phase == P_LISTEN) {
            sent = VOICE_NOTE ? record_note() : pump_mic();
        }
        if (!sent) {
            drop_connection("send failed");
            continue;
        }
        if (s_turn.phase == P_WAIT_REPLY) {
            strip_reply_tags();   /* never speak a [task:...] tag */
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
    static int16_t junk[256];
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
    for (auto &s : s_streams) {
        s.line = static_cast<char *>(psram_alloc(NDJSON_LINE_MAX));
        s.cap = NDJSON_LINE_MAX;
    }
    s_turn.tts_msg = -1;
    /* Stack in PSRAM: TLS, Noise and the MP3 decoder (~16 KB of scratch) all run here. */
    if (!s_cmds || !s_events || !s_in || !s_out || !s_turn.chunk || !s_turn.mp3 || (VOICE_NOTE && !s_turn.note) || !s_pcm || !s_pcm16 ||
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
extern "C" void muse_hatch_task_auto(bool on)
{
    s_task_auto = on;
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
    if (!CONFIG_MUSE_HERMES_URL[0] || !muse_settings_agent_hermes()) {
        return 0;
    }
    if (s_hermes_link == 2 && s_hermes_ok_us
        && (esp_timer_get_time() - s_hermes_ok_us) > 10 * 60 * 1000000LL) {
        return 1;   /* stale failure: stop reporting a fault we have not seen for 10 min */
    }
    return s_hermes_link;
}
