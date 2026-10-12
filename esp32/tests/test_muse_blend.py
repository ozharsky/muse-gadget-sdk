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

"""muse_blend.h is the cross-mood blend: a weight vector over the moods a face has
live at once, so a second [react:] arriving mid-flight cross-fades instead of
cutting. tools/muse/blend_test.c pins the module invariants (single-mood parity,
the eased hand-over, eviction, determinism); the second case here drives one real
mood pair through the golem renderer and requires the pair's fire-frame jump to
stay at or below the same mood fired solo -- the eased baseline (method: see
tools/muse/react_motion.py --sweep).
"""

from __future__ import annotations

import math
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BLEND_TEST = ROOT / "tools" / "muse" / "blend_test.c"
REACT_DEMO = ROOT / "tools" / "muse" / "react_demo.c"
GOLEM = ROOT / "components" / "muse" / "avatar" / "muse_pixel_golem.c"

DT = 0.04
MOVE = 30


def _cc() -> list[str]:
    cc = shlex.split(os.environ.get("CC", "cc"))
    if not cc or shutil.which(cc[0]) is None:
        raise unittest.SkipTest("C compiler not available")
    return cc


class BlendMath(unittest.TestCase):
    """tools/muse/blend_test.c: the blend's own invariants."""

    def test_blend_math_passes(self) -> None:
        cc = _cc()
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "blend_test"
            build = subprocess.run(
                [*cc, "-O2", "-Wall", "-I", "components/muse/avatar",
                 str(BLEND_TEST), "-lm", "-o", str(exe)],
                cwd=ROOT, capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, msg=build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, cwd=ROOT)
            self.assertEqual(run.returncode, 0, msg=run.stdout + run.stderr)
            self.assertIn("BLEND TEST OK", run.stdout)


class BlendPairRenderer(unittest.TestCase):
    """One mood fired over another must not out-step its own solo fire."""

    @staticmethod
    def _frames(exe: str, tmp: str, tag: str, events, total: float):
        import numpy as np
        from PIL import Image

        out = os.path.join(tmp, tag)
        args = [exe, out, events[0][0], f"{events[0][1]}", f"{total}"]
        if len(events) > 1:
            args += [events[1][0], f"{events[1][1]}"]
        subprocess.run(args, check=True, capture_output=True, text=True)
        return [np.asarray(Image.open(os.path.join(out, f)).convert("RGB")
                           .resize((64, 64), Image.NEAREST)).astype(int)
                for f in sorted(os.listdir(out))]

    def test_pair_fire_matches_the_solo_baseline(self) -> None:
        try:
            import numpy as np
            from PIL import Image  # noqa: F401
        except ImportError:
            raise unittest.SkipTest("numpy/Pillow not available")
        cc = _cc()
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "rdemo")
            build = subprocess.run(
                [*cc, "-O2", "-w", "-I", "components/muse", str(REACT_DEMO),
                 str(GOLEM), "-lm", "-o", exe],
                cwd=ROOT, capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, msg=build.stdout + build.stderr)

            fire, fire2, total = 1.5, 3.0, 4.4
            pair = self._frames(exe, tmp, "pair", [("angry", fire), ("smirk", fire2)], total)
            base = self._frames(exe, tmp, "base", [("smirk", fire2)], total)
            asolo = self._frames(exe, tmp, "asolo", [("angry", fire)], total)

            def mv_of(px):
                mv = [int((np.abs(px[i] - px[i - 1]).sum(2) > MOVE).sum())
                      for i in range(1, len(px))]
                return [mv[0]] + mv

            f2 = int(math.ceil(fire2 / DT - 1e-9))
            mv_pair, mv_base, mv_asolo = mv_of(pair), mv_of(base), mv_of(asolo)
            amb = lambda mv: float(np.median(mv[max(0, f2 - 12):max(1, f2 - 2)]))
            jump_pair = mv_pair[f2] - amb(mv_pair)
            jump_base = mv_base[f2] - amb(mv_base)
            a_amb = max(mv_asolo[max(0, f2 - 40):f2])
            # the blend's claim: angry handing over to smirk must not jump more than
            # smirk's solo fire does, beyond dither granularity + angry's own motion.
            self.assertLessEqual(
                jump_pair, jump_base + a_amb + 150,
                msg=f"pair fire jump {jump_pair} vs baseline {jump_base} (a_amb {a_amb})",
            )


if __name__ == "__main__":
    unittest.main()
