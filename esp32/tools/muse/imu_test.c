/* Host harness for the motion-gesture geometry (muse_imu_math.c). */
#include <stdio.h>
#include <assert.h>
#include <math.h>

#include "muse_imu_math.h"

static void tilted_cases(void)
{
    muse_imu_vec_t rest = { 0, 0, 1000 };
    muse_imu_vec_t same = { 0, 0, 1000 };
    muse_imu_vec_t small = { 200, 0, 979 };      /* ~11.5 deg */
    muse_imu_vec_t diag = { 0, 700, 700 };       /* 45 deg */
    muse_imu_vec_t side = { 1000, 0, 0 };        /* 90 deg */
    muse_imu_vec_t bad = { 0, 0, 50 };           /* free fall, no evidence */

    assert(!muse_imu_tilted(&rest, &same, 35));
    assert(!muse_imu_tilted(&rest, &small, 35));
    assert(muse_imu_tilted(&rest, &diag, 35));
    assert(muse_imu_tilted(&rest, &side, 35));
    assert(!muse_imu_tilted(&rest, &bad, 35));   /* the norm guard */
}

static void flipped_cases(void)
{
    muse_imu_vec_t rest = { 0, 0, 1000 };
    muse_imu_vec_t over = { 0, 0, -1000 };
    muse_imu_vec_t half = { 0, 985, -174 };     /* 100 deg, still 1 g */
    muse_imu_vec_t past = { 0, 0, -700 };

    assert(muse_imu_flipped(&rest, &over));
    assert(!muse_imu_flipped(&rest, &half));
    assert(muse_imu_flipped(&rest, &past));
}

static void jolt_cases(void)
{
    muse_imu_vec_t steady = { 0, 0, 1000 };
    muse_imu_vec_t push = { 0, 0, 1600 };
    muse_imu_vec_t tiny = { 0, 0, 1100 };
    muse_imu_vec_t drop = { 0, 0, 400 };         /* falling */

    assert(!muse_imu_jolt(&steady));
    assert(muse_imu_jolt(&push));
    assert(!muse_imu_jolt(&tiny));
    assert(muse_imu_jolt(&drop));
}

static void face_cases(void)
{
    muse_imu_vec_t up = { -281, 134, 913 };      /* the 1.75C on a desk, measured */
    muse_imu_vec_t down = { 120, -60, -985 };
    muse_imu_vec_t edge = { 0, -1000, 40 };      /* standing on its edge */
    muse_imu_vec_t tilted_down = { 0, 700, -700 };   /* 45 deg: neither */
    assert(!muse_imu_face_down(&up) && muse_imu_face_up_ish(&up));
    assert(muse_imu_face_down(&down) && !muse_imu_face_up_ish(&down));
    assert(!muse_imu_face_down(&edge) && muse_imu_face_up_ish(&edge));
    assert(!muse_imu_face_down(&tilted_down) && !muse_imu_face_up_ish(&tilted_down));
}

/* Feeds a sampled sine (amp deg, period ms) at 25 Hz; returns the ms it fired at, or -1. */
static int swing_fires(float amp, int period_ms, int total_ms, float offset)
{
    muse_swing_t s = { 0 };
    for (int t = 0; t <= total_ms; t += 40) {
        float deg = offset + amp * sinf(2.0f * 3.14159265f * (float)t / (float)period_ms);
        if (muse_swing_step(&s, deg, (int64_t)t * 1000)) {
            return t;
        }
    }
    return -1;
}

static void swing_cases(void)
{
    assert(swing_fires(20.0f, 700, 3000, 0) > 0);        /* a brisk nod: yes */
    assert(swing_fires(20.0f, 700, 3000, -80.0f) > 0);   /* same nod, held upright */
    assert(swing_fires(4.0f, 700, 3000, 0) < 0);         /* a hand tremor: no */
    assert(swing_fires(30.0f, 6000, 6000, 0) < 0);       /* one slow tilt and back: no */
    muse_swing_t s = { 0 };                               /* a single lift and settle: no */
    for (int t = 0, fired = 0; t <= 3000; t += 40) {
        float deg = t < 400 ? (float)t / 400.0f * 40.0f : 40.0f;
        fired |= muse_swing_step(&s, deg, (int64_t)t * 1000);
        assert(!fired);
    }
    muse_imu_vec_t flat = { 500, 0, 860 }, upright = { 30, -1000, 20 };
    assert(muse_imu_roll_ok(&flat) && !muse_imu_roll_ok(&upright));
    assert(muse_imu_roll_deg(&flat) > 25.0f && muse_imu_roll_deg(&flat) < 35.0f);
}

/* A nod fed through the leaky gyro integrator still swings past the detector's threshold. */
static void gyro_cases(void)
{
    muse_imu_vec_t flat = { 0, 0, 1000 }, upright = { 0, -1000, 0 };
    float spin_z[3] = { 0, 0, 90 }, spin_y[3] = { 0, 90, 0 }, tip_x[3] = { 90, 0, 0 };
    assert(fabsf(muse_imu_yaw_rate(&flat, spin_z) - 90.0f) < 0.5f);       /* flat: yaw is z */
    assert(fabsf(muse_imu_yaw_rate(&upright, spin_y) + 90.0f) < 0.5f);    /* upright: yaw is y */
    assert(fabsf(muse_imu_yaw_rate(&upright, tip_x)) < 0.5f);             /* a nod is not a shake */
    muse_swing_t s = { 0 };
    float ang = 0;
    int fired = -1;
    for (int t = 0; t <= 3000 && fired < 0; t += 40) {   /* +-120 deg/s at 1.4 Hz: a brisk nod */
        float rate = 120.0f * cosf(2.0f * 3.14159265f * 1.4f * (float)t / 1000.0f);
        ang = muse_imu_leaky_angle(ang, rate, 0.04f, 0.25f);
        if (muse_swing_step(&s, ang, (int64_t)t * 1000)) {
            fired = t;
        }
    }
    assert(fired > 0);
    s = (muse_swing_t){ 0 };
    ang = 0;
    for (int t = 0; t <= 3000; t += 40) {                /* a slow 20 deg/s drift: nothing */
        ang = muse_imu_leaky_angle(ang, 20.0f, 0.04f, 0.25f);
        assert(!muse_swing_step(&s, ang, (int64_t)t * 1000));
    }
}

int main(void)
{
    gyro_cases();
    tilted_cases();
    flipped_cases();
    jolt_cases();
    face_cases();
    swing_cases();
    puts("IMU TEST OK");
    return 0;
}
