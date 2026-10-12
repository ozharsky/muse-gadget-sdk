#!/usr/bin/env python3
"""Art guard for faces-v2 evidence captures.

Why: the Stage C golem pack shipped with renders of the OLD (pre-swap) sprite
(an off-repo capture tree) and nothing caught it before the owner did. This
guard verifies a rendered frame actually shows the palette baked in the face's
header, so wrong-art evidence fails loudly instead of shipping silently.

Usage:
    python3 art_check.py <sprite_header.h> <frame> [more frames ...] [--min 0.60] [--tol-ratio 0.16]

- Frames may be .ppm (parsed natively) or any format ffmpeg can decode (PNG/GIF...).
- 'lit' pixels: r+g+b >= 30. A lit pixel 'matches' when its euclidean RGB distance
  to the nearest SPR_PAL entry <= tol (default: 0.16 * 255 = 40.8).
- PASS when matched share of lit pixels >= --min; exit 1 otherwise.

Known-good pairs: (sprite_golem.h, golem-C5 captures) ~0.9+; the old k4-art golem
captures measured ~0.2-0.3 against the C5 palette -> correctly fails.
"""
import re, subprocess, sys, os

def parse_palette(header_path):
    txt = open(header_path).read()
    i = txt.find('SPR_PAL')
    if i < 0:
        sys.exit('no SPR_PAL in %s' % header_path)
    j = txt.find('};', i)
    body = txt[i:j]
    pal = [tuple(int(v) for v in m) for m in re.findall(r'\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', body)]
    if not pal:
        sys.exit('no palette triples found in %s' % header_path)
    return pal

def read_frame(path):
    if path.lower().endswith('.ppm'):
        d = open(path, 'rb').read()
        i = 0; fields = []
        while len(fields) < 4:
            while d[i:i+1] in b' \t\r\n':
                i += 1
            j = i
            while d[j:j+1] not in b' \t\r\n':
                j += 1
            fields.append(d[i:j]); i = j
        return d[i+1:]
    p = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-frames:v', '1',
                        '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'],
                       capture_output=True)
    return p.stdout

def check(pal, raw, tol):
    tot_lit = 0; tot_match = 0
    for k in range(0, len(raw) - 2, 3):
        r, g, b = raw[k], raw[k+1], raw[k+2]
        if r + g + b < 30:
            continue
        tot_lit += 1
        best = min((r-pr)**2 + (g-pg)**2 + (b-pb)**2 for pr, pg, pb in pal)
        if best <= tol * tol:
            tot_match += 1
    return tot_lit, tot_match

def main():
    args = sys.argv[1:]
    mn = 0.60; tol = 40.8
    files = []
    i = 0
    while i < len(args):
        if args[i] == '--min':
            mn = float(args[i+1]); i += 2
        elif args[i] == '--tol-ratio':
            tol = float(args[i+1]) * 255.0; i += 2
        else:
            files.append(args[i]); i += 1
    if len(files) < 2:
        sys.exit(__doc__)
    header, frames = files[0], files[1:]
    pal = parse_palette(header)
    print('palette: %d colours from %s' % (len(pal), os.path.basename(header)))
    rc = 0
    for f in frames:
        lit, match = check(pal, read_frame(f), tol)
        share = (match / lit) if lit else 0.0
        verdict = 'PASS' if share >= mn else 'FAIL'
        if verdict == 'FAIL':
            rc = 1
        print('%s  %-40s lit=%6d  palette-match=%5.1f%%  (need >= %.0f%%)'
              % (verdict, os.path.basename(f), lit, 100.0 * share, 100.0 * mn))
    sys.exit(rc)

if __name__ == '__main__':
    main()
