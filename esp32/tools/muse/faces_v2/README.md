# faces_v2 — Stage B build recipes (gadget faces v2)

The one-shot recipes that produced the **stage B art sets** for the gadget re-skin
(card `t_e867131b`). The baked headers themselves are NOT in git (the avatar dir is
gitignored); they live on the Mac at `~/muse/art-pipeline/faces_v2/stage_b/` and are
attached to the card as `stage_b_deliverables.zip`.

## What each face's art set is

| face | base sprite | poses | mood art |
|---|---|---|---|
| shodan (A1, approved) | `sprite_data.h` (17 colours) | `shodan_poses.h` — 27 sparse deltas (eyes/brows/mouth), Qwen edits of the A1 raw | (poses carry the expression; C effects on top) |
| stone (B3, approved) | `sprite_stone.h` (12 colours, incl. 21 pose tables + eye geometry) | 21 of the pack's 27 pose raws (gaze/lids are procedural) | `stone_moods.h` — all 52 relay moods, 864 frames, 45 391 diff px |
| golem | round-2 concepts only (C4/C5/C6) — awaits the owner's pick | — | — |

## Re-running

Everything runs on the Mac. Qwen pose edit needs ComfyUI reachable
(`COMFY_HOST=https://nzxt.tailcb1d23.ts.net`); Gemini needs `GEMINI_API_KEY`.

- shodan: `bash ~/muse/art-pipeline/faces_v2/stage_b/shodan_build.sh` (bakes
  `make_shodan_poses_a1.py`, then states/moods sheets + pose grid through the real renderer).
  Pose generation is a separate long step: `pack/gen_poses.py` (22 poses) +
  `pack/gen_poses_a1_extra.py` (the 5 eye-only poses the original pack hand-carried:
  half/closed/left/right/wink).
- stone: `bash ~/muse/art-pipeline/faces_v2/stage_b/stone_build.sh` (header → mood bake →
  sheets). Pose generation: `pack/gen_poses.py` (384 px canvas, consistency LoRA).
- preview tree: `stage_b/preview/esp32/` = a scratch copy of `components/muse/*.h` +
  `tools/muse/{dump_anim.c,preview_sheets.py,make_gifs.py,pose_dump.c}` + the face sources
  with the NEW headers — the real renderer running against unshipped art.
- `pose_dump.c` compiles by `#include`-ing the face `.c` (so its static pose machinery is
  reachable) — shodan needs `-DSHODAN_STYLE`, stone/golem use `POSE_TABLE` directly.

## Invariants learned the hard way (keep these when re-deriving a face)

1. **The stone palette's SOCKET index must be the dominant body dark.** The C's
   `eye_free()` only lets the drawn eyes/lids paint over `BLACK/DARK/SOCKET`, so any body
   tone that maps elsewhere blocks the eye redraws and the drawn eye gets holes.
2. **Generated headers must end every array with `};`** — `bake_moods.py`'s `load_sprite`
   regex captures up to the first `};`; a `},` terminator makes it swallow the next
   declaration.
3. **The eye-only poses** (`half/closed/left/right/wink` for shodan) were hand-carried in
   the original pack, not emitted by `gen_poses.py` — `gen_poses_a1_extra.py` regenerates
   them for a new base.
4. **`POSE_COUNT` is an enum** in the stone header (not a `#define`) — don't `#ifdef` it.
5. **State colour of the stone's eyes**: the C keeps `ANCHOR_HUE 180` (tuned for the old
   turquoise eyes); the B3 lens fill hue-shifts per state from its own gold, which reads
   fine (idle gold, listening green, thinking cyan, speaking magenta). Re-tune the shifts
   in `muse_pixel_stone.c` if the owner wants the old colour language back.
6. Golem round-2 concepts: `stage_b/golem/` (`make_sheet_r2.py` builds the owner sheet;
   C6v2 is one Gemini edit of C6 with stronger eyes — the 64px read wins).
