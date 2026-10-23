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

bool muse_imu_face_down(const muse_imu_vec_t *v)
{
    float n = normf(v);
    return n > 600.0f && n < 1400.0f && v->z < -0.8f * n;
}

bool muse_imu_face_up_ish(const muse_imu_vec_t *v)
{
    float n = normf(v);
    return n < 200.0f || v->z > -0.3f * n;
}

float muse_imu_pitch_deg(const muse_imu_vec_t *v)
{
    return atan2f((float)v->y, (float)v->z) * 57.29578f;
}

float muse_imu_roll_deg(const muse_imu_vec_t *v)
{
    return atan2f((float)v->x, (float)v->z) * 57.29578f;
}

bool muse_imu_roll_ok(const muse_imu_vec_t *v)
{
    return sqrtf((float)v->x * v->x + (float)v->z * v->z) > 500.0f;
}

bool muse_swing_step(muse_swing_t *s, float deg, int64_t now_us)
{
    if (!s->init || (s->dir && now_us - s->last_us > MUSE_SWING_IDLE_US)) {
        *s = (muse_swing_t){ .init = true, .extreme = deg, .last_us = now_us };
        return false;
    }
    if (!s->dir) {
        if (fabsf(deg - s->extreme) >= MUSE_SWING_DEG) {
            s->dir = deg > s->extreme ? 1 : -1;
            s->legs = 1;
            s->extreme = deg;
            s->first_us = s->last_us = now_us;
        } else if (now_us - s->last_us > MUSE_SWING_IDLE_US) {
            s->extreme = deg;   /* still: re-anchor, so a slow drift never counts as a leg */
            s->last_us = now_us;
        }
        return false;
    }
    if ((s->dir > 0 && deg > s->extreme) || (s->dir < 0 && deg < s->extreme)) {
        s->extreme = deg;      /* the leg keeps going */
        s->last_us = now_us;
    } else if (fabsf(deg - s->extreme) >= MUSE_SWING_DEG) {
        s->dir = -s->dir;      /* reversed */
        s->legs++;
        s->extreme = deg;
        s->last_us = now_us;
    }
    if (s->legs >= 3) {
        bool quick = now_us - s->first_us <= MUSE_SWING_WINDOW_US;
        *s = (muse_swing_t){ .init = true, .extreme = deg, .last_us = now_us };
        return quick;
    }
    return false;
}
