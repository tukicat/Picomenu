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
#
# make_webfont.py <nmirom.asm> <out.inc>: the NMI menu's font (the 96 characters after
# "charbank:", space to ~ and the copyright sign; Unscii 8, public domain) as a web font for
# the file transfer's page (pico/webpage.cxx), so the browser shows the same letters as the
# Spectrum. Each pixel is a square (runs of pixels in a row are one rectangle), 8 x 8 to the
# em, so at 16px (or any multiple of 8) the letters are sharp. The result is a WOFF file,
# written as a C string of its base64 (for a data: URL in the page's CSS). Needs fontTools
# (pip install fonttools). The copyright sign, the 96th, is put at U+00A9.
import base64, io, re, sys
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

asm_path, out_path = sys.argv[1], sys.argv[2]
font = []
inside = False
for ln in open(asm_path, encoding='utf-8', errors='replace'):
    if ln.startswith('charbank:'):
        inside = True
        continue
    if inside:
        m = re.match(r'\s*defb\s+((?:0x[0-9a-fA-F]{2},?){8})', ln)
        if m:
            font.append([int(x, 16) for x in m.group(1).split(',')])
            if len(font) == 96:
                break
if len(font) != 96:
    sys.exit('could not read the font from %s (got %d)' % (asm_path, len(font)))

PX = 128                    # a pixel, in font units: 8 to the em
EM = 8 * PX
ASCENT, DESCENT = 7 * PX, 1 * PX    # the cell's top row 7 pixels above the baseline, its last below

codes = list(range(32, 127)) + [0xA9]
names = ['.notdef'] + ['c%02x' % c for c in codes]
cmap = {c: 'c%02x' % c for c in codes}
glyphs, metrics = {}, {}

def outline(rows):
    pen = TTGlyphPen(None)
    for r, bits in enumerate(rows):
        top = ASCENT - r * PX
        x = 0
        while x < 8:
            if bits & (0x80 >> x):
                start = x
                while x < 8 and bits & (0x80 >> x):
                    x += 1
                # a rectangle, clockwise (TrueType's outer contours)
                x0, x1, y0, y1 = start * PX, x * PX, top - PX, top
                pen.moveTo((x0, y0)); pen.lineTo((x0, y1)); pen.lineTo((x1, y1)); pen.lineTo((x1, y0))
                pen.closePath()
            else:
                x += 1
    return pen.glyph()

glyphs['.notdef'] = outline([0x7E, 0x42, 0x42, 0x42, 0x42, 0x42, 0x7E, 0x00])
metrics['.notdef'] = (EM, 0)
for i, c in enumerate(codes):
    glyphs['c%02x' % c] = outline(font[i])
    metrics['c%02x' % c] = (EM, 0)

fb = FontBuilder(EM, isTTF=True)
fb.setupGlyphOrder(names)
fb.setupCharacterMap(cmap)
fb.setupGlyf(glyphs)
fb.setupHorizontalMetrics(metrics)
fb.setupHorizontalHeader(ascent=ASCENT, descent=-DESCENT)
fb.setupNameTable({'familyName': 'PicoMenu', 'styleName': 'Regular',
                   'copyright': 'Unscii 8 by Viznut, public domain (CC0)'})
fb.setupOS2(sTypoAscender=ASCENT, sTypoDescender=-DESCENT, sTypoLineGap=0,
            usWinAscent=ASCENT, usWinDescent=DESCENT, fsType=0)
fb.setupPost(isFixedPitch=1)
fb.font.flavor = 'woff'
buf = io.BytesIO()
fb.save(buf)
data = buf.getvalue()
b64 = base64.b64encode(data).decode('ascii')
with open(out_path, 'w') as o:
    o.write('// Made by tools/make_webfont.py from nmirom/nmirom.asm (the menu font, Unscii 8, public\n')
    o.write('// domain): a WOFF web font, %d bytes, as base64\n' % len(data))
    for i in range(0, len(b64), 100):
        o.write('"%s"\n' % b64[i:i + 100])
print('%s: %d bytes of WOFF, %d of base64' % (out_path, len(data), len(b64)))
