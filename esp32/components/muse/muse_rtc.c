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

#include "sdkconfig.h"

#include <stdio.h>

#include "esp_log.h"

#include "muse_rtc.h"

static const char *TAG = "muse_rtc";

static i2c_master_bus_handle_t s_bus;

esp_err_t muse_rtc_init(i2c_master_bus_handle_t bus)
{
    s_bus = bus;
    return bus ? ESP_OK : ESP_ERR_INVALID_ARG;
}

void muse_rtc_scan(void)
{
    if (!s_bus) {
        printf("@i2c.scan no bus\n");
        return;
    }
    printf("@i2c.scan");
    int n = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(s_bus, a, 30) == ESP_OK) {
            printf(" 0x%02X", a);
            n++;
        }
    }
    printf(" (%d device%s)\n", n, n == 1 ? "" : "s");
    ESP_LOGI(TAG, "scan done: %d device(s)", n);
}

/* Reads `n` bytes starting at register `reg` from a temporary device handle. */
static esp_err_t read_regs(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(s_bus, &cfg, &dev);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_transmit_receive(dev, &reg, 1, buf, n, 100);
    i2c_master_bus_rm_device(dev);
    return err;
}

void muse_rtc_probe(void)
{
    if (!s_bus) {
        printf("@rtc.probe no bus\n");
        return;
    }
    /* PCF85063A family: 0x51; DS3231: 0x68; PCF8563: 0x51; RX8025: 0x32. */
    static const uint8_t addrs[] = { 0x51, 0x68, 0x32, 0x6F };
    for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        uint8_t b[11] = { 0 };
        esp_err_t err = read_regs(addrs[i], 0x00, b, sizeof(b));
        if (err != ESP_OK) {
            printf("@rtc.probe 0x%02X: %s (no answer)\n", addrs[i], esp_err_to_name(err));
            continue;
        }
        printf("@rtc.probe 0x%02X:", addrs[i]);
        for (size_t k = 0; k < sizeof(b); k++) {
            printf(" %02X", b[k]);
        }
        printf("\n");
    }
}
