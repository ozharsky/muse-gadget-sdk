#!/usr/bin/env python3
"""A1 -> sprite_data.h (Stage B). Copy of make_sprite_header.py with A1 constants.

    python3 make_sprite_header_a1.py shodan_a1_64.png sprite_data.h
"""
import colorsys
import sys

import numpy as np
from PIL import Image

# --- portrait-specific constants (the A1 concept: silver android, green lenses) ---------------
EYE_R_RGB = (165, 227, 129)       # bright green lens / bright circuit accents
EYE_U_RGB = (208, 222, 228)       # lens highlight
SOCKET_RGB, LID_RGB, DARK_RGB, SKIN_RGB, BLACK_RGB = (33, 70, 76), (1, 1, 10), (1, 1, 5), (199, 213, 226), (0, 0, 0)
EYE_BOXES = [(33, 39, 19, 27), (33, 39, 37, 45)]   # (y0, y1, x0, x1) of each eye
# ----------------------------------------------------------------------------------------------

src, dst = sys.argv[1], sys.argv[2]
a = np.array(Image.open(src).convert('RGB'))
assert a.shape[:2] == (64, 64), "the portrait must be exactly 64x64"
cols, inv = np.unique(a.reshape(-1, 3), axis=0, return_inverse=True)
idx = inv.reshape(64, 64)
N = len(cols)
hsv = [colorsys.rgb_to_hsv(*(c / 255.0)) for c in cols]
pale = np.array([hsv[i][1] < 0.12 and hsv[i][2] > 0.5 for i in range(N)])
palemask = pale[idx]
ys, xs = np.where(palemask)
face = np.zeros((64, 64), bool)
for y in range(ys.min(), ys.max() + 1):               # the face = pale pixels, holes filled row by row
    row = np.where(palemask[y])[0]
    if len(row):
        face[y, row.min():row.max() + 1] = True

def pal(c):
    return [i for i in range(N) if tuple(cols[i]) == c][0]

EYE_R, EYE_U = pal(EYE_R_RGB), pal(EYE_U_RGB)
eyebox = np.zeros((64, 64), bool)
for y0, y1, x0, x1 in EYE_BOXES:
    eyebox[y0:y1, x0:x1] = True
KIND = np.zeros((64, 64), np.uint8)
for y in range(64):
    for x in range(64):
        i = idx[y, x]
        if eyebox[y, x] and i in (EYE_R, EYE_U):
            KIND[y, x] = 2
        elif i == EYE_R and not face[max(0, y - 1):y + 2, max(0, x - 1):x + 2].any():
            KIND[y, x] = 1
        elif hsv[i][1] > 0.3 and 0.3 < hsv[i][2] < 0.9 and not face[y, x]:
            KIND[y, x] = 3
fam = sorted([i for i in range(N) if hsv[i][1] > 0.3], key=lambda i: hsv[i][2])   # saturated colours, dark -> bright
BRIGHT = list(range(N))
for k, i in enumerate(fam[:-1]):
    BRIGHT[i] = fam[k + 1]                                                         # the next brighter shade
IDX = dict(SOCKET=pal(SOCKET_RGB), LID=pal(LID_RGB), DARK=pal(DARK_RGB), SKIN=pal(SKIN_RGB), BLACK=pal(BLACK_RGB))
with open(dst, 'w') as f:
    f.write('// Generated from the A1 portrait (64x64, %d colours). Do not edit by hand.\n#pragma once\n#include <stdint.h>\n' % N)
    f.write('#define SPR_N %d\n' % N)
    for k, v in IDX.items():
        f.write('#define IDX_%s %d\n' % (k, v))
    f.write('#define IDX_EYE_R %d\n#define IDX_EYE_U %d\n' % (EYE_R, EYE_U))
    f.write('static const uint8_t SPR_PAL[SPR_N][3] = {\n' + ',\n'.join('    {%d, %d, %d}' % tuple(c) for c in cols) + '\n};\n')
    f.write('static const uint8_t SPR_BRIGHT[SPR_N] = { %s };\n' % ', '.join(str(b) for b in BRIGHT))
    f.write('static const uint8_t SPR[64 * 64] = {\n')
    for y in range(64):
        f.write('    ' + ', '.join(str(v) for v in idx[y]) + ',\n')
    f.write('};\nstatic const uint8_t SPR_KIND[64 * 64] = {\n')
    for y in range(64):
        f.write('    ' + ', '.join(str(v) for v in KIND[y]) + ',\n')
    f.write('};\n')
print('colours', N, '| eye px', int((KIND == 2).sum()), '| line px', int((KIND == 1).sum()), '| root px', int((KIND == 3).sum()), '|', IDX)
# verification: exact KIND2 positions + the greens' full extent per colour
for name, i in (('EYE_R', EYE_R), ('EYE_U', EYE_U)):
    yy, xx = np.where(idx == i)
    print(f'{name} idx={i} total={len(yy)}')
for y in range(30, 41):
    row = ''
    for x in range(14, 50):
        i = idx[y, x]; k = KIND[y, x]
        if i == EYE_U:
            row += 'u'
        elif k:
            row += str(k)
        else:
            row += '.'
    print(f'{y:3d}| {row}')
print('face rows', ys.min(), ys.max())
