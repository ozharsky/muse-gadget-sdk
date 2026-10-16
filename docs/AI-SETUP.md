# Hand this repo to your AI — full setup runbook

You are an AI assistant reading this because a human did exactly what the heading says.
Follow this file top to bottom. Run every command yourself, verify every checkpoint,
and ask the human only for things only a human has: the board on a cable, a Muse SDK
token *if* they want the Muse cloud agent, and which of the three setups below they
want. Do not invent credentials, do not skip the checkpoints.

## 0. Decide with the human — three ways to use the gadget

| Way | What it needs | Pick when |
|---|---|---|
| **A. Muse cloud only** | Flash + the Muse app on their phone, **or a computer browser** (the SDK's setup page — see §4). Wi-Fi and account tokens arrive **over Bluetooth during pairing** (they can also enter Wi-Fi on the device itself: `Settings → Wi-Fi`). | They just want the stock Muse companion. No extra machine, no relay. |
| **B. A self-hosted agent** (Hermes, or anything else) | Flash + `relay/` running on an always-on Mac/Windows/Linux box + two config strings baked into the firmware. | They want their *own* agent answering. |
| **C. Both** | Everything from A and B. | They want the fork's three-agent switching (Muse / their agent / a second agent). |

## 1. Prerequisites (on the human's computer)

- **git** and **Python 3.10+**.
- **ESP-IDF 6.0** — install from Espressif's official installer (macOS/Linux/WSL and
  Windows are all supported; Windows: use the official installer, not a random Python).
  Then source its export script in every shell you use (`~/esp/esp-idf-v6/export.sh` on
  macOS/Linux, or the ESP-IDF terminal shortcut on Windows).
- A **USB data cable** and the **Waveshare ESP32-S3 1.43″ round AMOLED** board.
- If they chose A/C: a free **Muse SDK token** from https://gadgets.muse.ai/settings/sdk-tokens
  (Account → SDK tokens). The build also succeeds without it — you just cannot pair.
No token at build time? Set one after flashing: `>sdk_token=mgst_…` on the serial console stores
it in NVS, acks and restarts — the stored token is preferred over the build one. It is
serial-only on purpose (nobody nearby can change a gadget's token over Bluetooth).

## 2. Clone and build

```bash
git clone https://github.com/ozharsky/muse-gadget-sdk && cd muse-gadget-sdk
cd esp32
idf.py set-target esp32s3          # once per checkout
idf.py menuconfig                   # ESP32 Device SDK → board profile: Waveshare 1.43C,
                                    # CONFIG_GADGET_SDK_TOKEN (if using Muse), and — for
                                    # way B/C — CONFIG_MUSE_HERMES_URL / _REMOTE / _TOKEN
# plug the board in, find the port (`idf.py -p PORT flash`), then:
idf.py -p <port> flash
```

Checkpoints: cmake ends with no `FATAL_ERROR` (a missing SDK token is only a *warning*);
esptool ends with `Hash of data verified`. If the port is not found, it is almost always
the cable (charge-only) or a missing driver.

## 3. First boot

Open a serial monitor at **115200** (`idf.py -p <port> monitor`, Ctrl-] to exit).
Verify, in order:

1. Boot banner with `Compile time:` — proves the flash landed and restarted.
2. `link.heartbeat` lines every 5 s with `wifi=...` — proves radio + power are sane.
3. Type `>status` — a JSON line. `setup=advertising` means it is waiting to pair; that is
   the normal fresh state.
4. Wi-Fi: either let the Muse app deliver it during pairing (way A), on the device
   itself (`Settings → Wi-Fi` → pick the network → type the password on the keyboard),
   or from a computer's browser (the setup page — see §4).

## 4. Connect the agent

**Way A — Muse (no relay):** On the phone: Muse app → Settings → Devices → turn on
Developer mode → look for `MuseGadget-…` → pair. The app sends Wi-Fi credentials and the
account tokens **over Bluetooth** as part of this flow; the device reboots by design
once, then its heartbeat turns into `setup=done wifi=up ws=up ble=off`. Talk to it.

**Way A — from a computer instead of a phone (browser + Bluetooth — ships in the
repo, nothing to install):** The SDK includes a setup page that talks to the device's
Bluetooth setup service directly: `tools/muse/ble_setup.html`.
1. On the device: `Settings → Bluetooth → Phone setup` → ON.
2. On the computer: `cd esp32/tools/muse && python3 -m http.server 8000`, then open
   `http://localhost:8000/ble_setup.html` in Chrome (desktop or Android; on iOS use the
   Bluefy browser — Web Bluetooth needs https/localhost).
3. Click **Connect**; when the OS asks to pair, type the 6-digit code shown on the
   device's screen.
4. In the page: **Wi-Fi** (network name + password → Join) and **Muse** (Device token →
   Save → Test connection; VM ID optional). It also carries volume/mic/brightness/sleep.
The page is a file in the repo; the device side is all firmware. No phone, no relay.
5. If they chose way B/C, use the same page's **Your own agent** section to point the
   gadget at their relay: Relay host (home network), Relay base URL (from anywhere,
   optional) and the relay token — then **Save & restart gadget**. The device reboots
   and routes turns to the relay; no rebuild, no serial. (Newest firmware; older builds
   need the config values at build time.)
6. Verify the relay section end-to-end over Bluetooth (an AI can run this largely
   unattended): `esp32/tools/muse/e2e_ble_setup.py` from Terminal.app on a computer near
   the gadget — the first run asks for Bluetooth permission and shows one pairing sheet
   (type the code the device displays); the script then sets, checks, reboots and clears
   the relay values.

**Way B/C — your own agent via the relay:** On the always-on machine:
1. Copy `relay/` out of the repo and follow `relay/README.md` (Windows: `relay-launch.cmd`;
   macOS/Linux: `relay-launch.sh`; Python stdlib only). It creates a `token` file.
2. Back on the computer with the board: put the machine's address and that token into the
   board profile (`CONFIG_MUSE_HERMES_URL`, and `_REMOTE` too if the machine is reachable
   from outside, e.g. a Tailscale funnel; `CONFIG_MUSE_HERMES_TOKEN`), rebuild, reflash.
   Or seed the address live over serial: `>relay.lan=<machine-ip>`.
   On the newest firmware, skip the rebuild entirely: set all of it in the setup page's
   **Your own agent** section and press **Save & restart gadget** (see §4).
3. On the gadget: `>agent=hermes`, then talk. The relay's console should print
   `heard: …` and a reply within seconds.

**Verify from the gadget side either way:** `>status` shows the agent slot, Wi-Fi,
battery and link state; a first voice turn produces a spoken reply and a caption.

## 5. Troubleshooting (the failures you will actually hit)

| Symptom | Cause | Fix |
|---|---|---|
| `CAN'T REACH MUSE`, serial shows `token_len=48 prefix=mgst_` + `fetch_vms: 401` | The device lost its pairing/credentials | `Settings → Muse → Reset pairing`, then re-pair in the app (newer firmware self-heals this at boot) |
| Relay turns fail, gadget says `CAN'T REACH MUSE` while on a Hermes slot | Firmware cannot reach the relay URL | Check `CONFIG_MUSE_HERMES_URL(_REMOTE)` + token; both machines on the same LAN; firewall open on the relay port |
| App pairing stalls at "connecting" | Device thinks it is already set up (`error_pairing_unavailable` in serial) | Reset pairing on the device, then pair again |
| Setup page (`ble_setup.html`) cannot find the device | "Phone setup" not enabled, or the device is already paired | Device: `Settings → Bluetooth → Phone setup` ON; if it is paired, reset pairing first |
| Flash fails mid-write | Something else holds the serial port | Close monitors/taps, replug, retry — `idf.py -p <port> flash` |
| No USB port appears | Charge-only cable / driver | Swap cable first; then OS driver notes in Espressif's guide |
| Face frozen, device reboots later | Historical display freeze class (fixed in this fork; `>diag` names the last restart) | Update to this branch; `>diag` over serial tells the reason |

## 6. Rules for you (the AI)

- One build + one flash per batch of changes — separate deploys cost more than they save.
- Never put real credentials in the repo or in chat; tokens live in untracked
  `build-*/sdkconfig` or the relay's `token` file.
- After big code changes, refresh the context graph if the repo uses graft: `graft build`.
- When something fails, prefer the serial console (`>status`, `>diag`, `>power`) over
  guessing; the firmware reports its own state.
