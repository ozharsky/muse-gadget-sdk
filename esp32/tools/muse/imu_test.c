/* Host harness for the motion-gesture geometry (muse_imu_math.c). */
#include <stdio.h>
#include <assert.h>

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

int main(void)
{
    tilted_cases();
    flipped_cases();
    jolt_cases();
    puts("IMU TEST OK");
    return 0;
}
