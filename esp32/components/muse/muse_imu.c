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

#if CONFIG_MUSE_IMU

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_imu.h"
#include "muse_imu_math.h"

static const char *TAG = "muse_imu";

/* QMI8658 registers. */
#define REG_WHOAMI 0x00
#define REG_CTRL1  0x02
#define REG_CTRL2  0x03
#define REG_CTRL7  0x08
#define REG_AX_L   0x35

#define WHOAMI_QMI8658 0x05
#define ADDR_LOW       0x6B   /* SA0 low, as the Waveshare BSP wires it */
#define ADDR_HIGH      0x6A

#define ACC_FS_2G      0      /* CTRL2 bits 7:4; 16384 LSB/g */
#define ACC_ODR_31HZ   8      /* CTRL2 bits 3:0 */
#define SCL_HZ         100000

/* Gesture tuning. Angles are checked against the resting pose. */
#define TILT_DEG        35
#define FLIP_DEG        120
#define TILT_HOLD_US    60000    /* two samples in a row past the angle */
#define FLIP_HOLD_US    600000   /* a deliberate turn, not a pass-through */
#define JOLT_WINDOW_US  400000
#define JOLT_COUNT      2
#define SHAKE_COOLDOWN_US 1200000
#define STABLE_AGAIN_US 2000000  /* hold still this long and that's the new rest */
#define FACEDOWN_HOLD_US 1500000
#define FACEUP_HOLD_US  300000

#define AWAKE_PERIOD_US 40000
#define ASLEEP_PERIOD_US 150000

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_present;

static muse_imu_vec_t s_rest;
static bool s_have_rest;
static int64_t s_stable_since;
static int64_t s_tilt_seen, s_flip_seen, s_last_jolt, s_last_shake;
static int s_jolts;
static bool s_tilt_latched, s_flip_latched;
static int64_t s_last_poll;

static esp_err_t rd(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, n, 100);
}

static esp_err_t wr(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(dev, b, sizeof(b), 100);
}

bool muse_imu_present(void)
{
    return s_present;
}

esp_err_t muse_imu_read(int16_t mg[3])
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t b[6];
    esp_err_t err = rd(s_dev, REG_AX_L, b, sizeof(b));
    if (err != ESP_OK) {
        return err;
    }
    /* Little-endian int16, 16384 LSB per g at +-2 g. */
    const int32_t raw[3] = {
        (int16_t)((b[1] << 8) | b[0]),
        (int16_t)((b[3] << 8) | b[2]),
        (int16_t)((b[5] << 8) | b[4]),
    };
    for (int i = 0; i < 3; i++) {
        mg[i] = (int16_t)(raw[i] * 1000 / 16384);
    }
    return ESP_OK;
}

static esp_err_t configure(uint8_t addr)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = SCL_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t id = 0;
    if (rd(s_dev, REG_WHOAMI, &id, 1) != ESP_OK || id != WHOAMI_QMI8658) {
        goto not_here;
    }
    /* Byte-wise register access, all axes off, then accelerometer only. */
    if (wr(s_dev, REG_CTRL1, 0x40) != ESP_OK || wr(s_dev, REG_CTRL7, 0x00) != ESP_OK) {
        goto not_here;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    /* +-2 g at 31.25 Hz: a wake gesture needs nothing faster, and the slower
     * rate is what keeps a sleeping badge's battery honest. */
    if (wr(s_dev, REG_CTRL2, (ACC_FS_2G << 4) | ACC_ODR_31HZ) != ESP_OK) {
        goto not_here;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
    if (wr(s_dev, REG_CTRL7, 0x01) != ESP_OK) {
        goto not_here;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
not_here:
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t muse_imu_init(i2c_master_bus_handle_t bus)
{
    s_bus = bus;
    esp_err_t err = configure(ADDR_LOW);
    if (err != ESP_OK) {
        err = configure(ADDR_HIGH);
    }
    if (err != ESP_OK) {
        return err;
    }
    /* The first conversion is not ready the moment the part is enabled: wait for a
     * sample that actually looks like gravity, or the rest pose starts at zero and
     * every tilt test answers "not tilted" until the 2 s re-baseline. */
    int16_t mg[3] = { 0 };
    for (int i = 0; i < 10; i++) {
        if (muse_imu_read(mg) == ESP_OK
            && abs(mg[0]) + abs(mg[1]) + abs(mg[2]) > 200) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_rest.x = mg[0];
    s_rest.y = mg[1];
    s_rest.z = mg[2];
    s_have_rest = abs(mg[0]) + abs(mg[1]) + abs(mg[2]) > 200;
    s_present = true;
    ESP_LOGI(TAG, "QMI8658 ready at 31 Hz, rest %d/%d/%d mg", mg[0], mg[1], mg[2]);
    return ESP_OK;
}

unsigned muse_imu_poll(int64_t now_us, bool asleep)
{
    if (!s_present || (!asleep && now_us - s_last_poll < AWAKE_PERIOD_US)
        || (asleep && now_us - s_last_poll < ASLEEP_PERIOD_US)) {
        return 0;
    }
    s_last_poll = now_us;

    int16_t mg[3];
    if (muse_imu_read(mg) != ESP_OK) {
        return 0;
    }
    muse_imu_vec_t v = { mg[0], mg[1], mg[2] };
    if (!s_have_rest) {
        s_rest = v;
        s_have_rest = true;
        return 0;
    }

    unsigned flags = 0;
    bool tilted = muse_imu_tilted(&s_rest, &v, TILT_DEG);
    bool flipped = muse_imu_flipped(&s_rest, &v);
    bool jolt = muse_imu_jolt(&v);

    if (!tilted && !jolt) {
        if (!s_stable_since) {
            s_stable_since = now_us;
        } else if (!asleep && now_us - s_stable_since > STABLE_AGAIN_US) {
            /* Held still a while: this is the pose the next gesture is measured from. */
            s_rest = v;
            s_stable_since = now_us;
        }
    } else {
        s_stable_since = 0;
    }

    if (tilted) {
        if (!s_tilt_seen) {
            s_tilt_seen = now_us;
        } else if (!s_tilt_latched && now_us - s_tilt_seen >= TILT_HOLD_US) {
            s_tilt_latched = true;
            flags |= MUSE_IMU_TILT;
        }
    } else {
        s_tilt_seen = 0;
        s_tilt_latched = false;
    }

    if (flipped) {
        if (!s_flip_seen) {
            s_flip_seen = now_us;
        } else if (!s_flip_latched && now_us - s_flip_seen >= FLIP_HOLD_US) {
            s_flip_latched = true;
            flags |= MUSE_IMU_FLIP;
        }
    } else {
        s_flip_seen = 0;
        s_flip_latched = false;
    }

    /* Face-down: an edge after 1.5 s screen-down, and the matching face-up edge once it is
     * clearly not face-down for 0.3 s (the hysteresis keeps a wobble from toggling it). */
    static bool s_down;
    static int64_t s_down_seen, s_up_seen;
    if (!s_down) {
        s_down_seen = muse_imu_face_down(&v) ? (s_down_seen ? s_down_seen : now_us) : 0;
        if (s_down_seen && now_us - s_down_seen >= FACEDOWN_HOLD_US) {
            s_down = true;
            s_up_seen = 0;
            flags |= MUSE_IMU_FACEDOWN;
        }
    } else {
        s_up_seen = muse_imu_face_up_ish(&v) ? (s_up_seen ? s_up_seen : now_us) : 0;
        if (s_up_seen && now_us - s_up_seen >= FACEUP_HOLD_US) {
            s_down = false;
            s_down_seen = 0;
            flags |= MUSE_IMU_FACEUP;
        }
    }

    /* Nod / rock: only awake (the swings need the 25 Hz awake cadence). */
    static muse_swing_t s_pitch, s_roll;
    if (!asleep && !s_down) {
        if (muse_swing_step(&s_pitch, muse_imu_pitch_deg(&v), now_us)) {
            flags |= MUSE_IMU_NOD;
            s_roll = (muse_swing_t){ 0 };
        }
        if (muse_imu_roll_ok(&v) && muse_swing_step(&s_roll, muse_imu_roll_deg(&v), now_us) && !(flags & MUSE_IMU_NOD)) {
            flags |= MUSE_IMU_ROCK;
            s_pitch = (muse_swing_t){ 0 };
        }
    }

    if (jolt) {
        if (!s_last_jolt || now_us - s_last_jolt > JOLT_WINDOW_US) {
            s_jolts = 0;
        }
        s_jolts++;
        s_last_jolt = now_us;
        if (s_jolts >= JOLT_COUNT && now_us - s_last_shake > SHAKE_COOLDOWN_US) {
            s_jolts = 0;
            s_last_shake = now_us;
            flags |= MUSE_IMU_SHAKE;
        }
    }
    return flags;
}

#endif /* CONFIG_MUSE_IMU */
