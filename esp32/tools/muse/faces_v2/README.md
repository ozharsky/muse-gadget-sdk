# faces_v2 — Stage B build recipes (gadget faces v2)

The one-shot recipes that produced the **stage B art sets** for the gadget re-skin
(card `t_e867131b`). The baked headers themselves are NOT in git (the avatar dir is
gitignored); they live on the Mac at `~/muse/art-pipeline/faces_v2/stage_b/` and are
attached to the card as zips.

## What each face's art set is

| face | base sprite | poses | mood art |
|---|---|---|---|
| shodan (A1, approved) | `sprite_data.h` (17 colours) | `shodan_poses.h` — 27 sparse deltas (eyes/brows/mouth), Qwen edits of the A1 raw | (poses carry the expression; C effects on top) |
| stone (B3, approved) | `sprite_stone.h` (12 colours, incl. 21 pose tables + eye geometry) | 21 of the pack's 27 pose raws (gaze/lids are procedural) | `stone_moods.h` — all 52 relay moods, 864 frames, 45 391 diff px |
| golem (C5, approved direction) | `sprite_golem.h` (10 colours; eye ring + lens kind map; chest core seal) | (procedural — no pose table on the golem) | `golem_moods.h` — all 52 relay moods, 864 frames, 45 969 diffs, 1 003 KB |

## Re-running

Everything runs on the Mac. Qwen pose edit needs ComfyUI reachable
(`COMFY_HOST=<your ComfyUI base URL>); Gemini needs `GEMINI_API_KEY`.

- shodan: `bash …/stage_b/shodan_build.sh` (bakes `make_shodan_poses_a1.py`, then
  states/moods sheets + pose grid through the real renderer).
- stone: `bash …/stage_b/stone_build.sh` (header → mood bake → sheets).
- golem-C5: `bash …/stage_b/golem/pack/golem_build.sh` (composes the final base from the
  C5 snap, bakes `sprite_golem.h` + `golem_moods.h`, patches the preview-tree
  `muse_pixel_golem.c` with the C5 geometry edits, renders states+moods sheets).
  GIFs: `python3 …/golem/pack/make_gifs_golem.py`; before/after: `compare_golem.py`.
- preview tree: `stage_b/preview/esp32/` = a scratch copy of `components/muse/*.h` +
  `tools/muse/{dump_anim.c,preview_sheets.py,make_gifs.py,pose_dump.c}` + the face sources
  with the NEW headers — the real renderer running against unshipped art.

## Golem C5 re-fit notes (2026-10-12, stage B)

- `golem/pack/bake_moods.py` = the canonical moods_v2 baker + AVATAR repointed + 6 C5
  re-fits: scan bar starts on the crown (was y3, floating over the C5 head's y7 top),
  storm cloud+bolt moved down 5 (old crown), rainbow arc radius 25 (was 28), confetti
  carries RB pop colours (yellow bits vanished on the warm body), hacked tear on the
  crown, storm bolt got a G2 shadow line (bare white bolt was invisible on the cream plate).
- `golem/pack/patch_preview_golem.py` = the 9 anchored C edits the new face needs
  (eye_t cap 24→48 for the 38-px lens; mouth slot rows 37..40 with the carved smile as
  the closed mouth; chest_core/spark/wave re-centred at (31,52)/(31,52)/(31,53);
  eye_bloom (21/42, y29); motes exclude top-3 ranks + kind-2; draw_wave skips plate+lens).
  Stage D ports these into the repo `muse_pixel_golem.c`.
- `GPL rule` stands: everything here is original to this project.

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
5. **State colour of the stone's eyes**: the C keeps `ANCHOR_HUE 180`; the B3 lens fill
   hue-shifts per state from its own gold, which reads fine. Re-tune in
   `muse_pixel_stone.c` if the old colour language is wanted back.
6. **Machinery predicates are palette-dependent** — before baking any face, check the
   palette table against the C's tests (props: `R>60 && R>=B` goldish, `max<0.14*255`
   darkish; chest core: dark_teal `max<0.12*255 && B>=R`; eye bloom/`is_warm` near-neutral
   bright). The C5 palette needed: DEEP (12,11,30) for darkish, PLATE (245,233,218) for the
   s<=0.12 neutral lens-style branch, GLINT as the top rank. `golem_char.report()` prints
   the whole predicate table.
