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
 * Waveshare ESP32-S3-Touch-AMOLED-1.43C: 1.43" 466x466 round SH8601 AMOLED
 * over QSPI, CST816S touch, ES8311 speaker path (NS4150 amp on GPIO46) +
 * ES7210 dual mic, BOOT button only, battery ADC on GPIO4, charger status on
 * GPIO7, no PMU, native USB.
 *
 * Pin/fact sources (per devices/AGENTS.md):
 * - waveshareteam/ESP32-S3-Touch-AMOLED-1.43C:
 *   02_Example/arduino_v3.3.0/04_Audio_Test/bsp_config.h (display, touch,
 *   I2C, button, battery pins), esp32_s3_touch_amoled_1.43c.cpp (touch probe
 *   at I2C 0x15 with the CST816 register map; battery = ADC1_CH3 x2 divider,
 *   charging = GPIO7 low), src/codec_board/board_cfg.txt (I2S0: MCLK 38, BCLK
 *   39, WS 40, record 42, playback 41; out ES8311 + PA on 46; in ES7210).
 * - 78/xiaozhi-esp32 main/boards/waveshare/esp32-s3-touch-amoled-1.43c:
 *   SH8601 QSPI bring-up (init commands, 0x36=0xC0 180-degree rotation,
 *   8 px column gap, even-aligned flush windows), ES7210 record over I2S TDM
 *   with all four mics selected (driver takes TDM mode at >= 3 mics).
 * - The board schematic (04_Schematic/ESP32-S3-Touch-AMOLED-1.43C-Schematic.pdf
 *   in Waveshare's repo): ESP32-S3-PICO-1-N8R8 module; single BOOT button K1
 *   on GP0; BAT_ADC on GP4; CODEC_EN on GP18; NS4150B amp with CTRL on GP46;
 *   ES7210 (0x40) with the two physical mics on inputs MIC1/MIC2, MIC3 wired
 *   as the AEC echo reference, MIC4 unconnected; NO onboard speaker, only the
 *   unpopulated J8 "SPK" connector, so the speaker path is initialized but
 *   silent until a speaker is soldered to J8.
 *
 * The ES7210's TDM record stream is 4x16-bit per frame; a small data-if shim
 * below mixes the slots to the 16-bit stereo muse_audio expects.
 */
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "es7210_adc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_audio.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_H_RES 466
#define LCD_V_RES 466
#define DRAW_BUF_LINES 118      /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_H_RES * 8 * 2)

#define LCD_SPI_HOST SPI2_HOST
#define LCD_PIN_CS GPIO_NUM_15
#define LCD_PIN_SCK GPIO_NUM_14
#define LCD_PIN_D0 GPIO_NUM_9
#define LCD_PIN_D1 GPIO_NUM_10
#define LCD_PIN_D2 GPIO_NUM_11
#define LCD_PIN_D3 GPIO_NUM_12
#define LCD_PIN_RST GPIO_NUM_13

#define I2C_SDA GPIO_NUM_47
#define I2C_SCL GPIO_NUM_48
#define TOUCH_ADDR 0x15         /* the vendor's own driver probes 0x15 */
#define TOUCH_RST GPIO_NUM_16
/* NOTE: the touch INT pin (GPIO17) is deliberately left unconfigured. The
 * lvgl adapter switches to IRQ-gated reads when int_gpio_num is set, and the
 * CST816S on this panel never raised a usable interrupt in that mode, which
 * made the whole UI untouchable. The vendor's own driver polls the chip and
 * never touches the INT pin, so we do the same. */

#define I2S_NUM I2S_NUM_0
#define I2S_MCLK GPIO_NUM_38
#define I2S_BCLK GPIO_NUM_39
#define I2S_WS GPIO_NUM_40
#define I2S_DIN GPIO_NUM_42    /* record: ES7210 SDOUT */
#define I2S_DOUT GPIO_NUM_41   /* playback: ES8311 DIN */
#define PA_EN GPIO_NUM_46       /* NS4150 speaker amp enable */
#define CODEC_LDO_EN GPIO_NUM_18

#define BATT_ADC_CH ADC_CHANNEL_3   /* GPIO4, x2 divider (vendor's bsp_batt_*) */
#define CHARGE_STAT_GPIO GPIO_NUM_7 /* low while charging (ETA6098) */

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_boot;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;

/* SH8601 init, as xiaozhi-esp32's proven board for this panel. */
static const sh8601_lcd_init_cmd_t s_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x36, (uint8_t[]){0xC0}, 1, 0},   /* 180-degree rotation */
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 0},
    {0x11, (uint8_t[]){0x00}, 0, 100},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

static esp_err_t init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, GPIO_NUM_0), TAG, "boot button");

    /* Codec LDO on (the vendor's CODEC_LDO_EN_PIN). */
    gpio_config_t ldo = {
        .pin_bit_mask = 1ULL << CODEC_LDO_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&ldo), TAG, "codec ldo");
    ESP_RETURN_ON_ERROR(gpio_set_level(CODEC_LDO_EN, 1), TAG, "codec ldo on");

    /* Battery ADC with curve-fitting calibration, as the vendor's bsp_batt_*. */
    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BATT_ADC_CH, &ch_cfg), TAG, "adc ch");
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    s_cali_ok = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK;

    gpio_config_t chg = {
        .pin_bit_mask = 1ULL << CHARGE_STAT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&chg), TAG, "charge stat");
    return ESP_OK;
}

/* The SH8601 needs even-aligned update windows (as on the 1.75C's CO5300). */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

/* Vendor's GetCoords logic as an LVGL indev read callback. Two separate I2C
 * transactions: 2 bytes from 0x02 (gesture + count), then 4 bytes from 0x03
 * (x/y) only when a touch is down. Mirrors both axes for the 180-degree
 * panel rotation. */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    i2c_master_dev_handle_t tp_dev = (i2c_master_dev_handle_t)lv_indev_get_user_data(indev);
    uint8_t hdr[2] = {0, 0};
    uint8_t reg = 0x02;
    data->state = LV_INDEV_STATE_RELEASED;
    if (i2c_master_transmit_receive(tp_dev, &reg, 1, hdr, 2, 50) != ESP_OK) {
        return;
    }
    uint8_t event = hdr[1] >> 6;
    if (hdr[0] && event != 0x01) {
        uint8_t pos[4] = {0};
        reg = 0x03;
        if (i2c_master_transmit_receive(tp_dev, &reg, 1, pos, 4, 50) == ESP_OK) {
            uint16_t x = ((uint16_t)(pos[0] & 0x0f) << 8) | pos[1];
            uint16_t y = ((uint16_t)(pos[2] & 0x0f) << 8) | pos[3];
            /* Mirror for 180-degree rotation. */
            data->point.x = LCD_H_RES - 1 - x;
            data->point.y = LCD_V_RES - 1 - y;
            data->state = LV_INDEV_STATE_PRESSED;
        }
    }
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_PIN_SCK,
        .data0_io_num = LCD_PIN_D0,
        .data1_io_num = LCD_PIN_D1,
        .data2_io_num = LCD_PIN_D2,
        .data3_io_num = LCD_PIN_D3,
        .max_transfer_sz = LCD_CHUNK_BYTES,
    };
    if (spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = GPIO_NUM_NC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    if (esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    sh8601_vendor_config_t vendor_cfg = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_set_gap(s_panel, 8, 0);   /* round panel's column offset */
    if (esp_lcd_panel_reset(s_panel) != ESP_OK || esp_lcd_panel_init(s_panel) != ESP_OK) {
        return NULL;
    }

    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_H_RES,
            .ver_res = LCD_V_RES,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    /* CST820 at 0x15 (not CST816S; the panel is rotated 180 degrees, so mirror
     * both axes). The vendor's driver does two separate I2C transactions:
     * read 2 bytes from 0x02, then (if touched) 4 bytes from 0x03. The
     * esp_lcd_touch_cst816s driver's single 5-byte read from 0x02 does not
     * work with this chip, so we poll it directly with a raw LVGL indev.
     * Non-fatal: the UI and push-to-talk still boot without touch. */
    i2c_master_dev_handle_t tp_dev = NULL;
    const i2c_device_config_t tp_dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_ADDR,
        .scl_speed_hz = 400000,
    };
    *touch = NULL;

    /* Vendor's reset sequence from bsp_touch_init, before any I2C traffic. */
    gpio_config_t tp_rst = {
        .pin_bit_mask = 1ULL << TOUCH_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&tp_rst) == ESP_OK) {
        gpio_set_level(TOUCH_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(TOUCH_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(TOUCH_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (i2c_master_probe(s_i2c, TOUCH_ADDR, 50) == ESP_OK) {
        ESP_LOGI(TAG, "touch chip answers at 0x%02x", TOUCH_ADDR);
        if (i2c_master_bus_add_device(s_i2c, &tp_dev_cfg, &tp_dev) == ESP_OK) {
            /* Raw LVGL indev using the vendor's GetCoords logic. */
            lv_indev_t *indev = lv_indev_create();
            lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_display(indev, disp);
            lv_indev_set_user_data(indev, tp_dev);
            lv_indev_set_read_cb(indev, touch_read_cb);
            *touch = indev;
            ESP_LOGI(TAG, "touch indev registered (CST820 direct poll)");
        }
    } else {
        ESP_LOGW(TAG, "no touch chip at 0x%02x: continuing without touch", TOUCH_ADDR);
    }
    if (esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void send_brightness(void *level)
{
    /* SH8601 "write display brightness" (0x51). */
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x51 << 8), level, 1);
}

static void set_brightness(int pct)
{
    uint8_t level = (uint8_t)(pct * 255 / 100);
    muse_lcd_bands_run(send_brightness, &level);
}

static void send_sleep(void *sleep)
{
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | ((*(bool *)sleep ? 0x10 : 0x11) << 8), NULL, 0);
}

static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);   /* SLPIN/SLPOUT; GRAM is kept */
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

/*
 * The ES7210 record stream is I2S TDM, 16-bit, 4 slots per frame. This shim
 * mixes all four slots to dual-mono stereo for muse_audio.
 */
typedef struct {
    audio_codec_data_if_t base;
    const audio_codec_data_if_t *inner;
} tdm_stereo_if_t;

static int tdm_open(const audio_codec_data_if_t *h, void *data_cfg, int cfg_size)
{
    return ((const tdm_stereo_if_t *)h)->inner->open(((const tdm_stereo_if_t *)h)->inner, data_cfg, cfg_size);
}

static bool tdm_is_open(const audio_codec_data_if_t *h)
{
    return ((const tdm_stereo_if_t *)h)->inner->is_open(((const tdm_stereo_if_t *)h)->inner);
}

static int tdm_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t dev_type, bool enable)
{
    return ((const tdm_stereo_if_t *)h)->inner->enable(((const tdm_stereo_if_t *)h)->inner, dev_type, enable);
}

static int tdm_set_fmt(const audio_codec_data_if_t *h, esp_codec_dev_type_t dev_type, esp_codec_dev_sample_info_t *fs)
{
    /* esp_codec_dev_open() makes the data interface re-run i2s_channel_reconfig_tdm_slot
     * with total_slot = fs->channel and a mask built from it, which silently replaces the
     * 4-slot config from audio_init(). The ES7210 is in 4-slot TDM, so force 4 slots here. */
    esp_codec_dev_sample_info_t tdm_fs = *fs;
    if (dev_type & ESP_CODEC_DEV_TYPE_IN) {
        tdm_fs.channel = 4;
        tdm_fs.channel_mask = 0xF;
        tdm_fs.bits_per_sample = 16;
        ESP_LOGI(TAG, "mic rx forced to 4-slot TDM (caller asked %dch/%dbit/%dHz)",
                 (int)fs->channel, (int)fs->bits_per_sample, (int)fs->sample_rate);
    }
    return ((const tdm_stereo_if_t *)h)->inner->set_fmt(((const tdm_stereo_if_t *)h)->inner, dev_type, &tdm_fs);
}

static int16_t s_tdm_scratch[2 * 640];

static int tdm_read(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    /* esp_codec_dev data interfaces return ESP_CODEC_DEV_OK (0) on success, not a
     * byte count; read exactly frames * 8 bytes (4 slots x 16 bit) per chunk. */
    const tdm_stereo_if_t *shim = (const tdm_stereo_if_t *)h;
    int16_t *out = (int16_t *)data;
    int frames_left = size / 4;   /* stereo 16-bit output frames */
    while (frames_left > 0) {
        int frames = frames_left;
        if (frames > (int)(sizeof(s_tdm_scratch) / 8)) {
            frames = sizeof(s_tdm_scratch) / 8;
        }
        int r = shim->inner->read(shim->inner, (uint8_t *)s_tdm_scratch, frames * 8);
        if (r != ESP_CODEC_DEV_OK) {
            return r;
        }
        for (int f = 0; f < frames; f++) {
            /* Only MIC1 (slot 0) and MIC2 (slot 2) are real mics: slot 1 is the ES7210's
             * AEC echo reference (cabinet noise) and slot 3 floats. Summing all four cost
             * ~6 dB of SNR. Average the two real mics. */
            int32_t m = (int32_t)s_tdm_scratch[f * 4 + 0] + s_tdm_scratch[f * 4 + 2];
            int16_t mono = (int16_t)(m >> 1);
            out[0] = mono;
            out[1] = mono;
            out += 2;
        }
        frames_left -= frames;
    }
    return ESP_CODEC_DEV_OK;
}

static int tdm_write(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    const tdm_stereo_if_t *shim = (const tdm_stereo_if_t *)h;
    return shim->inner->write(shim->inner, data, size);
}

static int tdm_close(const audio_codec_data_if_t *h)
{
    return ((const tdm_stereo_if_t *)h)->inner->close(((const tdm_stereo_if_t *)h)->inner);
}

static const tdm_stereo_if_t *tdm_stereo_wrap(const audio_codec_data_if_t *inner)
{
    static tdm_stereo_if_t shim;
    shim.base.open = tdm_open;
    shim.base.is_open = tdm_is_open;
    shim.base.enable = tdm_enable;
    shim.base.set_fmt = tdm_set_fmt;
    shim.base.read = tdm_read;
    shim.base.write = tdm_write;
    shim.base.close = tdm_close;
    shim.inner = inner;
    return &shim;
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");

    /* Playback: plain I2S stereo into the ES8311. */
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");

    /* Record: TDM, 16-bit, 4 slots. */
    const i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = MUSE_AUDIO_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3,
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .bit_shift = true,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM,
        },
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(rx, &tdm_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t tx_i2s_cfg = { .port = I2S_NUM, .rx_handle = NULL, .tx_handle = tx };
    audio_codec_i2s_cfg_t rx_i2s_cfg = { .port = I2S_NUM, .rx_handle = rx, .tx_handle = NULL };
    const audio_codec_data_if_t *tx_if = audio_codec_new_i2s_data(&tx_i2s_cfg);
    const audio_codec_data_if_t *rx_if = audio_codec_new_i2s_data(&rx_i2s_cfg);
    ESP_RETURN_ON_FALSE(tx_if && rx_if, ESP_ERR_NO_MEM, TAG, "data if");

    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .bus_handle = s_i2c };
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_ERR_NO_MEM, TAG, "gpio if");

    /* ES8311 DAC -> NS4150 amp. Mirrors the 1.75C BSP's speaker bring-up. */
    i2c_cfg.addr = ES8311_CODEC_DEFAULT_ADDR;
    const audio_codec_ctrl_if_t *es8311_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(es8311_ctrl, ESP_FAIL, TAG, "es8311 ctrl");
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = es8311_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PA_EN,
        .pa_reverted = false,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *es8311 = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(es8311, ESP_FAIL, TAG, "ES8311 not responding");

    /* ES7210: all four mics stay selected for TDM mode (the 4-slot frame must carry all
     * slots); the shim above then mixes only MIC1/MIC2 -- the real mics. MIC3 is the AEC
     * reference (cabinet noise), MIC4 is unconnected. */
    i2c_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
    const audio_codec_ctrl_if_t *es7210_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(es7210_ctrl, ESP_FAIL, TAG, "es7210 ctrl");
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = es7210_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *es7210 = es7210_codec_new(&es7210_cfg);
    ESP_RETURN_ON_FALSE(es7210, ESP_FAIL, TAG, "ES7210 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es8311, .data_if = tx_if };
    esp_codec_dev_cfg_t in_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = es7210, .data_if = &tdm_stereo_wrap(rx_if)->base,
    };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    /* ES7210 PGA steps are 3 dB; snap so the UI shows what's applied. */
    db = (db / 3) * 3;
    esp_codec_dev_set_in_gain(mic, (float)db);
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_boot);   /* BOOT talks; there is no aux */
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_boot }, 1, timeout_ms);
}

/* Battery through the x2 divider on GPIO4, as the vendor's bsp_batt_*. */
static esp_err_t read_power(muse_power_t *out)
{
    int raw, mv = 0;
    if (adc_oneshot_read(s_adc, BATT_ADC_CH, &raw) == ESP_OK && s_cali_ok) {
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            mv *= 2;
        }
    }
    out->battery_mv = mv;
    /* Rough Li-ion curve for the meter; the vendor reports millivolts only. The 100%
     * anchor sits at 4.10 V: the charger's CV termination relaxes to ~4.06-4.10 V resting,
     * so a full cell idles just under 4.2 V — anchored at 4.2 the meter never reads 100. */
    out->battery_pct = mv <= 3300 ? 0 : mv >= 4100 ? 100 : (mv - 3300) * 100 / 800;
    out->charging = gpio_get_level(CHARGE_STAT_GPIO) == 0;
    out->usb = usb_serial_jtag_is_connected();
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    /* Single-button board: BOOT is push-to-talk, and holding it must not
     * kill the board mid-sentence. The physical PWR button handles power,
     * so software power-off is a no-op here. */
    return ESP_OK;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-AMOLED-1.43C",
    .width = LCD_H_RES,
    .height = LCD_V_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.43f,
    .talk_button = "boot",
    .aux_button = NULL,              /* single-button board: no aux key */
    .talk_hint = { LV_ALIGN_CENTER, 153, 129 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = -1,                 /* dual-mono from the shim; mixing is a no-op */
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
