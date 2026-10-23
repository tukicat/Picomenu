# This file is part of PicoMenu (picoFace), GNU GPL v3 or later.
#
# LZSS, as lzss_unpack() in the generated nmi.c undoes it: used for the NMI ROM
# (asm_to_c_file.py) and the Help text (make_help.py), so they take less of the firmware.
# LZSS: a flag byte for each 8 items (bit set: a literal byte; clear: a match of 2 bytes,
# offset 1..4096 in 12 bits and length 3..18 in 4 bits)
def compress(d):
    out = bytearray(); i = 0; n = len(d)
    heads = {}
    while i < n:
        flagpos = len(out); out.append(0); flags = 0
        for bit in range(8):
            if i >= n: break
            best_len = 0; best_off = 0
            if i + 3 <= n:
                key = d[i:i+3]
                for j in reversed(heads.get(key, [])[-256:]):
                    if i - j > 4096: break
                    l = 3
                    while l < 18 and i + l < n and d[j + l] == d[i + l]: l += 1
                    if l > best_len: best_len, best_off = l, i - j
                    if l == 18: break
            if best_len >= 3:
                v = ((best_off - 1) << 4) | (best_len - 3)
                out += bytes([v & 0xFF, v >> 8])
                step = best_len
            else:
                flags |= 1 << bit
                out.append(d[i]); step = 1
            for k in range(i, min(i + step, n - 2)):
                heads.setdefault(d[k:k+3], []).append(k)
            i += step
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

