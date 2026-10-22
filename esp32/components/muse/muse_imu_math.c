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

/* Pure geometry for the motion gestures: no IDF, no state, host-testable. */

#include <math.h>

#include "muse_imu_math.h"

static float dotf(const muse_imu_vec_t *a, const muse_imu_vec_t *b)
{
    return (float)a->x * b->x + (float)a->y * b->y + (float)a->z * b->z;
}

static float normf(const muse_imu_vec_t *v)
{
    return sqrtf(dotf(v, v));
}

static float cos_angle(const muse_imu_vec_t *a, const muse_imu_vec_t *b)
{
    float na = normf(a), nb = normf(b);
    if (na < 200.0f || nb < 200.0f) {
        return 1.0f;   /* free fall or a bad sample: not evidence of a turn */
    }
    return dotf(a, b) / (na * nb);
}

bool muse_imu_tilted(const muse_imu_vec_t *rest, const muse_imu_vec_t *cur, int deg)
{
    return cos_angle(rest, cur) < cosf(deg * 3.14159265f / 180.0f);
}

bool muse_imu_flipped(const muse_imu_vec_t *rest, const muse_imu_vec_t *cur)
{
    return cos_angle(rest, cur) < -0.5f;
}

bool muse_imu_jolt(const muse_imu_vec_t *v)
{
    return fabsf(normf(v) - 1000.0f) > 350.0f;
}
