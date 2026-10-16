#!/usr/bin/env python3
# This file is part of picoFace.
#
# picoFace is free software: you can redistribute it and/or modify it under the terms of
# the GNU General Public License as published by the Free Software Foundation, either
# version 3 of the License, or (at your option) any later version.
#
# picoFace is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
# without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with picoFace. If
# not, see <https://www.gnu.org/licenses/>.

"""Spectrum screen dumps (.scr, 6912 bytes) to one PNG, side by side, with a label each:
scr2png.py out.png a.scr b.scr ..."""
import sys, zlib, struct
PAL = [(0,0,0),(0,0,205),(205,0,0),(205,0,205),(0,205,0),(0,205,205),(205,205,0),(205,205,205),
       (0,0,0),(0,0,255),(255,0,0),(255,0,255),(0,255,0),(0,255,255),(255,255,0),(255,255,255)]
def pixels(d):
    img = [[(0,0,0)] * 256 for _ in range(192)]
    for y in range(192):
        for xb in range(32):
            a = ((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2) | xb
            byte = d[a]; at = d[6144 + (y >> 3) * 32 + xb]
            ink = PAL[(at & 7) + (8 if at & 64 else 0)]; paper = PAL[((at >> 3) & 7) + (8 if at & 64 else 0)]
            for b in range(8):
                img[y][xb * 8 + b] = ink if byte & (0x80 >> b) else paper
    return img
files = sys.argv[2:]
imgs = [pixels(open(f, 'rb').read()) for f in files]
W = 256 * len(imgs) + 8 * (len(imgs) - 1); H = 192
rows = []
for y in range(H):
    row = bytearray([0])
    for i, im in enumerate(imgs):
        if i: row += bytes([128, 128, 128]) * 8
        for p in im[y]: row += bytes(p)
    rows.append(bytes(row))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + \
    chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b'')
open(sys.argv[1], 'wb').write(png)
