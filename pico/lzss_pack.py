# This file is part of PicoMenu (picoFace), GNU GPL v3 or later.
#
# LZSS, as lzss_unpack() in the generated nmi.c undoes it: used for the NMI ROM
# (asm_to_c_file.py) and the Help text (make_help.py), so they take less of the firmware.
# LZSS: a flag byte for each 8 items (bit set: a literal byte; clear: a match of 2 bytes,
# offset 1..4096 in 12 bits and length 3..18 in 4 bits)
def compress(d):
    # optimal parsing: the cheapest choice of literals and matches for the whole input (a
    # literal costs 9 bits, a match 17), found from the end; the longest match at each place
    # (and the nearest offset for it) from hash chains over the last 4096 bytes
    n = len(d); heads = {}; maxl = [0] * n; offs = [0] * n
    for i in range(n):
        if i + 3 <= n:
            key = d[i:i+3]; best = 0; bo = 0
            for j in reversed(heads.get(key, [])):
                if i - j > 4096: break
                l = 3
                while l < 18 and i + l < n and d[j + l] == d[i + l]: l += 1
                if l > best: best, bo = l, i - j
                if l == 18: break
            maxl[i] = best; offs[i] = bo
            heads.setdefault(key, []).append(i)
    cost = [0] * (n + 1); choice = [1] * n
    for i in range(n - 1, -1, -1):
        c = 9 + cost[i + 1]; ch = 1
        for l in range(3, maxl[i] + 1):
            cc = 17 + cost[i + l]
            if cc < c: c, ch = cc, l
        cost[i] = c; choice[i] = ch
    out = bytearray(); i = 0
    while i < n:
        flagpos = len(out); out.append(0); flags = 0
        for bit in range(8):
            if i >= n: break
            l = choice[i]
            if l >= 3:
                v = ((offs[i] - 1) << 4) | (l - 3)      # (a match of l: the longest's offset has it)
                out += bytes([v & 0xFF, v >> 8])
                i += l
            else:
                flags |= 1 << bit
                out.append(d[i]); i += 1
        out[flagpos] = flags
    return bytes(out)

def decompress(c, n):
    out = bytearray(); i = 0
    while len(out) < n:
        flags = c[i]; i += 1
        for bit in range(8):
            if len(out) >= n: break
            if flags & (1 << bit):
                out.append(c[i]); i += 1
            else:
                v = c[i] | (c[i+1] << 8); i += 2
                off = (v >> 4) + 1; l = (v & 15) + 3
                for _ in range(l): out.append(out[-off])
    return bytes(out)

