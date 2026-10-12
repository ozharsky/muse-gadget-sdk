"""Assemble golem GIFs (states + 52-mood showcase) from the REAL renderer's frames."""
import glob
import os
import subprocess

from PIL import Image

PRE = os.path.expanduser("~/muse/art-pipeline/faces_v2/stage_b/preview/esp32")
OUT = os.path.expanduser("~/muse/art-pipeline/faces_v2/stage_b/sheets/golem")
SRC = os.path.join(PRE, "components/muse/avatar/muse_pixel_golem.c")
EXE = "/tmp/da_golem_gif"

STATES = ["boot", "idle", "listening", "thinking", "speaking", "happy", "off", "error"]
MOODS = ["smirk", "scan", "alert", "glitch", "squint", "angry", "surprised", "pleased", "laugh", "wink",
         "sleepy", "confused", "evil", "hacked", "powerup", "bored", "disgust", "target", "nervous",
         "proud", "excited", "confetti", "sunny", "rain", "storm", "cold", "rainbow", "searching",
         "browsing", "reading", "writing", "checking", "calculating", "calendar", "monitoring",
         "generating", "running", "invoking", "delegating", "remembering", "shrug", "embarrassed",
         "scared", "cool", "mind_blown", "crying_laughing", "sad", "nod", "cloudy", "windy", "hot", "snow"]
DUR = {"smirk": 2.8, "scan": 2.6, "alert": 2.2, "glitch": 1.8, "squint": 2.6, "angry": 2.6, "surprised": 1.8,
       "pleased": 2.6, "laugh": 2.4, "wink": 1.6, "sleepy": 3.4, "confused": 2.6, "evil": 3.0, "hacked": 2.4,
       "powerup": 2.4, "bored": 3.2, "disgust": 2.4, "target": 2.6, "nervous": 2.6, "proud": 2.8,
       "excited": 2.8, "confetti": 3.4, "sunny": 3.0, "rain": 3.2, "storm": 3.2, "cold": 3.0, "rainbow": 3.2,
       "searching": 2.6, "browsing": 3.0, "reading": 3.0, "writing": 3.0, "checking": 2.4, "calculating": 3.0,
       "calendar": 3.0, "monitoring": 2.8, "generating": 3.0, "running": 3.0, "invoking": 2.8,
       "delegating": 2.8, "remembering": 2.8, "shrug": 2.6, "embarrassed": 2.8, "scared": 2.6, "cool": 2.8,
       "mind_blown": 2.8, "crying_laughing": 2.8, "sad": 3.0, "nod": 2.4, "cloudy": 3.0, "windy": 3.0,
       "hot": 3.0, "snow": 3.4}


def run():
    subprocess.run(["cc", "-O2", "-w", "-I", os.path.join(PRE, "components/muse"),
                    os.path.join(PRE, "tools/muse/dump_anim.c"), SRC, "-lm", "-o", EXE], check=True)
    d = "/tmp/golem_gif_frames"
    subprocess.run(["rm", "-rf", d])
    print(subprocess.run([EXE, d, "3", "36", "state"] + STATES, capture_output=True, text=True).stderr.splitlines()[-8:])
    subprocess.run([EXE, d, "3", "7", "mood"] + ["%s:%s" % (m, DUR[m]) for m in MOODS],
                   capture_output=True, text=True)


def frames(d, name, keep=None):
    fs = sorted(glob.glob(os.path.join(d, name, "*.ppm")))
    if keep:
        step = max(1, len(fs) // keep)
        fs = fs[::step][:keep]
    return [Image.open(f).convert("RGB") for f in fs]


def gif(seq, path, scale, delay):
    seq = [im.resize((64 * scale, 64 * scale), Image.NEAREST) for im in seq]
    seq[0].save(path, save_all=True, append_images=seq[1:], duration=delay, loop=0, optimize=True)
    print(path, len(seq), "frames", os.path.getsize(path) // 1024, "KB")


if __name__ == "__main__":
    run()
    d = "/tmp/golem_gif_frames"
    seq = []
    for s in STATES:
        seq += frames(d, s, keep=20)
    gif(seq, os.path.join(OUT, "golem_states.gif"), 3, 120)
    seq = []
    for m in MOODS:
        seq += frames(d, m, keep=7)
    gif(seq, os.path.join(OUT, "golem_moods.gif"), 3, 150)
