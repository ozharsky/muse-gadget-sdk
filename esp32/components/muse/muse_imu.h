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
 * Motion gestures on the QMI8658 (Waveshare 1.75 / 1.75C): tilt to wake, flip to
 * stop a reply, shake to react. The chip has no driver in the BSP, so this is a
 * minimal one: probe, 31.25 Hz accelerometer only, six bytes a read.
 *
 * Compiled only with CONFIG_MUSE_IMU; boards without the part never call init and
 * every call below is a no-op that reports "not present".
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#define MUSE_IMU_TILT  (1u << 0)   /* pose changed from the resting one */
#define MUSE_IMU_FLIP  (1u << 1)   /* resting pose turned over, held */
#define MUSE_IMU_SHAKE (1u << 2)   /* jolts in quick succession */

/* Probes 0x6B then 0x6A and configures the accelerometer. */
esp_err_t muse_imu_init(i2c_master_bus_handle_t bus);

bool muse_imu_present(void);

/* Raw sample in milli-g, for the bench. Fails when the part isn't there. */
esp_err_t muse_imu_read(int16_t mg[3]);

/* Throttled (slower asleep) gesture poll; returns MUSE_IMU_* edges. */
unsigned muse_imu_poll(int64_t now_us, bool asleep);
