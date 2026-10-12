#!/usr/bin/env python3
"""Catch a notification being presented on the gadget and snapshot DURING its spoken line
(the mouth should be visibly open -- speaking mode drives the lip states through muse_lip).

Run on the Mac with the IDF venv python. The notification must already be queued on the
relay (POST /notifications) and the device must land on a hermes-mode agent, which this
script switches to first (the notification poll only runs on hermes-mode agents).
"""
import os
import sys
import time

import serial

PORT = "/dev/cu.usbmodem1101"
SNAP = os.path.expanduser("~/muse/muse-gadget-sdk/esp32/tools/muse/snap.py")
IDF = os.path.expanduser("~/.espressif/python_env/idf6.0_py3.13_env/bin/python")


def snap(out):
    rc = os.system('env -u PYTHONPATH "%s" "%s" %s "" %s 2>&1 | tail -1' % (IDF, SNAP, PORT, out))
    return rc


s = serial.Serial(PORT, 115200, timeout=0.5)
s.reset_input_buffer()
s.write(b">agent=hermes\n")           # hermes-mode agent: the notification poll runs
time.sleep(1.5)
print("watching for the notification (agent switched)...")
t0 = time.time()
while time.time() - t0 < 330:
    ln = s.readline().decode(errors="ignore").strip()
    if not ln:
        continue
    if "notif" in ln or "tts" in ln or "speaking" in ln:
        print("LOG:", ln[:170])
    if "notif from" in ln:
        s.close()
        t1 = time.time()
        print("presented -- snap NOW (would-be t+0.5s into the turn)")
        snap("/tmp/mouth1.png")
        print("snap1 done t+%.1fs; snap2" % (time.time() - t1))
        snap("/tmp/mouth2.png")
        print("snap2 done t+%.1fs" % (time.time() - t1))
        sys.exit(0)
print("no notification seen")
s.close()
sys.exit(1)
