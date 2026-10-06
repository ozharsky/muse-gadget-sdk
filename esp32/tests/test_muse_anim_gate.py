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

"""The animation preview tooling can't pass a broken render silently:
anim.c exits non-zero when an animation draws nothing, and make_gifs.py --gate
exits 1 when PIL merged dead (identical) frames out of the GIF. The renderers in
tests/avatar_fakes/ are deterministic stand-ins for a bad and a good avatar."""

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
FAKES = ROOT / "tests" / "avatar_fakes"


def _cc() -> list[str]:
    cc = shlex.split(os.environ.get("CC", "cc"))
    if not cc or shutil.which(cc[0]) is None:
        raise unittest.SkipTest("C compiler not available")
    return cc


def _have_pillow() -> bool:
    try:
        import PIL  # noqa: F401
    except ImportError:
        return False
    return True


class AnimDriverTest(unittest.TestCase):
    """anim.c's exit status: non-zero when an animation rendered no frames."""

    def test_stops_rendering_when_a_directory_cannot_be_written(self) -> None:
        cc = _cc()
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            exe = tmp / "anim"
            proc = subprocess.run(
                [*cc, "-O2", "-Wall", "-I", "components/muse",
                 "tools/muse/anim.c", "avatar/muse_pixel.c", "-lm", "-o", str(exe)],
                cwd=ROOT, capture_output=True, text=True,
            )
            self.assertEqual(proc.returncode, 0, msg=proc.stdout + proc.stderr)

            frames = tmp / "frames"
            frames.mkdir()
            # A regular file where the first animation's directory should go: boot
            # can't create it and writes zero frames.
            (frames / "boot").write_text("not a directory")

            run = subprocess.run([str(exe), str(frames)], capture_output=True, text=True)
            self.assertNotEqual(run.returncode, 0, msg=run.stderr)
            self.assertIn("boot rendered no frames", run.stderr)
            self.assertIn("anim: boot", run.stderr)
            self.assertIn("0 frames", run.stderr)
            # The animations after the broken one still ran.
            self.assertIn("anim: idle", run.stderr)
            self.assertIn("150 frames", run.stderr)

    def test_nonzero_when_the_renderer_dies(self) -> None:
        cc = _cc()
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            exe = tmp / "anim"
            proc = subprocess.run(
                [*cc, "-O2", "-Wall", "-I", "components/muse",
                 "tools/muse/anim.c", str(FAKES / "crash_pixel.c"), "-lm", "-o", str(exe)],
                cwd=ROOT, capture_output=True, text=True,
            )
            self.assertEqual(proc.returncode, 0, msg=proc.stdout + proc.stderr)

            frames = tmp / "frames"
            frames.mkdir()
            run = subprocess.run([str(exe), str(frames)], capture_output=True, text=True)
            self.assertNotEqual(run.returncode, 0)


@unittest.skipUnless(_have_pillow(), "Pillow not installed")
class GifGateTest(unittest.TestCase):
    """make_gifs.py --gate: exit 1 on merged (dead) frames, 0 on a clean render."""

    def setUp(self) -> None:
        _cc()
        self.tmp = tempfile.TemporaryDirectory()
        self.out = Path(self.tmp.name)
        self.script = str(ROOT / "tools" / "muse" / "make_gifs.py")
        self.env = dict(os.environ)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_gate(self, fake: str, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, self.script, "--src", str(FAKES / fake), "--gate", str(self.out / fake), *extra],
            cwd=ROOT, capture_output=True, text=True, env=self.env,
        )

    def test_gate_fails_on_a_renderer_with_dead_frames(self) -> None:
        run = self.run_gate("flat_pixel.c")
        self.assertEqual(run.returncode, 1, msg=run.stdout + run.stderr)
        self.assertIn("dead frames", run.stderr)
        # The GIF really holds fewer frames than the driver drew.
        self.assertIn("1 unique", run.stderr)
        gif = self.out / "flat_pixel.c" / "idle.gif"
        self.assertTrue(gif.exists())
        from PIL import Image
        with Image.open(gif) as im:
            self.assertEqual(im.n_frames, 1)
        self.assertEqual(self.run_gate("flat_pixel.c").returncode, 1)   # repeatable

    def test_gate_passes_on_a_clean_renderer(self) -> None:
        run = self.run_gate("clean_pixel.c")
        self.assertEqual(run.returncode, 0, msg=run.stdout + run.stderr)
        self.assertNotIn("dead frames", run.stderr)
        from PIL import Image
        # Every drawn frame survived the GIF.
        with Image.open(self.out / "clean_pixel.c" / "idle.gif") as im:
            self.assertEqual(im.n_frames, 150)

    def test_without_gate_it_only_warns(self) -> None:
        run = subprocess.run(
            [sys.executable, self.script, "--src", str(FAKES / "flat_pixel.c"), str(self.out / "warn")],
            cwd=ROOT, capture_output=True, text=True, env=self.env,
        )
        self.assertEqual(run.returncode, 0, msg=run.stdout + run.stderr)
        self.assertIn("WARNING", run.stderr)
        self.assertIn("dead frames", run.stderr)


if __name__ == "__main__":
    unittest.main()
