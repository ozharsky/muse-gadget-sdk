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

typedef struct {
    const uint8_t *data;   /* NULL: run call/call_arg instead */
    void (*call)(void *arg);
    void *call_arg;
    int x1, y1, x2, y2;
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
static SemaphoreHandle_t s_call_done;
static SemaphoreHandle_t s_chunk_free;   /* internal buffers not on the wire */
static uint8_t *s_chunk[2];
static size_t s_chunk_bytes;
static int s_chunks_out;                 /* of the band being sent */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_progress_ms;  /* last piece queued or done (ISR writes) */
static volatile uint32_t s_swallow;      /* bench: completions to drop on purpose */
static uint32_t s_recover_count;
static uint32_t s_recover_first_ms;
static volatile uint32_t s_driver_ms;    /* nonzero while a panel driver call is in progress */
static volatile uint32_t s_wedge_ms;     /* bench: make the next driver call stall this long */

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
    bool yield = chunk_done() && esp_lv_adapter_display_notify_color_trans_done_from_isr(s_disp);
    return yield || woken == pdTRUE;
}

static void lcd_recover(void);

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
         * Dump the interrupt state for the record -- this reboot pre-empts the
         * 20 s stall abort, and with it the core dump -- then reboot in ~4 s. */
        ESP_LOGE(TAG, "panel driver call stuck for %d ms (%d piece(s) outstanding): restarting",
                 DRIVER_STALL_MS, s_chunks_out);
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
    lv_display_flush_ready(s_disp);

    if (s_recover_count == 0 || (uint32_t)(now - s_recover_first_ms) > RECOVER_WINDOW_MS) {
        s_recover_count = 1;
        s_recover_first_ms = now;
    } else if (++s_recover_count >= RECOVER_MAX) {
        ESP_LOGE(TAG, "%u panel completions lost in a minute: restarting rather than limping",
                 (unsigned)s_recover_count);
        esp_restart();
    }
}

static esp_err_t queue_band(lv_display_t *disp, esp_lcd_panel_handle_t panel, int x1, int y1, int x2, int y2,
                            const void *data, void *ctx)
{
    (void)disp;
    (void)panel;
    (void)ctx;
    band_t b = { .data = data, .x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2 };
    return xQueueSend(s_bands, &b, 0) == pdTRUE ? ESP_OK : ESP_FAIL;
}

/*
 * The panel's draw waits for the piece before to finish, so each piece is
 * copied while the one before is on the wire. Even row counts keep every
 * piece's window starting on an even row, which the CO5300 needs.
 */
static void send_bands(void *arg)
{
    (void)arg;
    int k = 0;
    band_t b;
    for (;;) {
        xQueueReceive(s_bands, &b, portMAX_DELAY);
        if (!b.data) {
            /* The call travels in the band, not in a global: a caller that gave up
             * waiting (bounded RUN_WAIT_MS) must not have its call replaced by the
             * next one, nor crash the sender with a NULL. It talks to the panel
             * directly, so the driver-stall guard must cover it too. */
            if (b.call) {
                s_driver_ms = now_ms();
                b.call(b.call_arg);
                s_driver_ms = 0;
            }
            xSemaphoreGive(s_call_done);
            continue;
        }
        size_t row = (size_t)(b.x2 - b.x1) * 2;
        int rows = s_chunk_bytes / row & ~1;
        /* LVGL sends no more until this band is done, so nothing is still
         * counting down. */
        portENTER_CRITICAL_SAFE(&s_lock);
        s_chunks_out = (b.y2 - b.y1 + rows - 1) / rows;
        portEXIT_CRITICAL_SAFE(&s_lock);
        for (int y = b.y1; y < b.y2; y += rows) {
            int h = b.y2 - y < rows ? b.y2 - y : rows;
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
            memcpy(s_chunk[k], b.data + (size_t)(y - b.y1) * row, h * row);
            s_driver_ms = now_ms();
            if (s_wedge_ms) {
                /* Bench: simulate the IDF bus-lock handoff being lost -- the driver call
                 * never returns. The guard must reboot in ~4 s, not freeze for 20 s. */
                vTaskDelay(pdMS_TO_TICKS(s_wedge_ms));
                s_wedge_ms = 0;
            }
            esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, b.x1, y, b.x2, y + h, s_chunk[k]);
            s_driver_ms = 0;
            s_progress_ms = now_ms();
            if (err != ESP_OK) {
                xSemaphoreGive(s_chunk_free);
                if (chunk_done()) {
                    lv_display_flush_ready(s_disp);
                }
            }
            k ^= 1;
        }
    }
}

lv_display_t *muse_lcd_bands_register(esp_lv_adapter_display_config_t cfg, int lines, size_t chunk_bytes)
{
    s_panel = cfg.panel;
    s_chunk_bytes = chunk_bytes;
    if (xTaskGetCoreID(xTaskGetCurrentTaskHandle()) != MUSE_UI_CORE) {
        ESP_LOGE(TAG, "not pinned to core %d: the panel's SPI interrupt can strand the send task", MUSE_UI_CORE);
    }
    s_bands = xQueueCreate(2, sizeof(band_t));
    s_call_lock = xSemaphoreCreateMutex();
    s_call_done = xSemaphoreCreateBinary();
    s_chunk_free = xSemaphoreCreateCounting(2, 2);
    s_chunk[0] = heap_caps_malloc(chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_chunk[1] = heap_caps_malloc(chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_bands || !s_call_lock || !s_call_done || !s_chunk_free || !s_chunk[0] || !s_chunk[1] ||
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
    if (xSemaphoreTake(s_call_lock, pdMS_TO_TICKS(RUN_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "panel call busy for %d ms: skipped", RUN_WAIT_MS);
        return;
    }
    /* Drop a completion left behind by a previous caller whose wait timed out:
     * it would otherwise satisfy THIS wait before this call has run. Flush
     * BEFORE queueing, so our own call's completion can never be stolen (the
     * call still runs if a stale signal was the only one -- these users are
     * fire-and-forget brightness/sleep). */
    while (xSemaphoreTake(s_call_done, 0) == pdTRUE) {
    }
    const band_t call = { .call = fn, .call_arg = arg };
    if (xQueueSend(s_bands, &call, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "panel call queue full (send task stalled): skipped");
        xSemaphoreGive(s_call_lock);
        return;
    }
    if (xSemaphoreTake(s_call_done, pdMS_TO_TICKS(RUN_WAIT_MS)) != pdTRUE) {
        /* The call still runs when the send task gets there; not waiting further
         * keeps a stalled send task from freezing this caller too. The call
         * travels IN the band, so the next caller's cannot replace it. */
        ESP_LOGE(TAG, "panel call did not run within %d ms", RUN_WAIT_MS);
    }
    xSemaphoreGive(s_call_lock);
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
