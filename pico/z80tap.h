/*
This file is part of picoFace.

picoFace is free software: you can redistribute it and/or modify it under the terms of
the GNU General Public License as published by the Free Software Foundation, either
version 3 of the License, or (at your option) any later version.

picoFace is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with picoFace. If
not, see <https://www.gnu.org/licenses/>.
*/


// .z80 snapshot to .tap (T on the Batch convert screen): a tape a real Spectrum loads at the
// normal speed, with LOAD "". On it:
//   a BASIC program: 10 CLEAR VAL "R-1": LOAD ""CODE : RANDOMIZE USR VAL "R"
//   the loader (tools/z80tap/loader.asm, z80tap_loader.h), LC_SIZE bytes at R
//   the game's memory: the screen first (so it shows while the rest loads), then the rest,
//   a 128K one bank by bank; a run of 512 or more of the same byte is filled by the loader
//   instead of being loaded
//   last, the game's own LC_SIZE bytes at R, loaded by the loader's stub, which runs just below
//   the game's stack pointer (memory the game is not using) and then sets every register.
// R is the first of a few places that is clear of that stub. A 128K snapshot needs a 128K
// Spectrum (it is loaded with the Tape Loader, or LOAD "" in 128 BASIC).
// Nothing here touches the hardware, so it is tested on a PC (with the tape converter: the
// tape is loaded by the ROM's own loader on an emulated Spectrum, and the memory compared).

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "z80tap_loader.h"

namespace z80tap {

// The snapshot: the 30 byte .z80 header (snapload's, with X_7FFD, X_AYSEL, X_AYREGS after
// it for a 128K one) and its memory
struct Snap {
    const uint8_t* hdr;
    bool is128;
    const uint8_t* ram48;           // 48K: 0x4000 on
    const uint8_t* bank[8];         // 128K
};

static const uint16_t FILL_MIN = 512;
static const uint16_t STACK_ROOM = 32;      // the loader's stack, at the end of its LC_SIZE bytes

struct Mem {
    const Snap& s;
    // the byte the game has at a (with bank paged at 0xC000)
    uint8_t at(uint32_t a, int bank) const {
        if (!s.is128) return s.ram48[a - 0x4000];
        if (a < 0x8000) return s.bank[5][a - 0x4000];
        if (a < 0xC000) return s.bank[2][a - 0x8000];
        return s.bank[bank][a - 0xC000];
    }
    const uint8_t* ptr(uint32_t a, int bank) const {
        if (!s.is128) return s.ram48 + (a - 0x4000);
        if (a < 0x8000) return s.bank[5] + (a - 0x4000);
        if (a < 0xC000) return s.bank[2] + (a - 0x8000);
        return s.bank[bank] + (a - 0xC000);
    }
};

// One thing the loader does
struct Op {
    uint8_t type;       // 1 load, 2 fill, 3 page
    uint16_t addr;
    uint16_t len;
    uint8_t value;      // fill: the byte; page: port 0x7FFD
    int8_t bank;        // which bank is at 0xC000 (for the bytes of a load)
};

// The ops for the bytes from a to b (b up to 0x10000), runs of FILL_MIN or more as fills
template <class Add>
inline void split(const Mem& m, uint32_t a, uint32_t b, int bank, uint16_t fill_min, Add add) {
    uint32_t start = a;
    uint32_t i = a;
    while (i < b) {
        uint8_t v = m.at(i, bank);
        uint32_t j = i + 1;
        while (j < b && m.at(j, bank) == v) j++;
        if (j - i >= fill_min) {
            if (i > start) add(Op{ 1, (uint16_t) start, (uint16_t) (i - start), 0, (int8_t) bank });
            add(Op{ 2, (uint16_t) i, (uint16_t) (j - i), v, (int8_t) bank });
            start = j;
        }
        i = j;
    }
    if (b > start) add(Op{ 1, (uint16_t) start, (uint16_t) (b - start), 0, (int8_t) bank });
}

struct Plan {
    uint16_t r = 0;             // where the loader goes
    uint16_t stub = 0;          // where its stub goes
    uint32_t sp = 0;            // the game's stack pointer (0x10000 for 0)
    int paged = 0;              // 128K: the bank at 0xC000
    static const int OPS_MAX = 160;
    Op ops[OPS_MAX];
    int count = 0;
    bool full = false;
    void add(const Op& o) {
        if (count < OPS_MAX) ops[count++] = o;
        else full = true;
    }
    size_t table_bytes() const {
        size_t n = 1;
        for (int i = 0; i < count; i++) n += ops[i].type == 1 ? 5 : ops[i].type == 2 ? 6 : 2;
        return n;
    }
};

// Works out where everything goes; nullptr, or why it can't be done
inline const char* plan(const Snap& s, Plan& p) {
    const uint8_t* h = s.hdr;
    p.sp = (uint32_t) (h[8] | (h[9] << 8));
    if (p.sp == 0) p.sp = 0x10000;
    if (p.sp < 0x4000 + STUB_SIZE + 16) return "its stack is in the ROM";
    p.stub = (uint16_t) (p.sp - STUB_SIZE);
    uint32_t used_from = p.stub - 16;           // the stub and its stack: [used_from, sp)
    static const uint16_t PLACES[] = { 0xBC00, 0x9C00, 0x7C00, 0x6C00 };
    p.r = 0;
    for (uint16_t r : PLACES) {
        if (r + (uint32_t) LC_SIZE <= used_from || r >= p.sp) { p.r = r; break; }
    }
    if (p.r == 0) return "no room for the loader";
    p.paged = s.is128 ? (s.hdr[30] & 7) : 0;
    Mem m{ s };
    uint32_t r0 = p.r, r1 = p.r + LC_SIZE;
    for (uint16_t fill_min = FILL_MIN; ; fill_min = (uint16_t) (fill_min * 2)) {
        p.count = 0;
        p.full = false;
        auto add = [&](const Op& o) { p.add(o); };
        p.add(Op{ 3, 0, 0, 0x10, 0 });             // the 48K ROM (128 BASIC may have the other), bank 0
        auto span = [&](uint32_t a, uint32_t b, int bank) {   // a..b without R
            if (a < r0) split(m, a, b < r0 ? b : r0, bank, fill_min, add);
            if (b > r1) split(m, a > r1 ? a : r1, b, bank, fill_min, add);
        };
        span(0x4000, 0x5B00, 0);                    // the screen first
        if (!s.is128) {
            span(0x5B00, 0x10000, 0);
        } else {
            span(0x5B00, 0xC000, 0);
            static const int ORDER[6] = { 0, 1, 3, 4, 6, 7 };
            for (int k : ORDER) {
                p.add(Op{ 3, 0, 0, (uint8_t) (0x10 | k), (int8_t) k });
                span(0xC000, 0x10000, k);
            }
        }
        if (!p.full && LOADER_SIZE + p.table_bytes() + STACK_ROOM <= LC_SIZE) return nullptr;
        if (fill_min >= 8192) return "too many blocks";
    }
}

// Fills in the loader (LC_SIZE bytes at out) for the plan
inline void make_loader(const Snap& s, const Plan& p, uint8_t* out) {
    const uint8_t* h = s.hdr;
    memset(out, 0, LC_SIZE);
    memcpy(out, LOADER, LOADER_SIZE);
    auto put16 = [&](uint32_t at, uint32_t v) { out[at] = (uint8_t) v; out[at + 1] = (uint8_t) (v >> 8); };
    auto get16 = [&](uint32_t at) { return (uint32_t) (out[at] | (out[at + 1] << 8)); };
    for (uint16_t at : RELOC_LC) put16(at, get16(at) + p.r);
    for (uint16_t at : RELOC_STUB) put16(at, get16(at) + p.stub);
    // the table
    uint32_t t = AT_TABLE;
    for (int i = 0; i < p.count; i++) {
        const Op& o = p.ops[i];
        out[t++] = o.type;
        if (o.type == 3) {
            out[t++] = o.value;
            continue;
        }
        put16(t, o.addr);
        put16(t + 2, o.len);
        t += 4;
        if (o.type == 2) out[t++] = o.value;
    }
    out[t] = 0;
    // the sound chip and the paging
    if (s.is128) {
        out[AT_AYFLAG] = 1;
        out[AT_AYSEL] = h[32];
        memcpy(out + AT_AYREGS, h + 33, 16);
        out[AT_PAGE_STUB] = (uint8_t) ((h[30] & 0x0F) | 0x10);
    }
    put16(AT_STUB_AT, p.stub);
    // the registers, in the stub
    uint8_t* st = out + AT_STUB_IMAGE;
    st[S_PAGE] = s.is128 ? h[30] : 0x30;
    st[S_I] = h[10];
    uint8_t im = h[29] & 3;
    st[S_IM] = im == 0 ? 0x46 : im == 1 ? 0x56 : 0x5E;
    auto sput = [&](uint32_t at, uint8_t lo, uint8_t hi) { st[at] = lo; st[at + 1] = hi; };
    sput(S_AF2, h[22], h[21]);          // (pop af: F first)
    sput(S_BC2, h[15], h[16]);
    sput(S_DE2, h[17], h[18]);
    sput(S_HL2, h[19], h[20]);
    sput(S_IX, h[25], h[26]);
    sput(S_IY, h[23], h[24]);
    st[S_BORDER] = (h[12] >> 1) & 7;
    // R goes up by one for each instruction fetched after LD R,A (8 of them, to the game's)
    uint8_t r = (uint8_t) (((h[11] & 0x7F) - 8) & 0x7F) | (uint8_t) ((h[12] & 1) << 7);
    st[S_R] = r;
    sput(S_BC, h[2], h[3]);
    sput(S_DE, h[13], h[14]);
    sput(S_HL, h[4], h[5]);
    sput(S_AF, h[1], h[0]);
    sput(S_SP, h[8], h[9]);
    st[S_EI] = h[27] ? 0xFB : 0x00;
    sput(S_PC, h[6], h[7]);
}

// ---- the .tap ----

template <class Sink>
struct TapOut {
    Sink& sink;
    bool ok = true;
    uint8_t sum = 0;
    // a block: its length (with the flag and the checksum), the flag; then data(); then end()
    void begin(uint32_t len, uint8_t flag) {
        uint8_t b[3] = { (uint8_t) (len + 2), (uint8_t) ((len + 2) >> 8), flag };
        ok = ok && sink.write(b, 3);
        sum = flag;
    }
    void data(const uint8_t* p, uint32_t n) {
        for (uint32_t i = 0; i < n; i++) sum ^= p[i];
        ok = ok && sink.write(p, n);
    }
    void end() { ok = ok && sink.write(&sum, 1); }
    void block(uint8_t flag, const uint8_t* p, uint32_t n) { begin(n, flag); data(p, n); end(); }
    void header(uint8_t type, const char* name, uint16_t len, uint16_t p1, uint16_t p2) {
        uint8_t h[17];
        h[0] = type;
        size_t n = strlen(name);
        for (int i = 0; i < 10; i++) h[1 + i] = i < (int) n && (unsigned char) name[i] >= 32 && (unsigned char) name[i] < 128 ? (uint8_t) name[i] : ' ';
        h[11] = (uint8_t) len; h[12] = (uint8_t) (len >> 8);
        h[13] = (uint8_t) p1; h[14] = (uint8_t) (p1 >> 8);
        h[15] = (uint8_t) p2; h[16] = (uint8_t) (p2 >> 8);
        block(0x00, h, 17);
    }
};

// The whole tape. lc is LC_SIZE bytes of room. nullptr, or why not.
// p is big (keep it off the stack).
template <class Sink>
inline const char* write_tap(Sink& sink, const Snap& s, const char* name, uint8_t* lc, Plan& p) {
    const char* why = plan(s, p);
    if (why != nullptr) return why;
    make_loader(s, p, lc);
    TapOut<Sink> t{ sink };
    // 10 CLEAR VAL "r-1": LOAD ""CODE : RANDOMIZE USR VAL "r"
    char n1[8], n2[8];
    snprintf(n1, sizeof(n1), "%u", (unsigned) (p.r - 1));
    snprintf(n2, sizeof(n2), "%u", (unsigned) p.r);
    uint8_t prog[64];
    size_t k = 4;
    auto put = [&](uint8_t b) { prog[k++] = b; };
    auto str = [&](const char* q) { put('"'); while (*q) put((uint8_t) *q++); put('"'); };
    put(0xFD); put(0xB0); str(n1);                  // CLEAR VAL "..."
    put(':'); put(0xEF); put('"'); put('"'); put(0xAF);     // : LOAD ""CODE
    put(':'); put(0xF9); put(0xC0); put(0xB0); str(n2);     // : RANDOMIZE USR VAL "..."
    put(0x0D);
    prog[0] = 0; prog[1] = 10;                      // line 10
    prog[2] = (uint8_t) (k - 4); prog[3] = (uint8_t) ((k - 4) >> 8);
    t.header(0, name, (uint16_t) k, 10, (uint16_t) k);
    t.block(0xFF, prog, (uint32_t) k);
    t.header(3, name, LC_SIZE, p.r, 32768);
    t.block(0xFF, lc, LC_SIZE);
    Mem m{ s };
    for (int i = 0; i < p.count; i++) {
        const Op& o = p.ops[i];
        if (o.type == 1) t.block(0xFF, m.ptr(o.addr, o.bank), o.len);
    }
    t.block(0xFF, m.ptr(p.r, p.paged), LC_SIZE);   // R's own bytes, last (R is below 0xC000)
    return t.ok ? nullptr : "could not write the file";
}

}   // namespace z80tap
