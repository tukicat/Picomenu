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

# make_font.py <font.hex> <out.inc>: the 96 glyphs of the NMI ROM's font (space to ~, then the
# copyright sign) as defb lines, to paste after charbank: in nmirom/nmirom.asm, from a .hex
# bitmap font (codepoint:16 hex digits, 8 rows of 8 pixels, left pixel in the top bit).
# The font is Unscii 8 (fontfiles/unscii-8.hex of https://github.com/viznut/unscii), public
# domain / CC0. Then run pico/make_help.py, which takes the Help font from nmirom.asm.
import sys
src, out = sys.argv[1], sys.argv[2]
glyph = {}
for l in open(src):
    cp, h = l.strip().split(':')
    if len(h) == 16:
        glyph[int(cp, 16)] = [int(h[i:i+2], 16) for i in range(0, 16, 2)]
lines = []
for c in list(range(32, 127)) + [0xA9]:
    g = glyph.get(c)
    if g is None:
        sys.exit('missing U+%04X' % c)
    name = chr(c) if c != 0xA9 else '(c)'
    lines.append('\tdefb ' + ','.join('0x%02x' % v for v in g) + ' ; ' + name)
open(out, 'w').write('\n'.join(lines) + '\n')
