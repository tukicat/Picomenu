#!/usr/bin/env python3
# This file is part of PicoMenu (picoFace), GNU GPL v3 or later.
#
# Proves the .z80 to .tap converter: makes test snapshots (random memory with long runs, random
# registers, the program a DI / JR $ loop so that nothing changes once it has loaded), turns
# each into a .tap (z80tap_host), loads the tape on an emulated Spectrum through the ROM's own
# loader (../tzxconv/tzxconv_host with TZXNOFLASH=1: every edge played) and compares the
# snapshot it makes with the first: memory, registers, paging and the sound chip.
#   python3 roundtrip.py 48.rom 128.rom [work folder]
import os, random, struct, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
rom48, rom128 = sys.argv[1], sys.argv[2]
work = sys.argv[3] if len(sys.argv) > 3 else '/tmp/z80tap-rt'
os.makedirs(work, exist_ok=True)

def decode_block(data, i, n):
    out = bytearray()
    end = i + n
    while i < end:
        if data[i] == 0xED and i + 1 < end and data[i + 1] == 0xED:
            out += bytes([data[i + 3]]) * data[i + 2]; i += 4
        else:
            out.append(data[i]); i += 1
    return bytes(out)

def read_z80(path):
    d = open(path, 'rb').read()
    h = bytearray(d[:30])
    extlen = d[30] | (d[31] << 8)
    x = d[32:32 + extlen]
    pc = x[0] | (x[1] << 8)
    hw = x[2]
    is128 = hw >= 3 and hw != 14 if extlen > 23 else hw >= 3
    i = 32 + extlen
    pages = {}
    while i + 3 <= len(d):
        n = d[i] | (d[i + 1] << 8); p = d[i + 2]; i += 3
        if n == 0xFFFF:
            pages[p] = d[i:i + 16384]; i += 16384
        else:
            pages[p] = decode_block(d, i, n); i += n
    return h, pc, x, is128, pages

def write_z80(path, h, pc, is128, pages, p7ffd=0, aysel=0, ay=bytes(16)):
    x = bytearray(54)
    x[0] = pc & 255; x[1] = pc >> 8
    x[2] = 4 if is128 else 0
    x[3] = p7ffd
    x[6] = aysel
    x[7:23] = ay
    with open(path, 'wb') as f:
        f.write(bytes(h) + struct.pack('<H', 54) + bytes(x))
        for p in sorted(pages):
            f.write(struct.pack('<HB', 0xFFFF, p) + pages[p])

def fill_random(rng, n):
    b = bytearray(rng.getrandbits(8) for _ in range(n))
    for _ in range(rng.randint(0, 4)):         # some long runs
        a = rng.randrange(n); l = rng.randint(100, 3000); v = rng.choice([0, 0xFF, 0xAA, rng.getrandbits(8)])
        b[a:a + l] = bytes([v]) * len(b[a:a + l])
    return b

def make(seed, is128, sp, pc, ei=False):
    rng = random.Random(seed)
    h = bytearray(rng.getrandbits(8) for _ in range(30))
    h[6] = h[7] = 0
    h[8] = sp & 255; h[9] = (sp >> 8) & 255
    h[12] = (h[12] & 0x0F) & ~0x20              # border, R bit 7; not compressed
    h[12] = (h[12] & 0x0F) | 0x00
    h[27] = h[28] = 1 if ei else 0              # DI (so nothing changes once it runs)
    h[29] = 1 if ei else rng.choice([1, 2])
    if ei: h[10] = 0x3F
    pages = {}
    if is128:
        for b in range(8):
            pages[3 + b] = fill_random(rng, 16384)
        p7ffd = rng.choice([0, 1, 3, 4, 6, 7]) | rng.choice([0, 8]) | 0x10
    else:
        for p in (4, 5, 8):
            pages[p] = fill_random(rng, 16384)
        p7ffd = 0
    def put(addr, val):                         # at addr as the game sees it
        if addr < 0x8000: page = 8 if not is128 else 3 + 5
        elif addr < 0xC000: page = 4 if not is128 else 3 + 2
        else: page = 5 if not is128 else 3 + (p7ffd & 7)
        pages[page][addr & 0x3FFF] = val
    put(pc, 0x18); put(pc + 1, 0xFE)             # JR $
    ay = bytes(rng.getrandbits(8) & 0x1F for _ in range(16))
    return h, pages, p7ffd, rng.getrandbits(4), ay

def mem_of(is128, pages, p7ffd):
    if not is128:
        return pages[8] + pages[4] + pages[5]
    return {b: pages[3 + b] for b in range(8)}

cases = [
    ('48 high stack', 1, False, 0xFF40, 0x9000),
    ('48 stack at 0', 2, False, 0x0000, 0xA123),
    ('48 low stack', 3, False, 0x6100, 0x8000),
    ('48 stack by the loader', 4, False, 0xBC80, 0x7000),
    ('128 stack in the paged bank', 5, True, 0xF800, 0x8100),
    ('128 stack in bank 5', 6, True, 0x7000, 0x9000),
    ('48 interrupts on (IM 1)', 7, False, 0xFE00, 0x9000, True),
]
fails = 0
for case in cases:
    name, seed, is128, sp, pc = case[:5]
    ei = len(case) > 5
    h, pages, p7ffd, aysel, ay = make(seed, is128, sp, pc, ei)
    src = os.path.join(work, 't%d.z80' % seed)
    tap = os.path.join(work, 't%d.tap' % seed)
    back = os.path.join(work, 't%d-back.z80' % seed)
    write_z80(src, h, pc, is128, pages, p7ffd, aysel, ay)
    r = subprocess.run([os.path.join(here, 'z80tap_host'), src, tap, 'test%d' % seed], capture_output=True, text=True)
    print(name + ': ' + (r.stdout.strip() or r.stderr.strip()))
    if r.returncode != 0:
        fails += 1; continue
    env = dict(os.environ, TZXNOFLASH='1')
    r = subprocess.run([os.path.join(here, '..', 'tzxconv', 'tzxconv_host'), rom128 if is128 else rom48, tap, back, '200'],
                       capture_output=True, text=True, env=env)
    if not os.path.exists(back):
        print('  FAIL: the tape did not load\n' + r.stdout[-400:]); fails += 1; continue
    bh, bpc, bx, b128, bpages = read_z80(back)
    problems = []
    if bpc != pc: problems.append('PC %04X not %04X' % (bpc, pc))
    regs = {'A': 0, 'F': 1, 'BC': (2, 3), 'HL': (4, 5), 'SP': (8, 9), 'I': 10, 'DE': (13, 14), "BC'": (15, 16),
            "DE'": (17, 18), "HL'": (19, 20), "A'": 21, "F'": 22, 'IY': (23, 24), 'IX': (25, 26)}
    for k, v in regs.items():
        if isinstance(v, tuple):
            a, b = h[v[0]] | (h[v[1]] << 8), bh[v[0]] | (bh[v[1]] << 8)
        else:
            a, b = h[v], bh[v]
        if a != b: problems.append('%s %X not %X' % (k, b, a))
    if (bh[29] & 3) != (h[29] & 3): problems.append('IM %d not %d' % (bh[29] & 3, h[29] & 3))
    if bh[27] != h[27]: problems.append('IFF %d not %d' % (bh[27], h[27]))
    if ((bh[12] >> 1) & 7) != ((h[12] >> 1) & 7): problems.append('border')
    # memory, except the stub and its stack just below SP
    spv = sp if sp else 0x10000
    dead = set(range(spv - 90 - 16 - 8, spv))
    if ei: dead |= set(range(0x5C00, 0x5CC0))       # the ROM's interrupt routine writes its system variables
    if ei: dead |= set(range(spv - 140, spv))
    if not is128:
        a = mem_of(False, pages, 0); b = mem_of(False, bpages, 0)
        diff = [0x4000 + i for i in range(49152) if a[i] != b[i] and (0x4000 + i) not in dead]
    else:
        if (bx[3] & 0x3F) != (p7ffd & 0x3F): problems.append('7FFD %02X not %02X' % (bx[3], p7ffd))
        if bytes(bx[7:23]) != ay: problems.append('AY registers')
        diff = []
        for bk in range(8):
            a = pages[3 + bk]; b = bpages.get(3 + bk, bytes(16384))
            base = 0x4000 if bk == 5 else 0x8000 if bk == 2 else 0xC000
            for i in range(16384):
                if a[i] != b[i]:
                    addr = base + i
                    paged = bk == (p7ffd & 7) or base < 0xC000
                    if not (paged and addr in dead):
                        diff.append((bk, addr))
    if diff: problems.append('%d bytes of memory differ, first %s' % (len(diff), diff[:3]))
    if problems:
        fails += 1; print('  FAIL: ' + '; '.join(problems))
    else:
        print('  same memory, registers%s' % (', paging and sound chip' if is128 else ''))
print('%d failure(s)' % fails)
sys.exit(1 if fails else 0)
