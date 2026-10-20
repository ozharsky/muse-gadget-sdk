#!/usr/bin/env python3
"""Muse gadget doctor: one read-only command that checks the whole chain and prints the fix.

Checks:
  1. Relay  - reachable, token accepted, at least one usable agent
  2. TLS    - the root the firmware pins still validates Google's live TTS chain
  3. Device - (when on USB) wifi / link / battery / volume / faults over the serial console

Exit code 0 when nothing FAILs (warnings are fine), 1 when a check fails.
"""
import argparse
import glob
import json
import os
import re
import ssl
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

PASS, WARN, FAIL, SKIP = "PASS", "WARN", "FAIL", "SKIP"
results = []


def report(tag, status, msg):
    results.append((tag, status, msg))
    print(f"[{status:4}] {tag}: {msg}")


# ---------------------------------------------------------------- relay

def check_relay(url, token):
    if not url or not token:
        return report("relay", SKIP, "no --relay / token given")
    req = urllib.request.Request(url.rstrip("/") + "/agents",
                                 headers={"X-Gadget-Token": token})
    try:
        with urllib.request.urlopen(req, timeout=8) as r:
            data = json.load(r)
    except urllib.error.HTTPError as e:
        if e.code in (401, 403):
            return report("relay", FAIL, f"{url} answered {e.code}: the token is wrong "
                                         "(relay/token on the relay machine)")
        return report("relay", FAIL, f"{url} answered HTTP {e.code}")
    except Exception as e:
        return report("relay", FAIL, f"{url} unreachable: {e} "
                                     "(is Muse_Relay running? is the address right?)")
    agents = data.get("agents") or []
    usable = [a for a in agents if a.get("usable", True)]
    if not usable:
        return report("relay", FAIL, f"reachable, but no usable agent in {url}/agents")
    names = ", ".join(a.get("name", "?") for a in usable)
    report("relay", PASS, f"{url} ok, agents: {names}")


# ---------------------------------------------------------------- TLS pin

def parse_pinned_roots(firmware_dir):
    src = os.path.join(firmware_dir, "components/muse/muse_chat_session.cpp")
    if not os.path.isfile(src):
        return None
    text = open(src, encoding="utf-8", errors="replace").read()
    m = re.search(r"static const char TTS_ROOTS\[\]\s*=\s*(.*?);", text, re.S)
    if not m:
        return None
    block = m.group(1)
    pems = []
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', block):
        try:
            s = lit.encode().decode("unicode_escape")
        except UnicodeDecodeError:
            s = lit
        pems.append(s)
    certs = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
                       "".join(pems), re.S)
    return certs or None


def check_tls_pin(firmware_dir):
    if not firmware_dir:
        return report("tls", SKIP, "no --firmware-dir given")
    if not subprocess.run(["openssl", "version"], capture_output=True).returncode == 0:
        return report("tls", SKIP, "openssl not on PATH")
    pinned = parse_pinned_roots(firmware_dir)
    if not pinned:
        return report("tls", FAIL, "TTS_ROOTS not found in muse_chat_session.cpp "
                                   "(did the pin change?)")
    try:
        out = subprocess.run(
            ["bash", "-lc",
             "echo | openssl s_client -connect texttospeech.googleapis.com:443 "
             "-servername texttospeech.googleapis.com -showcerts 2>/dev/null"],
            capture_output=True, text=True, timeout=20).stdout
    except Exception as e:
        return report("tls", WARN, f"could not fetch the live chain: {e}")
    chain = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", out, re.S)
    if not chain:
        return report("tls", WARN, "no chain returned by texttospeech.googleapis.com")
    with tempfile.TemporaryDirectory() as d:
        ca = os.path.join(d, "pinned.pem")
        leaf = os.path.join(d, "leaf.pem")
        inter = os.path.join(d, "inter.pem")
        open(ca, "w").write("\n".join(pinned) + "\n")
        open(leaf, "w").write(chain[0] + "\n")
        open(inter, "w").write("\n".join(chain[1:]) + "\n")
        r = subprocess.run(["openssl", "verify", "-CAfile", ca, "-untrusted", inter, leaf],
                           capture_output=True, text=True)
    if r.returncode == 0:
        report("tls", PASS, "the pinned root still validates Google's TTS chain")
    else:
        report("tls", FAIL,
               "PIN ROTATED: the firmware's TTS root no longer validates the live chain "
               f"({(r.stderr or r.stdout).strip().splitlines()[-1] if (r.stderr or r.stdout).strip() else 'verify failed'}). "
               "Muse replies will be silent until re-pinned: grab the chain with "
               "`openssl s_client -showcerts`, take the self-signed root from "
               "components/mbedtls/esp_crt_bundle/cacrt_all.pem, replace TTS_ROOTS.")


# ---------------------------------------------------------------- device

def find_port(explicit):
    if explicit:
        return explicit
    for pat in ("/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    return None


def check_device(port, wait=6.0):
    port = find_port(port)
    try:
        import serial  # pyserial: optional dependency
    except ImportError:
        return report("device", SKIP, "pyserial not installed (pip install pyserial)")
    if not port:
        return report("device", SKIP, "no device on USB")
    try:
        s = serial.Serial(port, 115200, timeout=0.3)
        s.dtr = False
        s.rts = False
    except Exception as e:
        return report("device", FAIL, f"{port}: {e}")
    try:
        import time
        time.sleep(2.5)            # opening the port resets the board: let it boot first
        s.reset_input_buffer()
        best = None
        for attempt in range(4):
            s.write(b">status\n")
            end = time.time() + 5.0
            buf = b""
            while time.time() < end:
                buf += s.read(4096)
            text = buf.decode("utf-8", "replace")
            got = next((l for l in reversed(text.splitlines()) if "@status" in l), None)
            if not got:
                continue
            try:
                st_try = json.loads(got.split("@status", 1)[1].strip())
            except Exception:
                continue
            best = (got, st_try)
            wifi_try = st_try.get("wifi") or st_try.get("device", {}).get("wifi") or {}
            if wifi_try.get("state") == "connected":
                break
            time.sleep(2.0)
    finally:
        s.close()
    if not best:
        return report("device", FAIL, f"{port}: no @status answer (cable? wrong port? "
                                      "another program holding it?)")
    line, st = best
    wifi = st.get("wifi") or st.get("device", {}).get("wifi") or {}
    relay = st.get("relay", {})
    agent = st.get("agent", {})
    battery = st.get("device", {}).get("battery")
    volume = st.get("volume")
    state = wifi.get("state")
    if not state:
        return report("device", FAIL, f"{port}: @status came back without a wifi block: "
                                      f"{line[:120]}")
    if state == "connected":
        report("device", PASS, f"{port}: agent={agent.get('name')} wifi={state} "
                               f"rssi={wifi.get('rssi')} dBm battery={battery}% "
                               f"volume={volume}")
    else:
        report("device", FAIL, f"{port}: wifi state '{state}' - on the gadget: "
                               "Settings > Wi-Fi > pick the network > type the password")
    if st.get("link", {}).get("state"):
        report("device", WARN if st["link"]["state"] == "Connecting" else PASS,
               f"link: {st['link']['state']} (paired={st['link'].get('paired')})")
    if volume == 0:
        report("device", WARN, "volume is 0 - nothing will be audible")
    if isinstance(battery, int) and 0 <= battery < 15:
        report("device", WARN, f"battery low: {battery}%")
    if not relay.get("token") and not relay.get("lan") and not relay.get("remote"):
        report("device", PASS, "relay settings empty - turns use the compiled relay address "
                               "(normal unless you set one at runtime)")


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description="Muse gadget doctor (read-only)")
    ap.add_argument("--relay", default=os.environ.get("MUSE_RELAY_URL", ""),
                    help="relay base URL, e.g. http://127.0.0.1:8788")
    ap.add_argument("--token-file", default=os.environ.get("MUSE_RELAY_TOKEN_FILE", ""),
                    help="file holding the relay token")
    ap.add_argument("--token", default=os.environ.get("MUSE_RELAY_TOKEN", ""),
                    help="relay token (prefer --token-file)")
    ap.add_argument("--firmware-dir", default="",
                    help="path to esp32/ in the repo (for the TLS pin check)")
    ap.add_argument("--port", default=os.environ.get("MUSE_PORT", ""),
                    help="serial port of the gadget")
    ap.add_argument("--skip-relay", action="store_true")
    ap.add_argument("--skip-tls", action="store_true")
    ap.add_argument("--skip-device", action="store_true")
    a = ap.parse_args()

    token = a.token
    if not token and a.token_file and os.path.isfile(a.token_file):
        token = open(a.token_file).read().strip()

    if not a.skip_relay:
        check_relay(a.relay, token)
    if not a.skip_tls:
        check_tls_pin(a.firmware_dir)
    if not a.skip_device:
        check_device(a.port)

    fails = [r for r in results if r[1] == FAIL]
    print()
    print(f"{len(results)} checks: "
          f"{sum(1 for r in results if r[1] == PASS)} pass, "
          f"{sum(1 for r in results if r[1] == WARN)} warn, {len(fails)} fail")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
