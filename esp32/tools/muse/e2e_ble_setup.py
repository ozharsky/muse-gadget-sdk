#!/usr/bin/env python3
"""End-to-end check of the gadget's setup surface over REAL Bluetooth (BLE).

Drives exactly what tools/muse/ble_setup.html sends for the relay section, plus the
page's Save & restart flow:
  1. reads STATUS (relay block),
  2. sets relay.lan / relay.remote / relay.token (chunked, like the page),
  3. checks the too-long guard,
  4. sends `reboot` (expects the ack, then the device restarts under us),
  5. reconnects and verifies the values PERSISTED through the reboot,
  6. clears them all again.

Requirements: a computer near the gadget with Python + `pip install bleak`.
On macOS run it from Terminal.app: the first run asks to let Terminal use Bluetooth
and shows ONE pairing sheet — type the 6-digit code the gadget displays (the device's
setup page pairing, same as ble_setup.html). Linux uses BlueZ (an agent answers the
passkey prompt); a weak link can make pairing flaky — sit close.
"""
import asyncio
import json
import sys
import time

from bleak import BleakClient, BleakScanner

CMD = "4d757365-0002-4000-8000-6a6f6c6c7900"
STAT = "4d757365-0003-4000-8000-6a6f6c6c7900"

TEST_LAN = "10.0.0.99"
TEST_REMOTE = "https://e2e.example.ts.net:8443"

acks = []


def on_status(_c, data):
    msg = data.decode(errors="ignore")
    print("CHANGE:", msg[:160], flush=True)
    acks.append(msg)


async def find():
    return await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or "").startswith("MuseGadget-"), timeout=20)


async def status(c):
    return json.loads((await c.read_gatt_char(STAT)).decode())


async def main():
    dev = await find()
    print("DEVICE:", dev, flush=True)
    if not dev:
        sys.exit(2)

    print("\n>>> If a pairing sheet appears: type the code shown on the gadget.\n", flush=True)
    async with BleakClient(dev, timeout=90) as c:
        await c.start_notify(STAT, on_status)
        s = await status(c)
        print("STATUS relay:", s.get("relay"), flush=True)
        assert "relay" in s, "no relay block"

        async def send(cmd, val):
            acks.clear()
            await c.write_gatt_char(CMD, f"{cmd}={val}".encode(), response=True)
            t0 = time.time()
            while time.time() - t0 < 8:
                if any(a.startswith(cmd) for a in acks):
                    break
                await asyncio.sleep(0.2)
            print(f"SENT {cmd} ->", (acks[:1] or ["(no ack)"]), flush=True)
            return " ".join(acks)

        await send("relay.lan", TEST_LAN)
        s = await status(c)
        assert s["relay"]["lan"] == TEST_LAN, s["relay"]
        print("[1] relay.lan roundtrip OK", flush=True)

        await send("relay.remote", TEST_REMOTE)
        s = await status(c)
        assert s["relay"]["remote"] == TEST_REMOTE, s["relay"]
        print("[2] relay.remote roundtrip OK", flush=True)

        await send("relay.token", "e2e-1234567890")
        await send("relay.token+", "ABCDEFGHIJ")
        s = await status(c)
        assert s["relay"]["token"] is True, s["relay"]
        print("[3] relay.token chunked set OK", flush=True)

        r = await send("relay.token", "x" * 300)
        assert "error" in r, r
        print("[4] too-long guard OK", flush=True)

        # The reboot write ends in a disconnect BY DESIGN: the device restarts under us.
        try:
            r = await send("reboot", "")
            print("[5] reboot ack:", r[:80], flush=True)
        except Exception as e:   # noqa: BLE001
            print("[5] reboot ack:", (acks[:1] or [type(e).__name__]),
                  "(disconnect after reboot is expected)", flush=True)
        await asyncio.sleep(6)

    print("waiting for the gadget to reboot...", flush=True)
    await asyncio.sleep(14)
    ok = False
    for attempt in range(12):
        try:
            dev2 = await find()
            if not dev2:
                raise RuntimeError("not visible yet")
            async with BleakClient(dev2, timeout=25) as c2:
                s = await status(c2)
                assert s["relay"]["token"] is True, s["relay"]
                assert s["relay"]["remote"] == TEST_REMOTE, s["relay"]
                print("[6] values persisted through reboot OK", flush=True)
                await c2.write_gatt_char(CMD, b"relay.lan=", response=True)
                await asyncio.sleep(0.6)
                await c2.write_gatt_char(CMD, b"relay.remote=", response=True)
                await asyncio.sleep(0.6)
                await c2.write_gatt_char(CMD, b"relay.token=", response=True)
                await asyncio.sleep(0.6)
                s = await status(c2)
                assert s["relay"] == {"lan": "", "remote": "", "token": False}, s["relay"]
                print("[7] restored: relay cleared, token false", flush=True)
                ok = True
                break
        except Exception as e:   # noqa: BLE001
            print("reconnect attempt", attempt, type(e).__name__, e, flush=True)
            await asyncio.sleep(6)

    print("\nE2E", "PASS" if ok else "FAIL", flush=True)
    sys.exit(0 if ok else 1)


asyncio.run(main())
