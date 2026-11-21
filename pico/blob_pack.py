# This file is part of PicoMenu (picoFace), GNU GPL v3 or later.
#
# Packs what the firmware keeps packed (the NMI ROM and its routines, the Help text, the set up
# texts), as blob_unpack() in the generated nmi.c undoes it: LZ77 with Elias gamma codes, chosen
# by optimal parsing (about 12% smaller than the LZSS it replaces, and as quick to unpack).
#
# The stream mixes bytes and bits: a bit is taken from a byte of its own when the last one is
# used up (most significant bit first), at the point the unpacker needs it. Then until n bytes
# are out:
#   bit 0: a literal, the next byte
#   bit 1: a match: gamma(length - 1), gamma(((offset - 1) >> 8) + 1), then a byte, the offset
#          less one's low 8 bits; length 2-256, offset 1-65536
# gamma(v), v >= 1 of k bits: k - 1 zero bits, then v's k bits.
MAXLEN = 256
WINDOW = 65536
CHAIN = 400


def _gamma_bits(v):
    return 2 * v.bit_length() - 1


def compress(d):
    d = bytes(d)
    n = len(d)
    heads = {}
    # for each place: (offset, longest length) of earlier matches, nearest first
    cands = [None] * n
    for i in range(n):
        if i + 2 <= n:
            key = d[i:i + 2]
            lst = heads.setdefault(key, [])
            res = []
            longest = 1
            for j in reversed(lst[-CHAIN:]):
                if i - j > WINDOW:
                    break
                l = 0
                while i + l < n and l < MAXLEN and d[j + l] == d[i + l]:
                    l += 1
                if l > longest:             # (a farther match is only worth it if longer)
                    res.append((i - j, l))
                    longest = l
                    if l == MAXLEN:
                        break
            cands[i] = res
            lst.append(i)
    cost = [0] * (n + 1)
    choice = [(0, 1)] * n             # (offset, length): offset 0 is a literal
    for i in range(n - 1, -1, -1):
        best = 9 + cost[i + 1]
        ch = (0, 1)
        lo = 2
        for off, ml in cands[i] or ():
            oc = 1 + _gamma_bits(((off - 1) >> 8) + 1) + 8
            for l in range(lo, ml + 1):
                c = oc + _gamma_bits(l - 1) + cost[i + l]
                if c < best:
                    best = c
                    ch = (off, l)
            lo = max(lo, ml + 1)
        cost[i] = best
        choice[i] = ch
    out = bytearray()
    state = {'at': -1, 'left': 0}

    def bit(b):
        if state['left'] == 0:
            state['at'] = len(out)
            out.append(0)
            state['left'] = 8
        state['left'] -= 1
        if b:
            out[state['at']] |= 1 << state['left']

    def gamma(v):
        k = v.bit_length()
        for _ in range(k - 1):
            bit(0)
        for s in range(k - 1, -1, -1):
            bit((v >> s) & 1)

    i = 0
    while i < n:
        off, l = choice[i]
        if off == 0:
            bit(0)
            out.append(d[i])
        else:
            bit(1)
            gamma(l - 1)
            gamma(((off - 1) >> 8) + 1)
            out.append((off - 1) & 0xFF)
        i += l
    return bytes(out)


def decompress(c, n):
    c = bytes(c)
    st = {'p': 0, 'buf': 0, 'left': 0}

    def byte():
        b = c[st['p']]
        st['p'] += 1
        return b

    def bit():
        if st['left'] == 0:
            st['buf'] = byte()
            st['left'] = 8
        st['left'] -= 1
        return (st['buf'] >> st['left']) & 1

    def gamma():
        k = 1
        while bit() == 0:
            k += 1
        v = 1
        for _ in range(k - 1):
            v = (v << 1) | bit()
        return v

    o = bytearray()
    while len(o) < n:
        if bit() == 0:
            o.append(byte())
        else:
            l = gamma() + 1
            off = (((gamma() - 1) << 8) | byte()) + 1
            for _ in range(l):
                o.append(o[-off])
    assert st['p'] == len(c)
    return bytes(o[:n])
