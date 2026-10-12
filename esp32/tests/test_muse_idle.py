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

"""muse_idle.h is the resting face's whole rhythm: the blink cadence (doubles, doze
blinks), the saccade speed classes, the micro-expression priority rules (REACT
pre-empts idle; idle never rides on top of SPEAK) and the breath wobble. idle_test.c
asserts all of that under fixed seeds; this pins it into the suite so a future edit
cannot silently change how every face rests.
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
IDLE_TEST = ROOT / "tools" / "muse" / "idle_test.c"


def _cc() -> list[str]:
    cc = shlex.split(os.environ.get("CC", "cc"))
    if not cc or shutil.which(cc[0]) is None:
        raise unittest.SkipTest("C compiler not available")
    return cc


class IdleTest(unittest.TestCase):
    """tools/muse/idle_test.c: the idle scheduler's cadence, gaze, beats and breath."""

    def test_idle_math_passes(self) -> None:
        cc = _cc()
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "idle_test"
            build = subprocess.run(
                [*cc, "-O2", "-Wall", "-I", "components/muse/avatar",
                 str(IDLE_TEST), "-lm", "-o", str(exe)],
                cwd=ROOT, capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, msg=build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, cwd=ROOT)
            self.assertEqual(run.returncode, 0, msg=run.stdout + run.stderr)
            self.assertIn("IDLE TEST OK", run.stdout)


if __name__ == "__main__":
    unittest.main()
