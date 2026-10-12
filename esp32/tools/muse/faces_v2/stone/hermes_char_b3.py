"""B3 obsidian-oracle avatar as firmware-style sprite data: palette indices, brightness table, 'kinds', geometry.

Stage B build (faces_v2/stage_b/stone): same shape as hermes_char.py (which built the shipped stone),
re-derived for the approved B3 concept art (obsidian kintsugi oracle: near-black body, gold crack
veins + gold rims, cream-yellow glowing lens). Derived from the 6x nearest upscale of the approved
64x64 snap (base_b3_384.png) so the pose edits run at the same canvas as the original pack.
"""
import numpy as np
from PIL import Image

# index -> (name, rgb). Sampled from the approved B3 snap. Invariants (the C is authoritative):
#  - SOCKET must be the dominant body dark: eye_free() lets the drawn eyes/lids paint only over
#    BLACK/DARK/SOCKET pixels, so the whole near-black face has to belong to that family.
#  - DARK is the bake_moods dark ink and the closed-lash colour.
#  - Extra invented steps (SKIN/HI2/HI, GOLD_D/GOLD_L) give lids, highlights and glints a brighter tone.
PAL_DEF = [
    ("BLACK", (0, 0, 0)), ("DARK", (8, 8, 10)), ("SOCKET", (23, 23, 25)),
    ("STONE_M", (49, 49, 53)), ("STONE_L", (55, 54, 55)), ("SKIN", (64, 62, 62)), ("HI2", (92, 88, 86)), ("HI", (156, 146, 132)),
    ("GOLD_D", (150, 113, 34)), ("GOLD_M", (208, 159, 54)), ("GOLD_L", (250, 214, 120)), ("EYE_U", (250, 238, 207)),
]
NAME = {n: i for i, (n, _) in enumerate(PAL_DEF)}
PAL = np.array([c for _, c in PAL_DEF], np.uint8)
SPR_N = len(PAL_DEF)
BRIGHT = np.array([0, 2, 3, 4, 5, 6, 7, 7, 9, 10, 10, 11], np.uint8)      # next brighter shade in the same family
# the lens fill takes the state colour; the gold rims, the cracks and the stone keep their own
GLOW_FAMILY = np.array([n == "EYE_U" for n, _ in PAL_DEF])
IDX = {"IDX_BLACK": NAME["BLACK"], "IDX_DARK": NAME["DARK"], "IDX_SOCKET": NAME["SOCKET"], "IDX_LID": NAME["DARK"],
       "IDX_SKIN": NAME["SKIN"], "IDX_EYE_R": NAME["GOLD_M"], "IDX_EYE_U": NAME["EYE_U"]}


import pixsnap2
from PIL import ImageFilter


def _clean_specks(idx):
    """Drop the disconnected 1-3 px debris the dissolve tail leaves under the bust (rows >= 50)."""
    from collections import deque
    out = idx.copy()
    seen = np.zeros_like(idx, bool)
    for y in range(64):
        for x in range(64):
            if seen[y, x] or idx[y, x] == 0:
                continue
            comp = []
            q = deque([(y, x)])
            seen[y, x] = True
            while q:
                cy, cx = q.popleft()
                comp.append((cy, cx))
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        ny, nx = cy + dy, cx + dx
                        if 0 <= ny < 64 and 0 <= nx < 64 and not seen[ny, nx] and idx[ny, nx] != 0:
                            seen[ny, nx] = True
                            q.append((ny, nx))
            if len(comp) <= 3 and min(cy for cy, _ in comp) >= 50:
                for cy, cx in comp:
                    out[cy, cx] = 0
    return out


def to_indices(path, despeckle=True):
    """Lanczos to 64x64 + light unsharp, glow-colour protection, nearest palette colour in Lab, despeckle, specks."""
    src = np.array(Image.open(path).convert("RGB"))
    im = Image.open(path).convert("RGB").resize((64, 64), Image.LANCZOS).filter(ImageFilter.UnsharpMask(radius=1.0, percent=80, threshold=2))
    small = pixsnap2.protect_accents(src, np.array(im))
    lab_px, lab_pal = pixsnap2.to_lab(small.reshape(-1, 3)), pixsnap2.to_lab(PAL)
    idx = ((lab_px[:, None] - lab_pal[None]) ** 2).sum(2).argmin(1).reshape(64, 64)
    if despeckle:
        out = idx.copy()
        for y in range(64):
            for x in range(64):
                nb = [idx[yy, xx] for yy in range(max(0, y - 1), min(64, y + 2)) for xx in range(max(0, x - 1), min(64, x + 2)) if (yy, xx) != (y, x)]
                if idx[y, x] not in nb:
                    d = np.sqrt(((pixsnap2.to_lab(PAL[idx[y, x]][None])[0] - pixsnap2.to_lab(PAL[nb]).mean(0)) ** 2).sum())
                    if d < 35:
                        out[y, x] = np.bincount(nb).argmax()
        idx = out
    return _clean_specks(idx).astype(np.uint8)


SPR = to_indices("base_b3_384.png")

# 1 = glints along the gold (the kintsugi cracks), 2 = the eye lenses, 3 = shimmer in the shadow planes
_eyezone = np.zeros((64, 64), bool)
_eyezone[24:31, 16:30] = True
_eyezone[24:31, 35:49] = True
_eye = np.isin(SPR, [NAME["GOLD_M"], NAME["EYE_U"]]) & _eyezone
_gold_hi = np.isin(SPR, [NAME["GOLD_M"], NAME["GOLD_L"]])
_shimmer = np.isin(SPR, [NAME["STONE_M"], NAME["STONE_L"]])
KIND = np.zeros((64, 64), np.uint8)
KIND[_gold_hi] = 1
KIND[_shimmer] = 3
KIND[_eye] = 2

# Geometry of the effects (matches the C's own copies; the C is authoritative for the glass)
GEO = dict(
    eye_cy=26, eye_cx=(21, 42), centre=(31.5, 31.5),
    halo=(31.5, 31.5, 29.0, 29.0), ring_r=27.5,
    bang=(51, 5), q_squint=(51, 10), q_confused=(46, 4), zzz_x=51, zzz_y=24,
    scan_x=(10, 53), scan_y=(12, 54), scan_line_x=(8, 55),
    target=(31, 31), target_size=(29, 10), lightning=((16, 26), (51, 26)),
    sweat_x=53, sweat_y=(16, 36), twinkle=((13, 24), (50, 24)), wink_spark=(51, 23), bored=(46, 8),
)
EYES_BOX, MOUTH_BOX = (12, 35, 8, 56), (42, 57, 16, 48)       # rows r0:r1, cols c0:c1 for pose deltas (brows are in the eye box)
NOSE_BOX, HELMET_ROWS = (30, 44, 28, 36), 14
ANCHOR_HUE = 180.0     # kept C-accurate for the preview port (C's constant); see stage_b notes on the gold-eyes re-tune

# ---- procedural eyes for the B3 lens (rows 26-30, cols 19-26 / 37-44 in the art) -------------------
EYE_ROW0, EYE_COL0 = 26, (17, 37)
EYE_TEMPLATE = {0: (1, 6), 1: (0, 7), 2: (0, 8), 3: (1, 7)}                                   # rows 26-29
EYE_WIDE_TEMPLATE = {-1: (2, 5), 0: (1, 6), 1: (0, 7), 2: (0, 8), 3: (1, 7), 4: (2, 6)}       # rows 25-30
EYE_ZONES = np.zeros((64, 64), bool)
EYE_ZONES[23:32, 15:30] = True
EYE_ZONES[23:32, 35:50] = True


def clean_eyes(idx):
    """Solidify each pose's cream lens and give it one simple pupil; gold rims untouched."""
    out = idx.copy()
    glow, filler = [NAME["EYE_U"]], (NAME["BLACK"], NAME["DARK"], NAME["SOCKET"])
    for c_lo, c_hi in ((12, 33), (33, 56)):
        sub = out[18:36, c_lo:c_hi]
        g = np.isin(sub, glow)
        if g.sum() < 6:
            continue
        for y in range(sub.shape[0]):
            xs = np.nonzero(g[y])[0]
            if len(xs) >= 2:
                for x in range(xs.min(), xs.max() + 1):
                    if not g[y, x] and sub[y, x] in filler:
                        sub[y, x] = NAME["EYE_U"]
                        g[y, x] = True
        if g.sum() >= 14:
            ys, xs = np.nonzero(g)
            cy, cx = int(round(ys.mean())), int(round(xs.mean()))
            big = g.sum() >= 40
            for yy in ((cy - 1, cy, cy + 1) if big else (cy, cy + 1)):
                for xx in ((cx, cx + 1) if big else (cx,)):
                    if 0 <= yy < g.shape[0] and 0 <= xx < g.shape[1] and g[yy, xx]:
                        sub[yy, xx] = NAME["DARK"]
    return out
