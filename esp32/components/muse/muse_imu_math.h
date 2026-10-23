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

/* Screen facing the desk: gravity along the screen's -z at about 1 g (screen-up reads z ~ +1 g).
 * muse_imu_face_up_ish is the release test (any pose clearly not face-down), so the two
 * together give the hysteresis a resting-but-wobbly hand needs. */
bool muse_imu_face_down(const muse_imu_vec_t *v);
bool muse_imu_face_up_ish(const muse_imu_vec_t *v);

/* Tilt angles of gravity, degrees: pitch in the y-z plane (a nod), roll in the x-z plane (a
 * side-to-side rock). Roll is meaningless while the screen is upright (x and z both ~0): the
 * caller only trusts it when muse_imu_roll_ok says gravity has a real x-z component. */
float muse_imu_pitch_deg(const muse_imu_vec_t *v);
float muse_imu_roll_deg(const muse_imu_vec_t *v);
bool muse_imu_roll_ok(const muse_imu_vec_t *v);

/* Gyro: the rotation rate about gravity (deg/s) -- a head-shake "no", in any pose -- given the
 * accelerometer's gravity vector and the gyro rates (deg/s, same axes). 0 in free fall. */
float muse_imu_yaw_rate(const muse_imu_vec_t *g, const float w[3]);
/* A leaky integrator for a rate (deg/s) into an angle (deg): drift-free over seconds, enough to
 * see a nod's swing. keep is the fraction left per second (0.25 forgets ~75 % a second). */
float muse_imu_leaky_angle(float angle, float rate, float dt_s, float keep_per_s);

/* A back-and-forth swing of one angle: true once the angle has moved >= MUSE_SWING_DEG one way,
 * then back, then the first way again (three legs) within MUSE_SWING_WINDOW_US; a pause longer
 * than MUSE_SWING_IDLE_US starts over. Zero-initialise; it resets itself after firing. */
#define MUSE_SWING_DEG 12.0f
#define MUSE_SWING_WINDOW_US 1800000
#define MUSE_SWING_IDLE_US 700000
typedef struct {
    bool init;
    int dir;           /* 0 = waiting to move, +1 / -1 = current leg */
    int legs;
    float extreme;     /* baseline while waiting, then the leg's furthest point */
    int64_t first_us, last_us;
} muse_swing_t;
bool muse_swing_step(muse_swing_t *s, float deg, int64_t now_us);
