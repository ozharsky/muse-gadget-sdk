#!/usr/bin/env python3
"""Fix: the switch gated video_render() on active(), but apply_pending() lives INSIDE
video_render() -- so a pending request was never applied and the video never started.
Call video_render() unconditionally (it is a cheap no-op when idle) and only gate the
face skip on active()."""
import os
import sys

p = os.path.expanduser("~/muse/muse-gadget-sdk/esp32/components/muse/avatar/muse_pixel_switch.c")
t = open(p).read()
a = ("    if (muse_pixel_video_active()) {\n"
     "        muse_pixel_video_render();\n"
     "        return;\n"
     "    }")
b = ("    muse_pixel_video_render();   /* applies a pending start/stop; no-op when idle */\n"
     "    if (muse_pixel_video_active()) {\n"
     "        return;\n"
     "    }")
if b in t:
    print("already fixed")
    sys.exit(0)
n = t.count(a)
if n != 1:
    sys.exit(f"anchor fail: found {n}")
open(p, "w").write(t.replace(a, b))
open(p).read()  # re-read to prove the write landed
print("fixed switch: video_render() called unconditionally")
