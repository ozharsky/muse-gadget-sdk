#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Pack animated clips into the gadget's flash asset partition.

The "drop a GIF in" step: any ffmpeg-readable source (GIF, MP4, WebM, APNG, a
PNG sequence) becomes 64x64, 16-colour, 4-bit frames, delta-coded against the
previous frame, in one `assets.bin` flashed to the `assets` partition and
played by name.

Why a custom pack instead of a GIF decoder on the device: a 64x64 GIF frame is
~4 KB and needs LZW + per-frame palettes; a packed frame here is 2 KB and most
frames shrink to a few hundred bytes because only changed cells are stored.
ffmpeg reads every animated format for us -- at build time, not on the chip.

Usage:
    tools/muse/assets_pack.py --from clip.gif --name bounce --fps 25 --out assets.bin
    tools/muse/assets_pack.py --from clip.mp4 --name clip2 --fps 25 --out assets.bin --append
    tools/muse/assets_pack.py --list assets.bin

Layout (little-endian):
    pack header 16 B: "MGA1", u8 version=1, u8 count, u16 reserved,
                      u32 total_len, u32 reserved
    per clip, entry header 64 B:
        char name[16]
        u16 frames
        u8 fps, u8 fmt (0=u8, 1=i4), u8 npal, u8 pad[3]
        u16 pal[16]          (RGB565)
        u32 data_off         (from pack start)
        u32 data_len
    then (frames+1) u32 frame offsets, relative to data_off
    frame 0 is raw; later frames are runs of (u16 skip, u16 len, new bytes),
    where skip counts bytes since the decoder's position.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path

W = H = 64
PACK_MAGIC = b"MGA1"
ENTRY_HDR = 64
MAX_FRAMES = 250

PALETTE_RGB = [
    (0, 0, 0), (24, 24, 28), (48, 48, 56), (80, 80, 92),
    (116, 116, 130), (156, 156, 172), (200, 200, 214), (255, 255, 255),
    (230, 70, 70), (240, 150, 60), (240, 220, 80), (90, 210, 120),
    (80, 190, 240), (90, 120, 245), (170, 100, 240), (240, 90, 190),
]


def rgb565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


PAL565 = [rgb565(*c) for c in PALETTE_RGB]


def nearest(r: int, g: int, b: int) -> int:
    best, bi = 1 << 30, 0
    for i, (pr, pg, pb) in enumerate(PALETTE_RGB):
        d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
        if d < best:
            best, bi = d, i
    return bi


def frames_from(src: Path, fps: float, max_frames: int) -> list[bytes]:
    cmd = ["ffmpeg", "-v", "error", "-i", str(src), "-vf",
           f"scale={W}:{H}:flags=area,fps={fps}", "-pix_fmt", "rgb24",
           "-f", "rawvideo", "-"]
    out = subprocess.run(cmd, capture_output=True, check=False)
    if out.returncode != 0:
        sys.exit(f"ffmpeg failed: {out.stderr.decode()[:400]}")
    raw = out.stdout
    n = min(len(raw) // (W * H * 3), max_frames)
    if n == 0:
        sys.exit("ffmpeg produced no frames")
    frames = []
    for f in range(n):
        buf = raw[f * W * H * 3:(f + 1) * W * H * 3]
        idx = bytearray(W * H)
        for i in range(W * H):
            idx[i] = nearest(buf[i * 3], buf[i * 3 + 1], buf[i * 3 + 2])
        frames.append(bytes(idx))
    return frames


def pack_i4(idx: bytes) -> bytes:
    """Two pixels per byte, even-x in the LOW nibble (the player's contract)."""
    out = bytearray(W * H // 2)
    for i in range(W * H // 2):
        out[i] = idx[i * 2] | (idx[i * 2 + 1] << 4)
    return bytes(out)


def delta_runs(prev: bytes, cur: bytes) -> bytes:
    out = bytearray()
    pos, n, i = 0, len(cur), 0
    while i < n:
        while i < n and cur[i] == prev[i]:
            i += 1
        if i >= n:
            break
        start = i
        while i < n and cur[i] != prev[i] and (i - start) < 0xFFFF:
            i += 1
        out += struct.pack("<HH", start - pos, i - start)
        out += cur[start:i]
        pos = i
    return bytes(out)


def clip_blobs(name: str, frames: list[bytes], fps: int, fmt: int) -> tuple[bytes, bytes]:
    """(64-byte entry header with data_off/data_len zeroed, offsets+data)."""
    packed = [pack_i4(f) if fmt == 1 else f for f in frames]
    data = bytearray(packed[0])
    offsets = [0, len(data)]
    for prev, cur in zip(packed, packed[1:]):
        data += delta_runs(prev, cur)
        offsets.append(len(data))
    hdr = struct.pack("<16sHBBB3s16HII", name.encode(), len(packed), fps, fmt,
                      len(PAL565), b"\0\0\0", *PAL565, 0, 0)
    return hdr, struct.pack(f"<{len(offsets)}I", *offsets) + bytes(data)


def read_pack(path: Path) -> list[tuple[bytes, bytes]]:
    raw = path.read_bytes()
    if raw[:4] != PACK_MAGIC:
        sys.exit(f"{path} is not an MGA1 pack")
    count = raw[5]
    out = []
    off = 16
    for _ in range(count):
        hdr = raw[off:off + ENTRY_HDR]
        nframes = struct.unpack("<H", hdr[16:18])[0]
        data_off, data_len = struct.unpack("<II", hdr[56:64])
        table = raw[off + ENTRY_HDR:off + ENTRY_HDR + (nframes + 1) * 4]
        out.append((hdr + table, raw[data_off:data_off + data_len]))
        off = data_off + data_len
    return out


def write_pack(path: Path, clips: list[tuple[bytes, bytes]]) -> None:
    # Lay clips out in order, then patch each header's data_off/data_len.
    body = bytearray()
    off = 16
    patched = []
    for hdr_table, data in clips:
        hdr = bytearray(hdr_table[:ENTRY_HDR])
        table = hdr_table[ENTRY_HDR:]
        data_off = off + ENTRY_HDR + len(table)
        struct.pack_into("<II", hdr, 56, data_off, len(data))
        patched.append((bytes(hdr) + table, data))
        off = data_off + len(data)
    for e, d in patched:
        body += e + d
    total = 16 + len(body)
    out = struct.pack("<4sBBHII", PACK_MAGIC, 1, len(patched), 0, total, 0) + bytes(body)
    path.write_bytes(out)
    print(f"wrote {path}: {len(out)} bytes, {len(patched)} clip(s)")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from", dest="src", type=Path)
    ap.add_argument("--name")
    ap.add_argument("--fps", type=float, default=25.0)
    ap.add_argument("--fmt", choices=("i4", "u8"), default="i4")
    ap.add_argument("--max-frames", type=int, default=MAX_FRAMES)
    ap.add_argument("--out", type=Path, default=Path("assets.bin"))
    ap.add_argument("--append", action="store_true")
    ap.add_argument("--list", type=Path)
    args = ap.parse_args()

    if args.list:
        for e, d in read_pack(args.list):
            name, nframes, fps, fmt, npal = struct.unpack("<16sHBBB", e[:21])
            print(f"{name.decode():16s} frames={nframes:4d} fps={fps:3d} "
                  f"fmt={'i4' if fmt else 'u8'} pal={npal} data={len(d)} B "
                  f"({len(d) / max(nframes, 1):.0f} B/frame)")
        return

    if not args.src or not args.name:
        ap.error("--from and --name are required (or use --list)")
    frames = frames_from(args.src, args.fps, args.max_frames)
    fmt = 1 if args.fmt == "i4" else 0
    hdr, blob = clip_blobs(args.name, frames, int(round(args.fps)), fmt)
    # split the blob back into (header+table, data): the table length is known
    table_len = (len(frames) + 1) * 4
    entry = hdr + blob[:table_len]
    data = blob[table_len:]
    clips = [ (entry, data) ]
    if args.append and args.out.exists():
        clips = read_pack(args.out) + clips
    write_pack(args.out, clips)
    raw = len(frames) * (W * H // 2 if fmt else W * H)
    print(f"clip {args.name}: {len(frames)} frames, {len(data)} B vs {raw} B raw "
          f"({100.0 * len(data) / raw:.0f}%), {len(data) / len(frames):.0f} B/frame")


if __name__ == "__main__":
    main()
