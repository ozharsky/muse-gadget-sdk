# HERMES-FORK — the `hermes-local` branch

This is a **private fork** of [`facebookincubator/muse-gadget-sdk`](https://github.com/facebookincubator/muse-gadget-sdk).
All custom work lives on the **`hermes-local`** branch — which is also this repo's default
branch, so what you see first is the work, not upstream's `main`.

- `main` — untouched mirror of upstream's `main` at the fork point (commit `3b56ccb`).
- `hermes-local` — everything below. **86 commits ahead of the fork point; 70 beyond
  current upstream `main`** (upstream has moved 54 commits since the fork).
- `upstream` remote → `facebookincubator/muse-gadget-sdk` (rebase reference; **never push**).
- `origin` remote → this repo (`ozharsky/muse-gadget-sdk`).

> **Hardware target:** Waveshare ESP32-S3 1.43″ round AMOLED board profile
> (`s3143c`, `build-muse-waveshare-s3-143c`). Other boards exist upstream but this
> branch is developed and bench-verified on that one.

---

## What this fork adds

### 1. Three-agent companion (Muse / Hermes / ALT)

The gadget started as a Muse-only device. This branch makes it a switchable companion
for **three agent slots**:

| Slot | Default occupant | Face (avatar) | Transport |
|---|---|---|---|
| `MUSE_AGENT_MUSE` | Muse / SHODAN | `shodan` | Muse cloud VM |
| `MUSE_AGENT_HERMES` | Hermes (default profile) | `stone` (stone god) | Mac relay → Hermes |
| `MUSE_AGENT_HERMES_ALT` | Any named Hermes profile (e.g. `beepbop`) | `golem` | Mac relay → that profile |

- **Enum settings with NVS migration** — old 0/1 values keep their meaning when the
  third slot appears (`91bb056`).
- **Double-tap** of the talk button cycles slots (was triple-tap; gesture pinned by a
  host test: tap < 300 ms, 1.2 s accumulate window, long press is not a tap) (`92df4d8`).
- **Settings → Agents** page: direct radio-select with the agent's own mark, plus a
  text-entry row for the ALT profile name (`2cb11e6`, `5e7b0be`). An ALT slot with no
  profile name is dimmed and refuses selection (the relay can't route an unnamed profile).
- **Per-agent everything**: voice (relay-side `voice_name`), avatar face
  (`38148ae`, `muse_pixel_switch.c`), and per-turn `&profile=` on every relay URL.
- **Active-only vs stored getters** (`muse_settings_agent_profile` vs
  `muse_settings_agent_alt`) — the split that fixed "can't switch to ALT"
  (an accessor answering two different questions WAS the bug).

### 2. Reverse notification channel (relay → device)

Everything else the gadget does is poll ("I ask, you answer"). This is the opposite
direction: the Mac relay pushes a short line, the device looks up, chirps, speaks it,
and offers to open the exact agent that raised it.

- **Relay side** (`hermes-relay`): `notifications.py` store + endpoints
  `GET /notifications` (pending), `GET /notifications/history?limit=N` (read items
  included), `POST /notifications`, `/notifications/ack` (mark read),
  `/notifications/cancel` (**delete** — a cancelled reminder must never fire).
  Identity is resolved at creation time via `agents.py` so a notification always
  names a routable profile (`ee3cba6`).
- **Firmware side**: `notif_tick()` in the session task polls (15 s awake / 60 s
  asleep, gated to Hermes-mode agents), `notif_present()` acts the mood + chime +
  speaks + captions with a tap hint, `muse_notif_open_pending()` switches to the
  notifying agent's slot. The face tap opens a waiting nudge before it's an
  ordinary pet (`ee3cba6`, `bc0fab1`, `d128b64`).
- **On-device inbox page** (Settings → Notifications, bell row with unread count):
  tap a row to switch to that agent, **hold to remove** (cancels on the relay).
  Data path keeps the LVGL task off the network: a flag-poke request and a
  double-buffered PSRAM snapshot whose generation bumps only when content changed
  (a rebuild per poll would eat taps).
- **Voice-set timers**: the agent can end a reply with `[remind:<n><unit> <text>]`
  (s/m/h/d) — the relay converts it to a due-timestamped notification, strips it
  from speech, and the gadget presents it when the clock passes. No firmware timer
  logic at all (`extract_reminders()` in `relay.py`).
- **Failed-print alerts**: the print watcher raises an `alert`-mood nudge on a
  printing → ERROR/CANCELLED transition (a 9-hour print dying at hour 8 must not be
  discovered by walking over).

### 3. Display-freeze survivability + black box

The screen occasionally froze mid-update. Three decoded coredumps later, the root
cause was **two upstream ESP-IDF bugs** in `spi_bus_lock.c` (both patched in the
local IDF tree — see below) plus a class of *lost panel-completion interrupts* that
firmware must survive rather than fix.

- `muse_lcd_bands.c` — **two-level guard**:
  - *Level 1*, lost signal: bounded chunk waits (2 s), a 250 ms `lcd_guard` timer
    that detects stalled outstanding pieces, hands buffers back and releases the
    waiting flush so the next frame draws over the incomplete band.
  - *Level 2*, a driver call that never returns: `s_driver_ms` brackets every panel
    driver call; past 4 s the guard dumps interrupt state (`esp_intr_dump`) and
    `esp_restart()`s — a clean ~4 s reboot instead of a 20 s freeze + abort.
- **`lv_display_set_flush_wait_cb`** (`60c762b`) — replaces LVGL's untimed
  `while(disp->flushing);` spin (a lost "flush done" NOTIFY parked LVGL forever).
  Waits on the real condition (pieces outstanding OR notify owed), writes off the
  wait past 1 s so the screen keeps drawing.
- **The black box** (`muse_diag.c`): freezes happen off-USB and a clean restart
  leaves no coredump, so the reason is recorded in **NVS** — `restart_by`,
  `restart_last`, and an LCD fault blob (chunks outstanding, recover count, driver
  ms, internal/DMA/PSRAM free+min at the trip). `>diag` prints it over serial,
  cable or not. Boot report logs it and rotates it (`a48d183`, `2f74770`).
- **Fault injection** to prove each defense without hardware that loses signals:
  `>lcd.swallow[=N]` (drop completions → graceful), `>lcd.dropnotify[=N]` (drop
  flush notifies → ~1 s backstop), `>lcd.wedge[=MS]` (wedge a driver call →
  ~4 s clean restart).
- **IDF tree patches** (outside this repo, re-applied by
  `art-pipeline/check_idf_patches.sh` before every build): `94831e429ba`
  (`bg_exit_core` never resumes a waiting lock owner) and `a16c00376b8`
  (TOCTOU on `lock->acquiring_dev`, upstream issue #18527 whose reproducer \#3 is
  stock `esp_lcd` SPI panel + LVGL on ESP32-S3 — literally this board).

### 4. Power, restart and battery

- **Restart paths for a board whose PWR switch died**: hold BOOT 20 s → force
  restart; menu "Restart"; settings POWER button; console `>reboot`; and
  `reset_gadget.py` on the Mac (DTR/RTS pulse via the USB-Serial/JTAG peripheral,
  works when the firmware is fully hung) (`4683558`). The watchdog hard ceiling
  (10 min with BLE on) means a frozen screen always self-reboots.
- **Coredumps to flash** — partition `coredump` @ 0x7E2000 (120 KB, 32-task
  dump) instead of UART, so a panic leaves a backtrace even with nobody watching
  the port (`402a788`). Every flash archives the matching ELF so a later dump
  always decodes.
- **Low battery: warn at 15 %, protect the cell at 5 %** (`e112a1d`) — the board
  previously just died. Both thresholds read the **EMA-smoothed** percentage (the
  raw voltage gauge sags ~10 points for seconds under radio load and would
  false-trip). 15 % plays a soft descending two-note cue without waking the
  screen; 5 % captions and deep-sleeps. Bench: `>batt.sim=<pct>`.
- **Light sleep**: `CONFIG_PM` + tickless idle with Wi-Fi retained
  (`c7960fd`) — the idle-current lever. Battery meter recalibrated (100 % anchor
  at 4.10 V — the charger's relaxed CV termination means a full cell never idles
  at 4.2 V).

### 5. Connectivity fixes

- **Relay LAN announce + reachability cache** (`35c7523`) — killed the 7-second
  turns: the relay announces its LAN address in `/tasks` and `/agents`, the gadget
  stores it in NVS and prefers it over the compiled-in URL; the reachability probe
  is cached (1.5 s) and netmask-aware.
- **Wi-Fi saved-network switching** (`5e7b0be`, `8729991`) — tap = switch (joins
  BY NAME with the target's own stored password, never through the credential
  write), hold = forget; range-checked against the scan store the settings page
  actually fills; confirm joinable BEFORE tearing down the working association.
- **Muse chat bearer fix** (`882b926`) — the chat session asks the app layer for
  the provisioned access token instead of reusing the SDK identity-class token
  (the post-pairing 401 loop).
- **Task bridge repairs** (`0e5ee14`) — idempotency-key collision, stranded
  `running` cards, false-success completions, OOB read, backoff. Cross-agent task
  cards (Hermes ↔ Muse) close the loop end to end (`b450bde`, `d49b272`).

### 6. Avatar system

- **Three faces** built from C sources under renamed symbols: `shodan` (Muse),
  `stone` (Hermes default), `golem` (ALT). `muse_pixel_switch.c` maps agent slot →
  face and owns the 1.5 s glitch transition (`38148ae`).
- **Palette-read discipline**: every `SPR_PAL[value read back from the
  framebuffer]` is guarded `v < SPR_N` — the light pass paints dynamic indices
  (≥ SPR_N) into the framebuffer and an unguarded late-frame read is an OOB panic
  (`33cb2ba`, `a158f55`). This was the 2026-10 panic class.
- Baked mood/reaction frames (`golem_moods.h`, `shodan_poses.h`), per-agent
  reactions, tap = pet, long-press = reading mode of the active agent's last
  reply (`5b6ece7`).

### 7. Build & test infrastructure

- **198 host tests** run without hardware: `env -u PYTHONPATH python3 -m unittest
  discover -s tests -p 'test_*.py'` from `esp32/`.
- **`build_flash.sh`** (in `art-pipeline`, not this repo) — hard-gates the sdkconfig
  secrets before every build, verifies the built binary actually contains them,
  and archives the ELF+bin per flash for coredump decoding.
- **Bench commands over serial** (`>...`): `>status`, `>diag`, `>power`, `>tap`,
  `>notifs`, `>notif.cancel=<id>`, `>lcd.swallow|dropnotify|wedge`, `>batt.sim`,
  `>wifi.switch|forget|add`, `>reboot`, plus fault injection for every guard.
- **Snapshot QA** — `LV_USE_SNAPSHOT` can be enabled in the build sdkconfig for
  screen captures over serial (`tools/muse/snap.py`); off in shipped builds.

---

## Repo layout highlights

```
esp32/components/muse/        the companion UI + session (this fork's center of work)
  muse_chat_session.cpp       the session task: turns, tasks, notifications, inbox fetch
  muse_voice.c                push-to-talk, pre-roll, held notes, sound flags
  muse_settings_ui.c          every settings page incl. Agents + Notifications inbox
  muse_lcd_bands.c            the panel driver + freeze guards (levels 1 & 2)
  muse_diag.c / .h            the NVS black box
  boards/board_waveshare_s3_143c.c   the target board (touch, panel, PMU)
  avatar/                     three faces + icon headers (tracked since c14a224)
esp32/tests/                  198 host tests, no hardware needed
```

The Mac-side relay, print watcher, and art pipeline live in a separate private
workspace (`~/muse/hermes-relay`, `~/muse/art-pipeline`) — not part of this repo.

---

## Secrets policy

**No key material in this repo.** The device's secrets (Muse SDK token, relay
token, TTS key) live only in the *untracked* `build-*/sdkconfig` on the author's
machine and are re-applied by tooling at build time. Test fixtures use visibly
fake placeholder tokens (`mgst_AAAA…`). `esp32/debug/` (coredumps + ELF archive)
and `graft/` (local graph cache) are gitignored.

## Building

Use the upstream project's own instructions (see upstream `README.md`) — this fork
does not change the toolchain. On the author's machine it's
`bash ~/muse/art-pipeline/build_flash.sh s3143c <port> [flash]`, which enforces
the secret gate described above.

## Syncing with upstream

```bash
git fetch upstream
git rebase upstream/main hermes-local    # then push to origin
```

The `hermes-local` history is a linear rebase of upstream plus the work above;
no merge commits.
