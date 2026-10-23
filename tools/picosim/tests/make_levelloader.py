#!/usr/bin/env python3
# This file is part of picoFace (GPL v3 or later).
#
# Makes the test files of the levelloader48 script: a game with its own loader in RAM that
# starts as the ROM's LD-BYTES does (as Out Run's level loader is a copy of it), which the tape
# trap can only serve once the Pico has patched it.
#   levelloader.z80  a 48K snapshot: it waits until 0x9000 is not 0, then loads 4 bytes (flag
#                    FF) to 30000 with its loader, until one loads; then 4 at 30010 and a green
#                    border
#   levelloader.tap  10 LOAD ""CODE: RANDOMIZE USR 32768, the same code, then a 4 byte block
#                    (1 2 3 4): the game loads it with its own loader as soon as it starts
# Run: make_levelloader.py [folder]   (the files are written there, or in the current folder)
#
# The loader is PicoMenu's own few bytes, not a copy of the ROM's: it starts as LD-BYTES does
# (the flag in A', the border, its return pushed, then IN A,(FE) / RRA / AND 20) and waits for
# an edge with an LD-EDGE-like loop, which is all the Pico looks for. With no tape signal it
# waits for ever (as a game at PRESS PLAY does); patched, the trap's hook loads the block.
import os, struct, sys

out = sys.argv[1] if len(sys.argv) > 1 else '.'

LD = 0x8100                     # where the loader goes
RET = 0x8200                    # its return (as SA/LD-RET): just RET
src = bytes([
    0x14, 0x08, 0x15, 0xF3,         # INC D / EX AF,AF' / DEC D / DI
    0x3E, 0x0F, 0xD3, 0xFE,         # LD A,0F / OUT (FE),A
    0x21, RET & 0xFF, RET >> 8,     # LD HL,RET
    0xE5,                           # PUSH HL
    0xDB, 0xFE, 0x1F, 0xE6, 0x20,   # IN A,(FE) / RRA / AND 20   (made JP 0x0562 by the Pico)
    0xF6, 0x02, 0x4F,               # OR 2 / LD C,A
    0x3E, 0x7F, 0xDB, 0xFE, 0x1F,   # edge: LD A,7F / IN A,(FE) / RRA
    0xA9, 0xE6, 0x20,               # XOR C / AND 20
    0x28, 0xF6,                     # JR Z,edge (no signal: round for ever)
    0xB7, 0xC9,                     # OR A / RET (never reached in the tests)
])

def program(wait):
    p = bytearray()
    if wait:
        p += bytes([0x3A, 0x00, 0x90, 0xB7, 0x28, 0xFA])       # w: LD A,(9000) / OR A / JR Z,w
    p += bytes([0xDD, 0x21, 0x30, 0x75,                         # l: LD IX,30000
                0x11, 0x04, 0x00,                               # LD DE,4
                0x3E, 0xFF, 0x37,                               # LD A,FF / SCF
                0xCD, LD & 0xFF, LD >> 8,                       # CALL the copy
                0x30, 0xF1,                                     # JR NC,l
                0x3E, 0x04, 0xD3, 0xFE,                         # LD A,4 / OUT (FE),A
                0x21, 0x3A, 0x75, 0x36, 0x04,                   # LD HL,30010 / LD (HL),4
                0x18, 0xFE])                                    # JR $
    return p

def memory(wait):
    m = bytearray(49152)
    code = program(wait)
    m[0x8000 - 0x4000:0x8000 - 0x4000 + len(code)] = code
    m[LD - 0x4000:LD - 0x4000 + len(src)] = src
    m[RET - 0x4000] = 0xC9
    return m

# the .z80 (version 1, not compressed): PC 0x8000, SP 0xFF00, IM 1, interrupts off
h = bytearray(30)
h[6:8] = struct.pack('<H', 0x8000)
h[8:10] = struct.pack('<H', 0xFF00)
h[10] = 0x3F
h[12] = 0x0E                    # border 7
h[29] = 1
mem = memory(True)
open(os.path.join(out, 'levelloader.z80'), 'wb').write(bytes(h) + bytes(mem))

def block(flag, data):
    b = bytes([flag]) + bytes(data)
    x = 0
    for c in b: x ^= c
    return struct.pack('<H', len(b) + 1) + b + bytes([x])
def header(kind, name, length, p1, p2):
    return bytes([kind]) + name.ljust(10).encode() + struct.pack('<HHH', length, p1, p2)
basic = bytes([0x00, 0x0A]) + struct.pack('<H', 0)      # the line's length is filled in below
line = bytes([0xEF, 0x22, 0x22, 0xAF, 0x3A, 0xF9, 0xC0]) + b'32768' + bytes([0x0E, 0, 0, 0x00, 0x80, 0, 0x0D])
basic = bytes([0x00, 0x0A]) + struct.pack('<H', len(line)) + line
code = memory(False)[0x8000 - 0x4000:RET - 0x4000 + 1]
tap = block(0, header(0, 'level', len(basic), 10, len(basic)))
tap += block(0xFF, basic)
tap += block(0, header(3, 'levelcode', len(code), 0x8000, 32768))
tap += block(0xFF, code)
tap += block(0xFF, [1, 2, 3, 4])
open(os.path.join(out, 'levelloader.tap'), 'wb').write(tap)
print('levelloader.z80, levelloader.tap')
