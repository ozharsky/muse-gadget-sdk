#!/usr/bin/env python3
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
"""Collect the gadget's log history over USB serial.

Plug the gadget in and run:
    tools/muse/logdump.py                     # newest ring into gadget.log
    tools/muse/logdump.py -o crash.log        # name the file
    tools/muse/logdump.py -p /dev/cu.usbmodem1101

It saves the current RAM ring to flash (>log.save) and then dumps the flash
ring (>log.dump), so the file holds this boot plus everything kept from
earlier runs. Uses the IDF venv python if plain python3 lacks pyserial.
"""

from __future__ import annotations

import argparse
import glob
import os
import sys
import time

try:
    import serial
except ImportError:
    venv = os.path.expanduser("~/.espressif/python_env/idf6.0_py3.13_env/bin/python")
    if os.path.exists(venv) and sys.executable != venv:
        os.execv(venv, [venv, *sys.argv])
    sys.exit("pyserial not available (run with the IDF venv python)")

BAUD = 115200


def find_port() -> str | None:
    for pat in ("/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        hits = glob.glob(pat)
        if len(hits) == 1:
            return hits[0]
        if len(hits) > 1:
            return sorted(hits)[0]
    return None


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default=None)
    ap.add_argument("-o", "--out", default="gadget.log")
    ap.add_argument("--keep-boot-noise", action="store_true")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("no serial port found; pass -p /dev/cu.usbmodemXXXX")

    s = serial.Serial(port, BAUD, timeout=0.2)
    try:
        s.setDTR(False)
        s.setRTS(False)
    except Exception:
        pass
    time.sleep(2)

    def send(line: str) -> None:
        s.write(("\r\n" + line + "\r\n").encode())

    send(">log.save")
    time.sleep(3)
    s.reset_input_buffer()
    send(">log.dump")
    rows = []
    t0 = time.time()
    done = False
    while time.time() - t0 < 30 and not done:
        chunk = s.read(8192)
        if chunk:
            rows.append(chunk)
        done = b"@end.log.dump" in b"".join(rows[-3:])
    s.close()

    text = b"".join(rows).decode("utf-8", "replace")
    start = text.find("@log.dump")
    if start >= 0:
        text = text[start:]
    end = text.find("@end.log.dump")
    if end >= 0:
        text = text[:end]
    with open(args.out, "w", encoding="utf-8", errors="replace") as f:
        f.write(text)
    print(f"wrote {args.out} ({len(text)} bytes)")


if __name__ == "__main__":
    main()
