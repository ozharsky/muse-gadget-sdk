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

"""muse_rig.h's easing law is the base of every face's expression transitions:
it must converge without overshoot, freeze on dt<=0, and hard-cut on tau<=0.
rig_test.c asserts exactly that; this pins it into the suite so a future edit to
the rig cannot silently change how every face moves."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RIG_TEST = ROOT / "tools" / "muse" / "rig_test.c"


def _cc() -> list[str]:
    cc = shlex.split(os.environ.get("CC", "cc"))
    if not cc or shutil.which(cc[0]) is None:
        raise unittest.SkipTest("C compiler not available")
    return cc


class RigTest(unittest.TestCase):
    """tools/muse/rig_test.c: the easing law + the per-channel rig step."""

    def test_rig_math_passes(self) -> None:
        cc = _cc()
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "rig_test"
            build = subprocess.run(
                [*cc, "-O2", "-Wall", "-I", "components/muse/avatar",
                 str(RIG_TEST), "-lm", "-o", str(exe)],
                cwd=ROOT, capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, msg=build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, cwd=ROOT)
            self.assertEqual(run.returncode, 0, msg=run.stdout + run.stderr)
            self.assertIn("RIG TEST OK", run.stdout)


if __name__ == "__main__":
    unittest.main()
