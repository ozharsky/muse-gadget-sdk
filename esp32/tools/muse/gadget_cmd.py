#!/usr/bin/env python3
"""Send one console line (INCLUDE the > prefix) and print the board's answer for N seconds.

  python3 gadget_cmd.py /dev/cu.usbmodem1101 ">status" [secs]
"""
import sys
import time

import serial

port, line = sys.argv[1], sys.argv[2]
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0
s = serial.Serial(port, 115200, timeout=0.2)
time.sleep(0.3)
s.reset_input_buffer()
s.write((line + "\n").encode())
end = time.time() + secs
buf = b""
while time.time() < end:
    buf += s.read(65536)
print(buf.decode("latin1"))
