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

#include "muse_lcd_bands.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_mem.h"
#include "muse_diag.h"     /* the black box: restart reasons survive a reboot */

typedef struct {
    const uint8_t *data;   /* NULL: run call/call_arg instead */
    void (*call)(void *arg);
    void *call_arg;
    int x1, y1, x2, y2;
    bool last;             /* LVGL's last area of the frame */
} band_t;

static const char *TAG = "lcd_bands";

/*
 * A panel write's completion is an interrupt, and an interrupt can go missing:
 * the screen then stops mid-update -- LVGL waits forever for the "flush done"
 * that never comes, and the UI task stalls until the watchdog aborts for a
 * core dump (the L2 freeze). The DMA itself does not drop a transfer, so a
 * missing completion is treated as a lost SIGNAL, not lost pixels.
 *
 * The guard has two levels. A lost signal with the send task still
 * running gets the graceful recovery below (release the waiting flush). A panel
 * driver call that does not RETURN at all means the IDF SPI bus lock is waiting
 * for a completion handoff that will never come (spi_bus_lock_acquire_start
 * takes no timeout by design), which no app-level code can undo: the guard
 * dumps the interrupt state and restarts the board in ~4 s, instead of the
 * 20 s freeze + stall abort seen in the 2026-10-08 coredump (lvgl in
 * wait_for_flushing, lcd_send in dev_wait).
 * The send task's own waits are bounded too, so a lost signal starves it for
 * two seconds, not forever.
 *
 * Fault injection without hardware that actually loses one: ">lcd.swallow" on
 * the bench drops the next completion on purpose (muse_input.c).
 */
#define DONE_GRACE_MS 500        /* no piece queued or finished this long: signal lost */
#define FLUSH_WAIT_MS 1000       /* LVGL's wait for a band's notify: past this, write it off */
#define GUARD_PERIOD_MS 250      /* how often the guard looks */
#define CHUNK_WAIT_MS 2000       /* buffer wait; past recovery, the chain is broken */
#define RUN_WAIT_MS 2000         /* muse_lcd_bands_run: bound its waits too */
#define RECOVER_MAX 3            /* recoveries inside the window before a restart */
#define RECOVER_WINDOW_MS 60000
#define DRIVER_STALL_MS 4000     /* a driver call stuck this long: the IDF bus-lock handoff was lost */

static lv_display_t *s_disp;
static esp_lcd_panel_handle_t s_panel;
static QueueHandle_t s_bands;            /* room for LVGL's band and a call */
static SemaphoreHandle_t s_call_lock;
static SemaphoreHandle_t s_chunk_free;   /* internal buffers not on the wire */
static uint8_t *s_chunk[2];
static size_t s_chunk_bytes;
static int s_chunks_out;                 /* of the band being sent */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_progress_ms;  /* last piece queued or done (ISR writes) */
static volatile bool s_notify_pending;   /* a band-final notify is owed to LVGL */
static volatile uint32_t s_swallow;      /* bench: completions to drop on purpose */
static volatile uint32_t s_notify_drop;  /* bench: flush notifies to drop on purpose */
static uint32_t s_recover_count;
static uint32_t s_recover_first_ms;
static volatile uint32_t s_driver_ms;    /* nonzero while a panel driver call is in progress */
static volatile uint32_t s_wedge_ms;     /* bench: make the next driver call stall this long */

/* Tearing-effect sync + band timings (bench). */
static volatile bool s_te_on;
static volatile int s_te_gpio = -1;
static volatile int64_t s_te_rise, s_te_fall, s_te_period, s_te_blank;
static volatile uint32_t s_band_t0_us;   /* send task writes, ISR reads */
static uint32_t s_band_sum_us, s_band_max_us, s_band_count, s_te_held, s_te_frames;

/*
 * Whole frames, tear-free (set up with the tearing effect, muse_lcd_bands_set_te). A full
 * frame draws in four bands over ~100 ms; sent band by band, every panel refresh in between
 * showed part old frame, part new, so a swipe tore. In this mode each band lands in a copy of
 * the screen (LVGL gets its buffer straight back), and when the frame is complete its changed
 * areas go to the panel in one burst, timed against the scan (fb_send).
 */
#define DIRTY_MAX 8      /* areas kept apart per frame; more become one box */
#define FB_FAST_W 240    /* an area this narrow is sent faster than the panel scans it */
typedef struct {
    int x1, y1, x2, y2;
} rect_t;
static uint8_t *s_fb;                    /* the screen, in the panel's byte order */
static int s_fb_w, s_fb_h;
static SemaphoreHandle_t s_te_scan;      /* given as each scan starts (TE rise) */
static rect_t s_dirty[DIRTY_MAX];
static int s_ndirty;
static volatile int s_unread;            /* bands LVGL has queued that the send task hasn't copied */
static int s_k;                          /* the internal buffer the next piece goes in */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* A piece has gone, or failed to; true if it was the band's last. */
static bool IRAM_ATTR chunk_done(void)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    bool last = s_chunks_out > 0 && --s_chunks_out == 0;
    portEXIT_CRITICAL_SAFE(&s_lock);
    return last;
}

static bool IRAM_ATTR on_chunk_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    bool swallow;
    portENTER_CRITICAL_SAFE(&s_lock);
    swallow = s_swallow > 0;
    if (swallow) {
        s_swallow--;
    }
    portEXIT_CRITICAL_SAFE(&s_lock);
    if (swallow) {
        /* Bench fault injection: act exactly as if this interrupt never fired. */
        return false;
    }
    s_progress_ms = now_ms();
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_chunk_free, &woken);
    bool last = chunk_done();
    if (last && s_fb) {
        uint32_t us = (uint32_t)esp_timer_get_time() - s_band_t0_us;
        portENTER_CRITICAL_SAFE(&s_lock);
        s_band_sum_us += us;
        s_band_count++;
        if (us > s_band_max_us) {
            s_band_max_us = us;
        }
        portEXIT_CRITICAL_SAFE(&s_lock);
        return woken == pdTRUE;
    }
    if (last) {
        bool drop;
        portENTER_CRITICAL_SAFE(&s_lock);
        s_notify_pending = true;   /* owed to LVGL until delivered below */
        drop = s_notify_drop > 0;
        if (drop) {
            s_notify_drop--;
        }
        portEXIT_CRITICAL_SAFE(&s_lock);
        if (drop) {
            /* Bench: the accounting completed but the flush signal is lost -- the
             * exact shape of the 2026-10-08 freeze (lvgl spinning in
             * wait_for_flushing while the counter reads 0). The notify stays
             * pending; the flush-wait backstop must catch it. */
            return woken == pdTRUE;
        }
        bool yield = esp_lv_adapter_display_notify_color_trans_done_from_isr(s_disp);
        uint32_t us = (uint32_t)esp_timer_get_time() - s_band_t0_us;
        portENTER_CRITICAL_SAFE(&s_lock);
        s_notify_pending = false;
        s_band_sum_us += us;
        s_band_count++;
        if (us > s_band_max_us) {
            s_band_max_us = us;
        }
        portEXIT_CRITICAL_SAFE(&s_lock);
        return yield || woken == pdTRUE;
    }
    return woken == pdTRUE;
}

/*
 * LVGL's own flush handshake, replaced with a bounded wait. wait_for_flushing()
 * spins on `while(disp->flushing)` with NO timeout when no flush_wait_cb is set
 * -- if a band's done signal is lost after its counter already read zero, that
 * spin never ends, the UI task stalls, and stall_check aborts for a core dump
 * (the 2026-10-08 freeze: lvgl in wait_for_flushing at lv_refr.c:1458, lcd_send
 * idle, counter 0 -- the guard was blind because nothing looked outstanding).
 *
 * LVGL calls this only when it still believes a flush is in progress, and clears
 * its own flag right after we return -- so this waits, bounded, for the band's
 * final notify to land (s_notify_seq), and gives up after the same grace the
 * counter guard uses. The send task is serialized behind this wait, so declaring
 * it done cannot race a live transfer.
 */
/*
 * Count this recovery against the spree window and restart when they pile up: one
 * lost signal is noise, three in a minute is a display that has stopped keeping
 * up, and a clean reboot beats a screen that keeps stalling. The black box gets
 * the reason, so the next boot names the path that tripped it.
 */
static void spree_trip(const char *reason, const muse_diag_lcd_t *st)
{
    uint32_t now = now_ms();
    if (s_recover_count == 0 || (uint32_t)(now - s_recover_first_ms) > RECOVER_WINDOW_MS) {
        s_recover_count = 1;
        s_recover_first_ms = now;
        return;
    }
    if (++s_recover_count >= RECOVER_MAX) {
        ESP_LOGE(TAG, "%u display recoveries in a minute: restarting rather than limping (%s)",
                 (unsigned)s_recover_count, reason);
        muse_diag_note_fault_restart(reason, st, reason);
        esp_restart();
    }
}

/*
 * LVGL's own flush handshake, replaced with a bounded wait. wait_for_flushing()
 * spins on `while(disp->flushing)` with NO timeout when no flush_wait_cb is set
 * -- if a band's done signal is lost after its counter already read zero, that
 * spin never ends, the UI task stalls, and stall_check aborts for a core dump
 * (the 2026-10-08 freeze: lvgl in wait_for_flushing at lv_refr.c:1458, lcd_send
 * idle, counter 0 -- the guard was blind because nothing looked outstanding).
 *
 * LVGL calls this only when it still believes a flush is in progress, and clears
 * its own flag right after we return -- so this waits, bounded, for the band's
 * final notify to land, and gives up after the same grace the counter guard uses.
 * The send task is serialized behind this wait, so declaring it done cannot race
 * a live transfer.
 */
static void lcd_flush_wait(lv_display_t *disp)
{
    (void)disp;
    int64_t deadline = esp_timer_get_time() + (int64_t)FLUSH_WAIT_MS * 1000;
    for (;;) {
        bool outstanding;
        portENTER_CRITICAL_SAFE(&s_lock);
        /* "flush outstanding" covers both halves of the wait: a band still
         * sending (pieces left) and a band whose last piece is done but whose
         * notify has not been delivered yet. Waiting on the second half alone
         * was the first bug: mid-band it reads false, and the callback would
         * release LVGL while pieces were still on the wire. */
        outstanding = s_unread > 0 || (!s_fb && (s_chunks_out > 0 || s_notify_pending));
        portEXIT_CRITICAL_SAFE(&s_lock);
        if (!outstanding) {
            return;   /* notify arrived (or nothing was owed): LVGL clears its flag */
        }
        if (esp_timer_get_time() >= deadline) {
            break;
        }
        vTaskDelay(1);
    }

    ESP_LOGW(TAG, "flush wait timed out (%d piece(s) outstanding, notify %s): releasing LVGL's wait",
             s_chunks_out, s_notify_pending ? "owed" : "delivered");
    portENTER_CRITICAL_SAFE(&s_lock);
    s_notify_pending = false;   /* written off; a late notify cannot double-deliver */
    /* Known (rare, cosmetic): pieces of this written-off band can still be in flight. If the
     * next band starts before the last one completes, that completion decrements the NEW
     * band's count (line ~353) and LVGL may draw one piece early -- subtle tearing, never a
     * hang (the guards above stay intact). Fix = tag completions with a band generation. */
    portEXIT_CRITICAL_SAFE(&s_lock);
    muse_diag_lcd_t st = {
        .chunks_out = s_chunks_out,
        .recover_count = (int)s_recover_count,
        .driver_ms = s_driver_ms ? (int)(now_ms() - s_driver_ms) : 0,
    };
    muse_diag_note_lcd_fault("lcd_flush_lost", &st);
    spree_trip("lcd_flush_lost", &st);
}

static void lcd_recover(void);
static void te_wait(void);

/*
 * The guard runs on its own timer so it also catches the case where the send
 * task itself is wedged inside the panel driver.
 */
static void lcd_guard(void *arg)
{
    (void)arg;
    uint32_t drv = s_driver_ms;
    if (drv != 0 && (uint32_t)(now_ms() - drv) > DRIVER_STALL_MS) {
        /* A panel driver call has not returned in DRIVER_STALL_MS. That can only be
         * the IDF SPI bus lock waiting for a completion handoff that is not coming
         * (the acquire takes no timeout, so the task is unrecoverable from here).
         * Dump the interrupt state, record what the display looked like in the black
         * box (this reboot pre-empts the 20 s stall abort and its coredump), then
         * reboot in ~4 s. */
        ESP_LOGE(TAG, "panel driver call stuck for %d ms (%d piece(s) outstanding): restarting",
                 DRIVER_STALL_MS, s_chunks_out);
        muse_diag_lcd_t st = {
            .chunks_out = s_chunks_out,
            .recover_count = (int)s_recover_count,
            .driver_ms = DRIVER_STALL_MS,
        };
        muse_diag_note_fault_restart("lcd_driver_stuck", &st, "lcd_driver_stuck");
        esp_intr_dump(stdout);
        fflush(stdout);
        esp_restart();
    }
    lcd_recover();
}

/*
 * A completion went missing. Believe the transfer happened (DMA does not drop
 * one) and only its signal did: re-open the copy buffers, release the LVGL
 * flush that is waiting on it, and let the next frame draw over the glitch.
 * A late interrupt may still arrive and hand back a buffer twice; the counting
 * semaphore caps that at two and the done-count guard keeps it from flushing a
 * band twice.
 *
 * The staleness check is INSIDE the critical section, on the same snapshot the
 * decision acts on: checking outside and acting inside could zero a live band
 * that started between the two, and LVGL would then never hear of that band --
 * the exact freeze this function exists to prevent.
 */
static void lcd_recover(void)
{
    uint32_t now = now_ms();
    int missed;
    portENTER_CRITICAL_SAFE(&s_lock);
    if (s_chunks_out <= 0 || (uint32_t)(now - s_progress_ms) <= DONE_GRACE_MS) {
        portEXIT_CRITICAL_SAFE(&s_lock);
        return;   /* nothing outstanding, or something moved recently: not a loss */
    }
    missed = s_chunks_out;
    s_chunks_out = 0;
    portEXIT_CRITICAL_SAFE(&s_lock);

    UBaseType_t have = uxSemaphoreGetCount(s_chunk_free);
    for (UBaseType_t i = have; i < 2; i++) {
        xSemaphoreGive(s_chunk_free);
    }

    ESP_LOGW(TAG, "panel completion lost (%d piece(s) outstanding): releasing the waiting flush", missed);
    if (!s_fb) {
        lv_display_flush_ready(s_disp);
    }

    muse_diag_lcd_t st = {
        .chunks_out = missed,
        .recover_count = (int)s_recover_count,
        .driver_ms = 0,
    };
    spree_trip("lcd_loss_spree", &st);
}

static esp_err_t queue_band(lv_display_t *disp, esp_lcd_panel_handle_t panel, int x1, int y1, int x2, int y2,
                            const void *data, void *ctx)
{
    (void)panel;
    (void)ctx;
    band_t b = { .data = data, .x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2, .last = lv_display_flush_is_last(disp) };
    portENTER_CRITICAL_SAFE(&s_lock);
    s_unread++;
    portEXIT_CRITICAL_SAFE(&s_lock);
    if (xQueueSend(s_bands, &b, 0) != pdTRUE) {
        portENTER_CRITICAL_SAFE(&s_lock);
        s_unread--;
        portEXIT_CRITICAL_SAFE(&s_lock);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* h rows of x1..x2 from src (rows stride bytes apart) into an internal buffer and onto the
 * wire; the buffer comes back when the piece is done (on_chunk_sent). */
static void put_piece(int x1, int y, int x2, int h, const uint8_t *src, size_t stride)
{
    size_t row = (size_t)(x2 - x1) * 2;
    if (xSemaphoreTake(s_chunk_free, pdMS_TO_TICKS(CHUNK_WAIT_MS)) != pdTRUE) {
        /* A piece's completion never came, so its buffer never returned.
         * Recover once and take again; if even that fails the chain is
         * broken for good -- a clean restart beats a frozen screen. */
        lcd_recover();
        if (xSemaphoreTake(s_chunk_free, pdMS_TO_TICKS(50)) != pdTRUE) {
            ESP_LOGE(TAG, "panel send starved past recovery: restarting");
            esp_restart();
        }
    }
    if (stride == row) {
        memcpy(s_chunk[s_k], src, h * row);
    } else {
        for (int r = 0; r < h; r++) {
            memcpy(s_chunk[s_k] + r * row, src + r * stride, row);
        }
    }
    s_driver_ms = now_ms();
    if (s_wedge_ms) {
        /* Bench: simulate the IDF bus-lock handoff being lost -- the driver call
         * never returns. The guard must reboot in ~4 s, not freeze for 20 s. */
        vTaskDelay(pdMS_TO_TICKS(s_wedge_ms));
        s_wedge_ms = 0;
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x1, y, x2, y + h, s_chunk[s_k]);
    s_driver_ms = 0;
    s_progress_ms = now_ms();
    if (err != ESP_OK) {
        xSemaphoreGive(s_chunk_free);
        if (chunk_done() && !s_fb) {
            lv_display_flush_ready(s_disp);
        }
    }
    s_k ^= 1;
}

static void fb_band(const band_t *b);

/*
 * The panel's draw waits for the piece before to finish, so each piece is
 * copied while the one before is on the wire. Even row counts keep every
 * piece's window starting on an even row, which the CO5300 needs.
 */
static void send_bands(void *arg)
{
    (void)arg;
    band_t b;
    for (;;) {
        xQueueReceive(s_bands, &b, portMAX_DELAY);
        if (!b.data) {
            /* The call travels in the band, not in a global. Callers return as soon
             * as it is queued and pass static storage for call_arg (see
             * muse_lcd_bands_run), so the pointer stays valid however late the send
             * task gets here. It talks to the panel directly, so the driver-stall
             * guard must cover it too. */
            if (b.call) {
                s_driver_ms = now_ms();
                b.call(b.call_arg);
                s_driver_ms = 0;
            }
            continue;
        }
        if (s_fb) {
            fb_band(&b);
            continue;
        }
        if (b.y1 == 0) {
            /* First band of a frame: leave just after the panel restarts its
             * scan, so this frame lands in one refresh. Frames whose bands are
             * mostly unchanged are cheap and arrive late; they wait too -- one
             * refresh of latency, no torn frame. */
            te_wait();
        }
        s_band_t0_us = (uint32_t)esp_timer_get_time();
        size_t row = (size_t)(b.x2 - b.x1) * 2;
        int rows = s_chunk_bytes / row & ~1;
        /* LVGL sends no more until this band is done, so nothing is still
         * counting down. */
        portENTER_CRITICAL_SAFE(&s_lock);
        s_chunks_out = (b.y2 - b.y1 + rows - 1) / rows;   /* see lcd_flush_wait: written-off bands can still decrement this */
        s_unread--;
        portEXIT_CRITICAL_SAFE(&s_lock);
        for (int y = b.y1; y < b.y2; y += rows) {
            int h = b.y2 - y < rows ? b.y2 - y : rows;
            put_piece(b.x1, y, b.x2, h, b.data + (size_t)(y - b.y1) * row, row);
        }
    }
}

lv_display_t *muse_lcd_bands_register(esp_lv_adapter_display_config_t cfg, int lines, size_t chunk_bytes)
{
    s_panel = cfg.panel;
    s_chunk_bytes = chunk_bytes;
    s_fb_w = cfg.profile.hor_res;
    s_fb_h = cfg.profile.ver_res;
    if (xTaskGetCoreID(xTaskGetCurrentTaskHandle()) != MUSE_UI_CORE) {
        ESP_LOGE(TAG, "not pinned to core %d: the panel's SPI interrupt can strand the send task", MUSE_UI_CORE);
    }
    s_bands = xQueueCreate(2, sizeof(band_t));
    s_call_lock = xSemaphoreCreateMutex();
    s_chunk_free = xSemaphoreCreateCounting(2, 2);
    s_chunk[0] = heap_caps_malloc(chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_chunk[1] = heap_caps_malloc(chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_bands || !s_call_lock || !s_chunk_free || !s_chunk[0] || !s_chunk[1] ||
        xTaskCreatePinnedToCore(send_bands, "lcd_send", 2560, NULL, MUSE_UI_PRIORITY + 1, NULL, MUSE_UI_CORE) != pdPASS) {
        return NULL;
    }
    const esp_lcd_panel_io_callbacks_t io_cbs = { .on_color_trans_done = on_chunk_sent };
    esp_lcd_panel_io_register_event_callbacks(cfg.panel_io, &io_cbs, NULL);
    esp_lv_adapter_set_default_display_idf_callback_registration_enabled(false);

    cfg.profile.buffer_height = lines;
    cfg.profile.use_psram = true;
    cfg.profile.require_double_buffer = true;
    s_disp = esp_lv_adapter_register_display(&cfg);
    if (!s_disp) {
        return NULL;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    const esp_lv_adapter_draw_bitmap_callbacks_t draw_cbs = { .custom_draw_bitmap = queue_band };
    esp_lv_adapter_set_draw_bitmap_callbacks(s_disp, &draw_cbs, NULL);
    /* Replace LVGL's untimed `while(disp->flushing)` spin with a bounded wait: a
     * done signal lost after the counter reads zero would otherwise stall the UI
     * task until the watchdog aborts (see lcd_flush_wait). */
    lv_display_set_flush_wait_cb(s_disp, lcd_flush_wait);

    s_progress_ms = now_ms();
    static esp_timer_handle_t s_guard_timer;
    const esp_timer_create_args_t ga = { .callback = lcd_guard, .name = "lcd_guard" };
    if (esp_timer_create(&ga, &s_guard_timer) == ESP_OK) {
        esp_timer_start_periodic(s_guard_timer, GUARD_PERIOD_MS * 1000);
    }
    return s_disp;
}

void muse_lcd_bands_run(void (*fn)(void *arg), void *arg)
{
    if (!s_bands) {
        fn(arg);
        return;
    }
    /* Fire and forget: the call is queued and runs in the send task, in order.
     * `arg` must point at STATIC storage -- these users (brightness/sleep) are
     * small scalars; a stack address dies before the send task gets there. */
    if (xSemaphoreTake(s_call_lock, pdMS_TO_TICKS(RUN_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "panel call busy for %d ms: skipped", RUN_WAIT_MS);
        return;
    }
    const band_t call = { .call = fn, .call_arg = arg };
    if (xQueueSend(s_bands, &call, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "panel call queue full (send task stalled): skipped");
    }
    xSemaphoreGive(s_call_lock);
}

/* The panel toggles this once a refresh: high through the scan, low through
 * vertical blanking (DCS 35h asks for V-blank only). */
static void IRAM_ATTR on_te(void *arg)
{
    (void)arg;
    int64_t t = esp_timer_get_time();
    if (gpio_get_level(s_te_gpio)) {
        if (s_te_rise) {
            s_te_period = t - s_te_rise;
        }
        s_te_rise = t;
        s_te_fall = 0;
        if (s_te_scan) {
            BaseType_t woken = pdFALSE;
            xSemaphoreGiveFromISR(s_te_scan, &woken);
            portYIELD_FROM_ISR(woken);
        }
    } else {
        if (s_te_rise) {
            s_te_blank = t - s_te_rise;
        }
        s_te_fall = t;
    }
}

void muse_lcd_bands_set_te(int gpio)
{
    s_te_gpio = gpio;
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err == ESP_OK) {
        err = gpio_install_isr_service(0);
        err = err == ESP_ERR_INVALID_STATE ? ESP_OK : err;   /* already installed */
    }
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(gpio, on_te, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tearing effect on GPIO%d: %s", gpio, esp_err_to_name(err));
        return;
    }
    s_te_on = true;
    vTaskDelay(pdMS_TO_TICKS(40));   /* let a couple of refreshes measure */
    ESP_LOGI(TAG, "tearing effect on GPIO%d: refresh %d us, blank %d us", gpio,
             (int)s_te_period, (int)s_te_blank);
    if (s_te_period <= 0) {
        return;   /* no refreshes seen: band by band, as before */
    }
    /* Whole frames from here (before LVGL's first, muse_lcd_bands.h). Without the memory,
     * band by band as before. */
    SemaphoreHandle_t scan = xSemaphoreCreateBinary();
    uint8_t *fb = heap_caps_calloc((size_t)s_fb_w * s_fb_h, 2, MALLOC_CAP_SPIRAM);
    if (!scan || !fb) {
        ESP_LOGW(TAG, "no room for whole frames: band by band");
        heap_caps_free(fb);
        if (scan) {
            vSemaphoreDelete(scan);
        }
        return;
    }
    s_te_scan = scan;
    s_fb = fb;
    ESP_LOGI(TAG, "whole frames, sent against the scan");
}

static void fb_merge(void)
{
    for (int i = 1; i < s_ndirty; i++) {
        rect_t *a = &s_dirty[0], *r = &s_dirty[i];
        a->x1 = r->x1 < a->x1 ? r->x1 : a->x1;
        a->y1 = r->y1 < a->y1 ? r->y1 : a->y1;
        a->x2 = r->x2 > a->x2 ? r->x2 : a->x2;
        a->y2 = r->y2 > a->y2 ? r->y2 : a->y2;
    }
    s_ndirty = s_ndirty ? 1 : 0;
}

/*
 * The frame's changed areas, from the screen copy, against the panel's scan (16.7 ms, top to
 * bottom). A narrow area goes faster than the scan: started as the scan starts, the write stays
 * ahead of it and this refresh shows the area whole. A wide one goes slower: it starts once the
 * scan is past its first row, so the scan stays ahead -- this refresh shows the old frame, the
 * next the new one, never a mix. At 80 MHz a full frame takes ~20-35 ms, inside the two
 * refreshes that needs.
 */
static void fb_send(void)
{
    bool wide = false;
    for (int i = 0; i < s_ndirty; i++) {
        wide |= s_dirty[i].x2 - s_dirty[i].x1 > FB_FAST_W;
    }
    if (wide) {
        fb_merge();   /* one box, so one timing covers all of it */
    }
    xSemaphoreTake(s_te_scan, 0);   /* a scan that started before this frame was done doesn't count */
    int64_t t0 = esp_timer_get_time();
    bool synced = xSemaphoreTake(s_te_scan, pdMS_TO_TICKS(25)) == pdTRUE;
    if (synced && wide) {
        int64_t lead = (s_te_period - s_te_blank) * s_dirty[0].y1 / s_fb_h + 1000;   /* +1 ms: late is safe */
        vTaskDelay(pdMS_TO_TICKS((lead + 999) / 1000));
    }
    portENTER_CRITICAL_SAFE(&s_lock);
    s_te_frames++;
    s_te_held += esp_timer_get_time() - t0 > 500;
    portEXIT_CRITICAL_SAFE(&s_lock);

    int pieces = 0;
    for (int i = 0; i < s_ndirty; i++) {
        int rows = s_chunk_bytes / ((size_t)(s_dirty[i].x2 - s_dirty[i].x1) * 2) & ~1;
        pieces += (s_dirty[i].y2 - s_dirty[i].y1 + rows - 1) / rows;
    }
    s_band_t0_us = (uint32_t)esp_timer_get_time();
    portENTER_CRITICAL_SAFE(&s_lock);
    s_chunks_out = pieces;
    portEXIT_CRITICAL_SAFE(&s_lock);
    for (int i = 0; i < s_ndirty; i++) {
        const rect_t *r = &s_dirty[i];
        int rows = s_chunk_bytes / ((size_t)(r->x2 - r->x1) * 2) & ~1;
        for (int y = r->y1; y < r->y2; y += rows) {
            int h = r->y2 - y < rows ? r->y2 - y : rows;
            put_piece(r->x1, y, r->x2, h, s_fb + ((size_t)y * s_fb_w + r->x1) * 2, (size_t)s_fb_w * 2);
        }
    }
    s_ndirty = 0;
}

/* A band into the screen copy; LVGL has its buffer back at once. The frame's last sends it. */
static void fb_band(const band_t *b)
{
    size_t row = (size_t)(b->x2 - b->x1) * 2;
    for (int y = b->y1; y < b->y2; y++) {
        memcpy(s_fb + ((size_t)y * s_fb_w + b->x1) * 2, b->data + (size_t)(y - b->y1) * row, row);
    }
    if (s_ndirty == DIRTY_MAX) {
        fb_merge();
    }
    s_dirty[s_ndirty++] = (rect_t){ b->x1, b->y1, b->x2, b->y2 };
    portENTER_CRITICAL_SAFE(&s_lock);
    s_unread--;
    portEXIT_CRITICAL_SAFE(&s_lock);
    lv_display_flush_ready(s_disp);
    if (b->last) {
        fb_send();
    }
}

/* Waits for the panel to enter blanking (the fall edge), bounded. */
static void te_wait(void)
{
    if (!s_te_on) {
        return;
    }
    int64_t start = esp_timer_get_time();
    int64_t was = s_te_fall;
    while (s_te_fall == was) {
        if (esp_timer_get_time() - start > 25000) {   /* no edge in 25 ms: don't wait */
            break;
        }
    }
    int64_t waited = esp_timer_get_time() - start;
    portENTER_CRITICAL_SAFE(&s_lock);
    s_te_frames++;
    if (waited > 500) {
        s_te_held++;
    }
    portEXIT_CRITICAL_SAFE(&s_lock);
}

void muse_lcd_bands_bench(void)
{
    uint32_t sum, max, count, held, frames;
    portENTER_CRITICAL_SAFE(&s_lock);
    sum = s_band_sum_us;
    max = s_band_max_us;
    count = s_band_count;
    held = s_te_held;
    frames = s_te_frames;
    s_band_sum_us = s_band_max_us = s_band_count = 0;
    s_te_held = s_te_frames = 0;
    portEXIT_CRITICAL_SAFE(&s_lock);
    int avg = count ? (int)(sum / count) : 0;
    printf("@flush.bench bands=%u avg=%d us max=%u us | full frame (4 bands) ~%d us (~%d fps) | te %s, %u/%u frames waited >0.5ms\n",
           (unsigned)count, avg, (unsigned)max, avg * 4,
           avg > 0 ? 1000000 / (avg * 4) : 0,
           s_te_on ? "on" : "off", (unsigned)held, (unsigned)frames);
}

void muse_lcd_bands_swallow(int completions)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    s_swallow = completions > 0 ? (uint32_t)completions : 0;
    portEXIT_CRITICAL_SAFE(&s_lock);
}

void muse_lcd_bands_wedge(int ms)
{
    s_wedge_ms = ms > 0 ? (uint32_t)ms : 0;
}

void muse_lcd_bands_notify_drop(int notifies)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    s_notify_drop = notifies > 0 ? (uint32_t)notifies : 0;
    portEXIT_CRITICAL_SAFE(&s_lock);
}
