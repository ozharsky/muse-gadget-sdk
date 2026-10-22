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
 * The part of the motion gestures that is worth testing: pure geometry over an
 * accelerometer sample, in milli-g. No IDF here so the host harness can include it.
 *
 * Everything is relative to the REST pose captured while the device sits still,
 * so none of it assumes which axis points out of the screen: "flipped" means the
 * gravity vector now points the other way, whatever that vector was.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int16_t x, y, z;   /* milli-g, screen frame as the part reports it */
} muse_imu_vec_t;

/* Angle from `rest` beyond `deg` degrees (a tilt, a turn, a lift). */
bool muse_imu_tilted(const muse_imu_vec_t *rest, const muse_imu_vec_t *cur, int deg);

/* The rest pose turned over: the gravity vector points the other way. */
bool muse_imu_flipped(const muse_imu_vec_t *rest, const muse_imu_vec_t *cur);

/* A jolt: the magnitude is far from 1 g, which a steady hold never is. */
bool muse_imu_jolt(const muse_imu_vec_t *v);
