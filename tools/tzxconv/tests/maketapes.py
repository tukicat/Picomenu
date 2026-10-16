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

"""Makes the test tapes for the tape converter, from game.bin and turbo.bin (sjasmplus).

Every tape loads the same thing; what differs is how the signal is written in the file.
Expected result for all the 'ok' tapes: GAMEOK at 0x9000, the game at 0x8000 as in
game.bin, the screen pattern at 0x4000, PC in the game's loop.
"""
import struct, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'out')
os.makedirs(OUT, exist_ok=True)
game = open(os.path.join(HERE, 'game.bin'), 'rb').read()
turbo = open(os.path.join(HERE, 'turbo.bin'), 'rb').read()

# ---- BASIC
def num(n):
    """A number as BASIC keeps it: the digits, then 0x0E and the 5 byte small integer form."""
    return str(n).encode() + bytes([0x0E, 0, 0, n & 0xFF, n >> 8, 0])

def basic_line(no, body):
    body = body + b'\x0d'
    return struct.pack('>H', no) + struct.pack('<H', len(body)) + body

CLEAR, LOAD, CODE, RANDOMIZE, USR = 0xFD, 0xEF, 0xAF, 0xF9, 0xC0
def loader_basic(clear, usr):
    return basic_line(10, bytes([CLEAR]) + num(clear) + b':' + bytes([LOAD]) + b'""' + bytes([CODE]) +
                      b':' + bytes([RANDOMIZE, USR]) + num(usr))

def header(kind, name, length, p1, p2):
    return bytes([kind]) + name.ljust(10)[:10].encode() + struct.pack('<HHH', length, p1, p2)

def rom_block(flag, data):
    """flag + data + the ROM's XOR checksum"""
    b = bytes([flag]) + data
    x = 0
    for c in b:
        x ^= c
    return b + bytes([x])

# ---- the standard part: a BASIC loader and the turbo loader as CODE
def standard_blocks(code=turbo, addr=0x6000, usr=0x6000, clear=24575):
    prog = loader_basic(clear, usr)
    return [rom_block(0, header(0, 'turbotest', len(prog), 10, len(prog))), rom_block(0xFF, prog),
            rom_block(0, header(3, 'loader', len(code), addr, 32768)), rom_block(0xFF, code)]

# ---- the turbo part
screen = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(6144)) + bytes((0x38 + (i % 7)) for i in range(768))

def r_key(k):
    # LD R,A with A=0x80 sets R; LD HL and LD BC add 2; LD A,R reads it after its own 2;
    # 9 increments a loop; bit 7 stays
    return 0x80 | ((4 + 9 * k) & 0x7F)

game_enc = bytes(b ^ r_key(k) for k, b in enumerate(game))

def turbo_payload(flag, data):
    x = 0
    for c in data:
        x ^= c
    return bytes([flag]) + data + bytes([x])

T_PILOT, T_SYNC, T_ZERO, T_ONE, N_PILOT = 1200, 400, 400, 800, 1500
TURBO = [turbo_payload(1, screen), turbo_payload(2, game_enc)]

def pulses_of(data, pilot=True):
    """The turbo signal of a block as pulse lengths (each one an edge)"""
    p = [T_PILOT] * N_PILOT + [T_SYNC, T_SYNC] if pilot else []
    for byte in data:
        for i in range(8):
            t = T_ONE if byte & (0x80 >> i) else T_ZERO
            p += [t, t]
    return p

# ---- TZX blocks
def tzx(blocks):
    return b'ZXTape!\x1a\x01\x14' + b''.join(blocks)

def b10(data, pause=1000):
    return b'\x10' + struct.pack('<HH', pause, len(data)) + data

def b11(data, pause=1000):
    return b'\x11' + struct.pack('<HHHHHHBH', T_PILOT, T_SYNC, T_SYNC, T_ZERO, T_ONE, N_PILOT, 8, pause) + \
        struct.pack('<I', len(data))[:3] + data

def b12(length, count):
    return b'\x12' + struct.pack('<HH', length, count)

def b13(pulses):
    return b'\x13' + bytes([len(pulses)]) + b''.join(struct.pack('<H', p) for p in pulses)

def b14(data, pause=1000):
    return b'\x14' + struct.pack('<HHBH', T_ZERO, T_ONE, 8, pause) + struct.pack('<I', len(data))[:3] + data

def b20(ms):
    return b'\x20' + struct.pack('<H', ms)

def levels(pulses, start_level=0):
    """pulse lengths -> (level, duration) runs, each pulse starting with an edge"""
    out, lv = [], start_level
    for p in pulses:
        lv ^= 1
        out.append((lv, p))
    return out

def b15(pulses, pause=1000, tps=79):
    """direct recording: one bit a sample of tps T, from the level runs"""
    bits = []
    acc = 0.0
    for lv, d in levels(pulses):
        acc += d / tps
        n = int(round(acc))
        acc -= n
        bits += [lv] * n
    used = len(bits) % 8 or 8
    bits += [0] * (-len(bits) % 8)
    data = bytes(int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))
    return b'\x15' + struct.pack('<HHB', tps, pause, used) + struct.pack('<I', len(data))[:3] + data

def b18(pulses, pause=1000, rate=44100):
    """CSW, plain RLE"""
    out = bytearray()
    acc = 0.0
    for p in pulses:
        acc += p * rate / 3500000
        n = int(round(acc))
        acc -= n
        n = max(n, 1)
        out += bytes([n]) if n < 256 else b'\x00' + struct.pack('<I', n)
    body = struct.pack('<H', pause) + struct.pack('<I', rate)[:3] + bytes([1]) + \
        struct.pack('<I', len(pulses)) + bytes(out)
    return b'\x18' + struct.pack('<I', len(body)) + body

def b19(data, pause=1000):
    """generalised data: pilot symbol 0 (one edge, T_PILOT), sync symbol 1 (2 edges);
    data symbols: 0 = 2 edges of T_ZERO, 1 = 2 edges of T_ONE"""
    npp, asp = 2, 2
    pdef = bytes([0]) + struct.pack('<HH', T_PILOT, 0) + bytes([0]) + struct.pack('<HH', T_SYNC, T_SYNC)
    prle = bytes([0]) + struct.pack('<H', N_PILOT) + bytes([1]) + struct.pack('<H', 1)
    npd, asd = 2, 2
    ddef = bytes([0]) + struct.pack('<HH', T_ZERO, T_ZERO) + bytes([0]) + struct.pack('<HH', T_ONE, T_ONE)
    totd = len(data) * 8
    body = struct.pack('<HIBBIBB', pause, 2, npp, asp, totd, npd, asd) + pdef + prle + ddef + data
    return b'\x19' + struct.pack('<I', len(body)) + body

std = [b10(b) for b in standard_blocks()]

tapes = {}
tapes['standard_rom'] = None        # made below as a .tap and a .tzx
tapes['turbo_11'] = tzx(std + [b11(d) for d in TURBO])
tapes['tone_pulses_14'] = tzx(std + sum(([b12(T_PILOT, N_PILOT), b13([T_SYNC, T_SYNC]), b14(d)] for d in TURBO), []))
tapes['direct_15'] = tzx(std + [b15(pulses_of(d)) for d in TURBO])
tapes['csw_18'] = tzx(std + [b18(pulses_of(d)) for d in TURBO])
tapes['generalised_19'] = tzx(std + [b19(d) for d in TURBO])
# the pilot as a loop of 15 tones of 100 pulses, the 2 turbo blocks reached with a jump over junk
loop_pilot = lambda d: [b'\x24' + struct.pack('<H', 15), b12(T_PILOT, 100), b'\x25',
                        b13([T_SYNC, T_SYNC]), b14(d)]
blocks = std + [b'\x23' + struct.pack('<h', 2), b10(b'\x00junk junk', 0)] + loop_pilot(TURBO[0]) + loop_pilot(TURBO[1])
tapes['loop_jump'] = tzx(blocks)
# call sequence: the two turbo blocks are called (in the order 1, 2) from a call block before them
# blocks: 4 the calls (to 6 and 8), 5 a jump to 10 (where play goes on after the calls)
calls = std + [b'\x26' + struct.pack('<Hhh', 2, 2, 4), b'\x23' + struct.pack('<h', 5),
               b11(TURBO[0]), b'\x27', b11(TURBO[1]), b'\x27', b20(500)]
tapes['call_seq'] = tzx(calls)
# a "stop the tape" between the two turbo blocks (played through)
tapes['stop_20'] = tzx(std + [b11(TURBO[0]), b20(0), b11(TURBO[1])])
# text, archive info and a group around everything: skipped
tapes['info_blocks'] = tzx([b'\x30\x05hello', b'\x32' + struct.pack('<HB', 8, 1) + b'\x00\x05title', b'\x21\x04game'] +
                           std + [b11(d) for d in TURBO] + [b'\x22'])
# should fail: a byte of the game damaged (the loader's checksum fails: red border, HALT)
bad = bytearray(TURBO[1]); bad[100] ^= 0xFF
tapes['bad_turbo_checksum'] = tzx(std + [b11(TURBO[0]), b11(bytes(bad))])
# should fail: a byte of the standard loader damaged (the ROM says R Tape loading error)
sb = standard_blocks(); sb3 = bytearray(sb[3]); sb3[50] ^= 0xFF; sb[3] = bytes(sb3)
tapes['bad_rom_checksum'] = tzx([b10(b) for b in sb] + [b11(d) for d in TURBO])
# should fail: an unknown block id
tapes['unknown_block'] = tzx(std + [b'\x99\x01\x02'] + [b11(d) for d in TURBO])
# should be "maybe": the turbo blocks missing (the loader waits for ever)
tapes['missing_turbo'] = tzx(std)

for name, data in tapes.items():
    if data is not None:
        open(os.path.join(OUT, name + '.tzx'), 'wb').write(data)

# the plain ROM loader: the game as CODE, run by the BASIC
plain = standard_blocks(code=game, addr=0x8000, usr=0x8000, clear=32767)
open(os.path.join(OUT, 'standard_rom.tap'), 'wb').write(b''.join(struct.pack('<H', len(b)) + b for b in plain))
open(os.path.join(OUT, 'standard_rom.tzx'), 'wb').write(tzx([b10(b) for b in plain]))
print('tapes in', OUT, ':', ', '.join(sorted(os.listdir(OUT))))
