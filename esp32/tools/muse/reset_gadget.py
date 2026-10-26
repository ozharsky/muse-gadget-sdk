#!/usr/bin/env python3
"""Reset the gadget / read its state from the Mac. The recovery tool for a frozen device.

WHY: the 143C board has NO reset button (schematic-verified: only BOOT and the
hardware-only PWR switch, and the PWR switch on this unit is dead). If the firmware
ever hangs before its own watchdog fires, these are the ways back in:

  reset   hardware reset over USB-Serial/JTAG -- works even when the firmware is
          fully hung (the reset is done by the chip's USB peripheral, not by code)
  reboot  the firmware's own ">reboot" console command (works when the input task
          is alive but the screen/UI is not)
  boot    hold GPIO0 low through a reset = ROM download mode (for esptool work)
  status  a quick health read (wifi / agents / battery)

Usage:
  python3 tools/muse/reset_gadget.py reset
  python3 tools/muse/reset_gadget.py reboot
  python3 tools/muse/reset_gadget.py boot
  python3 tools/muse/reset_gadget.py status
"""
import glob
import os
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:  # the system python lacks pyserial; the IDF venv has it
    import importlib
    _sp = glob.glob(os.path.expanduser(
        "~/.espressif/python_env/idf*/lib/python*/site-packages"))
    if _sp:
        sys.path.insert(0, _sp[0])
    import serial
    import serial.tools.list_ports


def find_port():
    """The board's USB Serial/JTAG console port (0x303A:0x1001), or the sole usbmodem."""
    for p in serial.tools.list_ports.comports():
        if (p.vid, p.pid) == (0x303A, 0x1001):
            return p.device
    cands = glob.glob("/dev/cu.usbmodem*")
    if len(cands) == 1:
        return cands[0]
    raise SystemExit("no gadget on USB (or several usbmodem ports: %s)" % ", ".join(cands))


def reset_hard(port):
    """Pulse DTR/RTS the way esptool does: the USB-Serial/JTAG peripheral pulls the
    chip's reset line in hardware, so this works on a hung firmware."""
    s = serial.Serial(port, 115200, timeout=0.2)
    s.dtr = False
    s.rts = True
    time.sleep(0.15)
    s.rts = False
    s.close()
    print("reset pulsed on %s" % port)


def enter_boot(port):
    """Hold GPIO0 (BOOT) low through a reset -> ROM download bootloader."""
    s = serial.Serial(port, 115200, timeout=0.2)
    s.dtr = True       # DTR drives GPIO0 on USB-Serial/JTAG
    s.rts = True
    time.sleep(0.15)
    s.rts = False      # reset released while GPIO0 is held low
    time.sleep(0.1)
    s.dtr = False
    s.close()
    print("download mode requested on %s (use esptool, then reset)" % port)


def console(port, command, want, timeout=6.0):
    """Send one console line and return the lines that answer it."""
    s = serial.Serial(port, 115200, timeout=0.2)
    time.sleep(0.4)
    s.reset_input_buffer()
    s.write(b">" + command.encode() + b"\n")
    out, end = [], time.time() + timeout
    while time.time() < end:
        line = s.readline().decode(errors="ignore").strip()
        if line:
            out.append(line)
            if want and want in line:
                break
    s.close()
    return out


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("reset", "reboot", "boot", "status"):
        print(__doc__.strip())
        return 2
    port = find_port()
    what = sys.argv[1]
    if what == "reset":
        reset_hard(port)
    elif what == "boot":
        enter_boot(port)
    elif what == "reboot":
        for line in console(port, "reboot", "@reboot"):
            print(" ", line)
        # the chip restarts; confirm it comes back
        time.sleep(3)
        for line in console(port, "status", "@status"):
            print(" ", line[:200])
    else:
        for line in console(port, "status", "@status"):
            print(" ", line[:400])
    return 0


if __name__ == "__main__":
    sys.exit(main())
