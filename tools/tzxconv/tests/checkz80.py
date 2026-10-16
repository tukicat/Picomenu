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

"""Checks a .z80 made by the converter from a test tape: python3 checkz80.py file.z80 [turbo]"""
import sys, struct, os
HERE = os.path.dirname(os.path.abspath(__file__))

def load_z80(path):
    d = open(path, 'rb').read()
    h = d[:30]
    assert struct.unpack('<H', h[6:8])[0] == 0, 'expected a version 2/3 file'
    extra = struct.unpack('<H', d[30:32])[0]
    pc = struct.unpack('<H', d[32:34])[0]
    hw = d[34]
    assert hw == 0, 'expected a 48K snapshot'
    mem = bytearray(65536)
    pos = 32 + extra
    where = {8: 0x4000, 4: 0x8000, 5: 0xC000}
    while pos < len(d):
        clen, page = struct.unpack('<HB', d[pos:pos + 3]); pos += 3
        if clen == 0xFFFF:
            block = d[pos:pos + 16384]; pos += 16384
        else:
            src = d[pos:pos + clen]; pos += clen
            block = bytearray(); i = 0
            while i < len(src):
                if src[i] == 0xED and i + 1 < len(src) and src[i + 1] == 0xED:
                    block += bytes([src[i + 3]]) * src[i + 2]; i += 4
                else:
                    block.append(src[i]); i += 1
        assert len(block) == 16384, f'page {page} is {len(block)} bytes'
        mem[where[page]:where[page] + 16384] = block
    regs = dict(A=h[0], F=h[1], BC=struct.unpack('<H', h[2:4])[0], HL=struct.unpack('<H', h[4:6])[0],
                SP=struct.unpack('<H', h[8:10])[0], I=h[10], R=(h[11] & 0x7F) | ((h[12] & 1) << 7),
                border=(h[12] >> 1) & 7, IFF=h[27], IM=h[29] & 3, PC=pc)
    return mem, regs

mem, r = load_z80(sys.argv[1])
turbo = len(sys.argv) > 2 and sys.argv[2] == 'turbo'
game = open(os.path.join(HERE, 'game.bin'), 'rb').read()
problems = []
if mem[0x8000:0x8800] != game: problems.append('game at 0x8000 differs')
if mem[0x9000:0x9006] != b'GAMEOK': problems.append('no GAMEOK at 0x9000')
if struct.unpack('<H', mem[0x9010:0x9012])[0] == 0: problems.append('the game has not run (frame count 0)')
if not (0x8016 <= r['PC'] <= 0x8024): problems.append(f"PC {r['PC']:04X} not in the game's loop")
if r['IM'] != 1 or not r['IFF']: problems.append('interrupts not as the game set them')
if r['border'] != 5: problems.append(f"border {r['border']} (the game sets 5)")
if turbo:
    screen = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(6144)) + bytes((0x38 + (i % 7)) for i in range(768))
    if mem[0x4000:0x5B00] != screen: problems.append('screen (turbo block 1) differs')
print(('OK ' if not problems else 'FAIL ') + os.path.basename(sys.argv[1]) +
      f"  PC={r['PC']:04X} SP={r['SP']:04X} frames={struct.unpack('<H', mem[0x9010:0x9012])[0]}" +
      ''.join('\n   - ' + p for p in problems))
sys.exit(1 if problems else 0)
