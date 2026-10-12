"""Patch the preview-tree copy of muse_pixel_golem.c for the C5 face.

All edits are anchored, all-or-nothing (every anchor must be found before
anything is written), idempotent (a re-run after a successful patch is a no-op),
and verified with occurrence counts printed at the end.

The edits (stage D ports this same list into the repo file):
  1. eye_t init cap 24 -> 48 (the C5 lens is 38 px per eye; arrays already 48)
  2. draw_mouth: C5 slot x28..35 rows 37..40; at rest it leaves the carved smile alone
  3. chest_core cy 49 -> 52 (the C5 core seal centre)
  4. think_spin orbit centre y 49 -> 52
  5. draw_wave centre cy 54 -> 53
  6. eye_bloom centres {20,43} y23 -> {21,42} y29
  7. motes: exclude the plate/glint top-3 by rank (SPR_N-3) and the kind-2 lens
  8. draw_wave: skip the face plate and the lens (the old green test never fires on the warm palette)
"""
import re
import sys

EDITS = [
    ("eye-cap",
     "            if (e->n < 24) {",
     "            if (e->n < 48) {   /* C5 lens = 38 px per eye */"),
    ("chest-core",
     "    int cx = 31, cy = 49;                 /* the plate's true centre (y46..53 + keyhole) */",
     "    int cx = 31, cy = 52;                 /* the C5 core seal's true centre (31.5, 51.5) */"),
    ("think-spin",
     """        int x = 31 + iround(cosf(a) * 10.5f), y = 49 + iround(sinf(a) * 9.0f);
        int tx = 31 + iround(cosf(a - 0.55f) * 10.5f), ty = 49 + iround(sinf(a - 0.55f) * 9.0f);""",
     """        int x = 31 + iround(cosf(a) * 10.5f), y = 52 + iround(sinf(a) * 9.0f);
        int tx = 31 + iround(cosf(a - 0.55f) * 10.5f), ty = 52 + iround(sinf(a - 0.55f) * 9.0f);"""),
    ("wave-centre",
     "    static const int cx = 31, cy = 54;",
     "    static const int cx = 31, cy = 53;"),
    ("bloom-cx",
     "    static const int cx[2] = { 20, 43 };",
     "    static const int cx[2] = { 21, 42 };"),
    ("bloom-cy",
     "                int x = cx[e] + dx, y = 23 + dy;",
     "                int x = cx[e] + dx, y = 29 + dy;"),
    ("motes",
     """    /* the body pixels the glint field drifts over: a ~1/4 spatial subset of the
     * solid stone, never the lens (rank >= 18 is the cream eye) */
    for (int y = 3; y < H - 3; y++) {
        for (int x = 3; x < W - 3; x++) {
            uint8_t k = s_pix_k[y * W + x];
            if (k == 0 || k > 17 || ((x * 3 + y * 5) & 3) != 0) {""",
     """    /* the body pixels the glint field drifts over: a ~1/4 spatial subset of the
     * solid stone; never the top ranks (plate/glint) nor the kind-2 lens */
    for (int y = 3; y < H - 3; y++) {
        for (int x = 3; x < W - 3; x++) {
            uint8_t k = s_pix_k[y * W + x];
            if (k == 0 || k > SPR_N - 3 || SPR_KIND[y * W + x] == 2 || ((x * 3 + y * 5) & 3) != 0) {"""),
    ("wave-skip",
     """            /* Guard: a dynamic index (>= SPR_N) is glow already, not stone -- skip it. */
            if (cur < SPR_N && SPR_PAL[cur][1] > 60 && SPR_PAL[cur][1] >= SPR_PAL[cur][0]) {
                continue;                                /* don't paint over the golem itself */
            }""",
     """            /* Guard: a dynamic index (>= SPR_N) is glow already, not stone -- skip it.
             * C5 palette: never over the face plate or the eyes (all warm -- the old
             * green test never fires on this palette). */
            if ((cur < SPR_N && cur == IDX_SKIN) || SPR_KIND[y * W + x] == 2) {
                continue;                                /* don't paint over the golem itself */
            }"""),
]

DRAW_MOUTH_OLD = """static void draw_mouth(float open)
{
    static const int x0 = 28, x1 = 35;         /* the carved slot (x 28..35, y 30..31) */
    for (int r = 0; r < 3; r++) {              /* wipe to face plate first (3 rows of slack) */
        for (int x = x0 + r; x <= x1 - r; x++) {
            px(x, 30 + r, IDX_SKIN);
        }
    }
    if (open < 0.2f) {                         /* closed: the flat carved slit */
        for (int x = x0; x <= x1; x++) {
            px(x, 30, IDX_LID);
        }
    } else if (open < 0.45f) {                 /* half: a small rounded O (2 rows) */
        for (int x = x0 + 1; x <= x1 - 1; x++) {
            px(x, 30, IDX_LID);
        }
        for (int x = x0 + 2; x <= x1 - 2; x++) {
            px(x, 31, IDX_BLACK);
        }
    } else {                                   /* open: a tall O — dark walls, hollow, a floor */
        for (int x = x0 + 2; x <= x1 - 2; x++) {
            px(x, 30, IDX_LID);
            px(x, 31, IDX_BLACK);
            px(x, 32, IDX_BLACK);
            if (open > 0.9f && x >= x0 + 3 && x <= x1 - 3) {
                px(x, 33, IDX_LID);
            }
        }
        px(x0 + 1, 31, IDX_LID); px(x1 - 1, 31, IDX_LID);
        px(x0 + 1, 32, IDX_LID); px(x1 - 1, 32, IDX_LID);
    }
}"""

DRAW_MOUTH_NEW = """static void draw_mouth(float open)
{
    /* C5: the carved smile (rows 37..38) IS the closed mouth -- at rest this leaves it
     * alone. The speaking inhale parts the slot downward from the smile line. */
    static const int x0 = 28, x1 = 35;
    if (open < 0.2f) {
        return;
    }
    for (int r = 0; r < 3; r++) {              /* wipe to face plate first (3 rows of slack) */
        for (int x = x0 + r; x <= x1 - r; x++) {
            px(x, 37 + r, IDX_SKIN);
        }
    }
    if (open < 0.45f) {                        /* half: a small rounded O (2 rows) */
        for (int x = x0 + 1; x <= x1 - 1; x++) {
            px(x, 37, IDX_LID);
        }
        for (int x = x0 + 2; x <= x1 - 2; x++) {
            px(x, 38, IDX_BLACK);
        }
    } else {                                   /* open: a tall O — dark walls, hollow, a floor */
        for (int x = x0 + 2; x <= x1 - 2; x++) {
            px(x, 37, IDX_LID);
            px(x, 38, IDX_BLACK);
            px(x, 39, IDX_BLACK);
            if (open > 0.9f && x >= x0 + 3 && x <= x1 - 3) {
                px(x, 40, IDX_LID);
            }
        }
        px(x0 + 1, 38, IDX_LID); px(x1 - 1, 38, IDX_LID);
        px(x0 + 1, 39, IDX_LID); px(x1 - 1, 39, IDX_LID);
    }
}"""


def apply(text, name, old, new):
    """Replace old->new; old must occur exactly once unless new is already present."""
    if new in text:
        return text, "already"
    n = text.count(old)
    if n != 1:
        raise SystemExit("PATCH REFUSED: anchor '%s' found %d times (need 1)" % (name, n))
    return text.replace(old, new), "applied"


def main(path):
    raw = open(path, "rb").read()
    crlf = b"\r\n" in raw
    text = raw.decode("utf-8").replace("\r\n", "\n")
    log = []
    for name, old, new in EDITS:
        text, res = apply(text, name, old, new)
        log.append((name, res))
    text, res = apply(text, "draw-mouth", DRAW_MOUTH_OLD, DRAW_MOUTH_NEW)
    log.append(("draw-mouth", res))
    if crlf:
        text = text.replace("\n", "\r\n")
    open(path, "wb").write(text.encode("utf-8"))
    for name, res in log:
        print("  %-12s %s" % (name, res))
    # verify the markers landed
    body = text
    checks = [("e->n < 48", 1), ("y = 52 +", 2), ("cy = 53", 1), ("cx[2] = { 21, 42 }", 1),
              ("y = 29 + dy", 1), ("SPR_N - 3", 1), ("cur == IDX_SKIN) || SPR_KIND", 1), ("px(x, 37 + r", 1)]
    bad = 0
    for pat, want in checks:
        got = body.count(pat)
        ok = "ok" if got == want else ("MISSING" if got == 0 else "x%d" % got)
        if got != want:
            bad += 1
        print("  verify %-24s %s" % (pat, ok))
    print("PATCH-DONE" if not bad else "PATCH-VERIFY-FAIL")


if __name__ == "__main__":
    main(sys.argv[1])
