#!/usr/bin/env python3
"""Flash an app image, then guard the boot: a panic, assert, stack overflow or a
second boot inside the watch window flashes a known-good image back at once.

A boot loop on this board can revoke the Muse pairing, so never flash risky
changes without a rollback image to hand.

Usage:
  python3 tools/muse/safeflash.py NEW.bin GOOD.bin [watch_s] [PORT]

  NEW.bin   build-<board>/muse-gadget.bin you want to run
  GOOD.bin  a previously verified image (build one from a clean HEAD:
            git stash; build; copy the bin; git stash pop)
  watch_s   seconds to watch the boot (default 30)
  PORT      serial port; default: the only /dev/cu.usbmodem* present

Requires pyserial and an esptool-capable Python (the ESP-IDF venv works:
~/.espressif/python_env/idf6.0_py3.13_env/bin/python). Writes otadata + the app
to the ota_0 slot; NVS (pairing, Wi-Fi, secrets) is untouched.
"""
import glob
import subprocess
import sys
import time

import serial

port = sys.argv[4] if len(sys.argv) > 4 else (glob.glob("/dev/cu.usbmodem*") or [None])[0]
if not port:
    sys.exit("no serial port (pass it as the fourth argument)")
new, good = sys.argv[1], sys.argv[2]
watch = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0


def flash(img):
    args = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", port,
            "-b", "921600", "write-flash", "--flash-mode", "dio", "--flash-freq", "80m"]
    # the build dir also holds the factory otadata this slot layout expects
    ota = img.rsplit("/", 1)[0] + "/ota_data_initial.bin"
    if glob.glob(ota):
        args += ["0x1d000", ota]
    args += ["0x20000", img]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-400:], r.stderr[-400:])
    return r.returncode == 0


if not flash(new):
    sys.exit("flash failed")
s = serial.Serial(port, 115200, timeout=0.05)
out, end, bad = b"", time.time() + watch, None
while time.time() < end and not bad:
    out += s.read(4096)
    t = out.decode(errors="replace")
    for k in ("Guru Meditation", "assert failed", "stack overflow",
              "abort() was called", "Rebooting..."):
        if k in t:
            bad = k
s.close()
if bad:
    print("CRASH (%s): restoring the good image" % bad)
    print("\n".join(l for l in out.decode(errors="replace").splitlines()
                    if any(k in l for k in ("assert", "Guru", "overflow", "abort", "Backtrace")))[:800])
    print("restored" if flash(good) else "RESTORE FAILED")
    sys.exit(1)
print("booted clean for %.0f s" % watch)
