# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The QMI8658 motion gestures decide "tilt", "flipped" and "jolt" from the
accelerometer alone, relative to a resting pose, so they never assume which axis
points out of the screen. imu_test.c pins the geometry: the thresholds, the
free-fall guard (a sample too short to be gravity is not a turn), and the flip
sign. A wrong sign here would stop a reply every time the device is raised.
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMU_TEST = ROOT / "tools" / "muse" / "imu_test.c"
MATH = ROOT / "components" / "muse" / "muse_imu_math.c"


def _cc() -> list[str]:
    cc = shlex.split(os.environ.get("CC", "cc"))
    if not cc or shutil.which(cc[0]) is None:
        raise unittest.SkipTest("C compiler not available")
    return cc


class ImuTest(unittest.TestCase):
    """tools/muse/imu_test.c: the gesture geometry."""

    def test_imu_math_passes(self) -> None:
        cc = _cc()
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "imu_test"
            build = subprocess.run(
                [*cc, "-O2", "-Wall", "-Wextra", "-I", "components/muse",
                 str(IMU_TEST), str(MATH), "-lm", "-o", str(exe)],
                cwd=ROOT, capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, msg=build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, cwd=ROOT)
            self.assertEqual(run.returncode, 0, msg=run.stdout + run.stderr)
            self.assertIn("IMU TEST OK", run.stdout)
