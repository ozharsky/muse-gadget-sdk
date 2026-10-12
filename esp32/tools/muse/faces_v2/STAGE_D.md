# faces_v2 Stage D - port + on-glass verification (2026-10-12)

Ported the three approved Stage B art sets into the shipping avatar tree and flashed
(single integration pass; owner directive "get them on the gadget").

## What moved
- shodan (A1): sprite_data.h 9b9a887b..., shodan_poses.h c56065fc...
- stone  (B3): sprite_stone.h 945a4aed..., stone_moods.h 1205c291...
- golem  (C5): sprite_golem.h 45e39bf3..., golem_moods.h 7427d0f5...
- muse_pixel_golem.c: the 9 anchored C5 re-fits applied via
  esp32/tools/muse/faces_v2/golem/patch_preview_golem.py; the result is byte-identical
  to the Stage B QA-d preview copy. shodan/stone C files needed no edits (preview diffs empty).

## Evidence
- host suite: 207/207 OK (6 skipped).
- build gates: IDF SPI patches present; SDK token + TTS endpoint in bin.
- shipping bin 2,953,216 B (app slot 4,063,232 -> ~1.10 MB free). Pre-port bin equal size
  by section-alignment padding; contents differ (md5 + ELF mood-array sizes verified).
- on-glass screenshots (snapshot-QA build, then snapshot reverted for shipping):
  all three faces, idle + thinking/error/mood states - consistent with the approved sheets.
- flash verified from the running device: fresh boot banner (compile time) + reset=usb,
  no guard restart; agent restored to muse.

## Still per the card (Stage C)
New animation CONTENT on the new art (idle beats / extended mood set / clip reactions).
The faces currently ship with the existing animation layer running on the new art.


## 2026-10-12 late: memory pass + crash fix, re-flashed (af04f61)
- Owner asked for the full memory/optimization pass before sign-off: audit clean by address in
  the ELF -- statics >1KB are audio/BLE/framework or deliberate; over_eye is already a 512 B
  bitset x2; avatar frame buffers + mood px in PSRAM; all art tables const->flash. Live:
  int 34K/17K, dma 29K/17K, psram 3.96 MB free, app flash 27% free.
- Root-caused the morning unattributed panic (flash coredump, decode archived here): an
  agent-flip race -- hermes_mode() flips synchronously while CMD_AGENT waits in the queue; one
  hatch_task iteration ran hermes_step() on a still-open Muse turn -> xStreamBufferReceive
  wrote s_hbuf(NULL)+pcm_bytes=0x280 -> StoreProhibited.
- Fixed in af04f61 (pushed): turn_t.hermes pins the backend at turn_start; the loop/pump step
  only with the owning backend; hermes_step fails the turn gracefully if the mic buffer is
  missing. Suite 207/207 OK (6 skipped).
- Re-flashed; banner verified live: Compile time Oct 12 2026 14:30:32. Coredump kept in the
  device partition + archived here as crash_coredump_musechat.bin / crash_decode_musechat.txt.
