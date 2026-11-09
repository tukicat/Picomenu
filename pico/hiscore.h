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


// High score tables: where a game keeps its table, found from something the player typed
// into it (a name or a score), and the files that keep it between games.
//
// A game's table is in "/High Scores/<game>.txt" (where: "address = n", "length = n", a
// text file that can be shared) and "/High Scores/<game>.bin" (the table itself, as it was
// last seen). PicoMenu keeps the .bin up to date and puts it back into the game when it is
// loaded (picoface.cxx). Nothing here touches the hardware, so it is tested on a PC.
//
// Finding it: the memory 0x4000 to 0xFFFF is searched for what was typed, as games keep it:
//   - a name or a score as characters, in any character set ("relative" search: the letters
//     or digits must differ from each other as they do in the alphabet, so A=0x41, A=0, A=10
//     or the game's own font all match), a last character with bit 7 set too;
//   - a score as packed BCD (12345 is 01 23 45) or as a number (16 or 24 bits, either order).
// Then the table around a find: the same kind of field again and again at a fixed distance
// (the size of an entry), taking in the bytes between them (the names, if the find is a score).

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace hiscore {

static const char FOLDER[] = "/High Scores";
static const int TYPED_MAX = 16;            // letters typed into the finder
static const int HITS_MAX = 9;              // finds shown (1 to 9 picks one)
static const int HITS_KEEP = 32;            // finds looked at (the most like a table are shown)
static const uint16_t TABLE_MAX = 512;      // bytes of a table
static const uint16_t MEM_START = 0x4000;
static const uint32_t MEM_SIZE = 49152;     // 0x4000 to 0xFFFF
static const int ENTRIES_MIN = 3;           // a table has at least this many entries found
static const int STRIDE_MAX = 64;           // bytes of an entry, at most

enum Enc : uint8_t {
    REL_LETTERS,        // a name, in some character set (base: the code of A)
    REL_DIGITS,         // a score as characters (base: the code of 0)
    TEXT,               // letters and digits together, as ASCII (either case)
    BCD,                // a score as packed BCD
    BIN,                // a score as a number
};

struct Hit {
    uint16_t addr;      // where it starts (0x4000 to 0xFFFF)
    uint8_t len;        // bytes
    uint8_t enc;
    uint8_t base;       // REL_*: the code of A or 0
};

// What was typed: a score (digits), a name (letters and spaces) or other text
enum Typed { T_NONE, T_SCORE, T_NAME, T_TEXT };
inline Typed classify(const char* s) {
    bool digit = false, letter = false, other = false;
    for (; *s != '\0'; s++) {
        char c = *s;
        if (c >= '0' && c <= '9') digit = true;
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) letter = true;
        else if (c != ' ') other = true;
    }
    if (!digit && !letter) return T_NONE;
    if (digit && !letter && !other) return T_SCORE;
    if (letter && !digit && !other) return T_NAME;
    return T_TEXT;
}

// Each letter (0 A .. 25 Z) or digit (0 .. 9), -1 for a space (anything matches it)
inline int index_of(char c, bool letters) {
    if (letters) {
        if (c >= 'a' && c <= 'z') return c - 'a';
        if (c >= 'A' && c <= 'Z') return c - 'A';
        return -1;
    }
    return c >= '0' && c <= '9' ? c - '0' : -1;
}

class Finder {
public:
    Hit hits[HITS_KEEP];
    int count = 0;          // finds shown (at most HITS_MAX; the first of hits)
    int total = 0;          // finds in all
    int kept = 0;

    // mem: MEM_SIZE bytes, mem[0] at 0x4000
    void find(const uint8_t* mem, const char* typed) {
        count = total = kept = 0;
        char t[TYPED_MAX + 1];
        // spaces at the ends are not part of it
        while (*typed == ' ') typed++;
        size_t n = strlen(typed);
        while (n > 0 && typed[n - 1] == ' ') n--;
        if (n > TYPED_MAX) n = TYPED_MAX;
        memcpy(t, typed, n);
        t[n] = '\0';
        Typed kind = classify(t);
        if (n < 2 || kind == T_NONE) return;
        if (kind == T_NAME || kind == T_SCORE) {
            relative(mem, t, kind == T_NAME);
        } else {
            text(mem, t);
        }
        if (kind == T_SCORE) {
            bcd(mem, t);
            number(mem, t);
        }
        rank(mem);
        count = kept < HITS_MAX ? kept : HITS_MAX;
    }

private:
    // A character set games use (the ASCII codes, or a font that starts at 0, 1, 10 ...)
    static bool usual(uint8_t enc, uint8_t base) {
        if (enc == REL_DIGITS) return base == '0' || base == 0 || base == 1 || base == 0x10;
        if (enc == REL_LETTERS) return base == 'A' || base == 'a' || base == 0 || base == 1 || base == 10 || base == 0x11;
        return true;
    }

    void add(uint32_t at, size_t len, uint8_t enc, uint8_t base) {
        const uint16_t a = (uint16_t) (MEM_START + at);
        for (int i = 0; i < kept; i++) {
            Hit& o = hits[i];
            if (o.addr == a) return;                        // (found already another way)
            if (o.enc == enc && a < o.addr + o.len && o.addr < a + len) {
                // overlapping the same kind of find ("12345" in 012345 is also 01234 counted
                // one up): the one in a usual character set is kept
                if (usual(enc, base) && !usual(o.enc, o.base)) o = { a, (uint8_t) len, enc, base };
                return;
            }
        }
        total++;
        if (kept < HITS_KEEP) {
            hits[kept++] = { a, (uint8_t) len, enc, base };
        } else if (usual(enc, base)) {
            // full: a find in a usual character set takes the place of one that isn't
            for (int i = 0; i < kept; i++) {
                if (!usual(hits[i].enc, hits[i].base)) {
                    hits[i] = { a, (uint8_t) len, enc, base };
                    return;
                }
            }
        }
    }

    // The finds most like a table first (the most entries found around them), then those in a
    // usual character set, then as they were found
    void rank(const uint8_t* mem);

    // The letters (or digits) as differences from the first: every byte must be its letter
    // plus the same base. At least two different letters (or "AAAA" is any run of bytes).
    void relative(const uint8_t* mem, const char* t, bool letters) {
        int idx[TYPED_MAX];
        size_t n = strlen(t);
        int first = -1;
        bool differ = false;
        for (size_t k = 0; k < n; k++) {
            idx[k] = index_of(t[k], letters);
            if (idx[k] >= 0) {
                if (first < 0) first = (int) k;
                else if (idx[k] != idx[first]) differ = true;
            }
        }
        if (first < 0 || !differ) return;
        const int span = letters ? 25 : 9;
        for (uint32_t i = 0; i + n <= MEM_SIZE; i++) {
            uint8_t base = (uint8_t) (mem[i + first] - idx[first]);
            if (base > 255 - span) continue;            // (the alphabet would go past 255)
            bool ok = true;
            for (size_t k = first + 1; k < n && ok; k++) {
                if (idx[k] < 0) continue;
                uint8_t b = mem[i + k];
                if ((uint8_t) (b - base) != idx[k]) {
                    // the last letter may have bit 7 set (as the Spectrum's ROM ends a word)
                    ok = k == n - 1 && (b & 0x80) != 0 && (uint8_t) ((b & 0x7F) - base) == idx[k];
                }
            }
            if (ok) add(i, n, letters ? REL_LETTERS : REL_DIGITS, base);
        }
    }

    void text(const uint8_t* mem, const char* t) {
        size_t n = strlen(t);
        for (uint32_t i = 0; i + n <= MEM_SIZE; i++) {
            size_t k = 0;
            for (; k < n; k++) {
                uint8_t b = mem[i + k];
                if (k == n - 1) b &= 0x7F;
                char c = t[k];
                if (c >= 'a' && c <= 'z') c = (char) (c - 32);
                if (b >= 'a' && b <= 'z') b = (uint8_t) (b - 32);
                if (b != (uint8_t) c) break;
            }
            if (k == n) add(i, n, TEXT, 0);
        }
    }

    void exact(const uint8_t* mem, const uint8_t* p, size_t n, uint8_t enc) {
        for (uint32_t i = 0; i + n <= MEM_SIZE; i++) {
            if (memcmp(mem + i, p, n) == 0) add(i, n, enc, 0);
        }
    }

    // 12345 as 01 23 45
    void bcd(const uint8_t* mem, const char* t) {
        size_t n = strlen(t);
        uint8_t p[TYPED_MAX / 2 + 1];
        size_t bytes = (n + 1) / 2;
        size_t d = 0;
        for (size_t b = 0; b < bytes; b++) {
            uint8_t hi = (n % 2 == 1 && b == 0) ? 0 : (uint8_t) (t[d++] - '0');
            uint8_t lo = (uint8_t) (t[d++] - '0');
            p[b] = (uint8_t) (hi << 4 | lo);
        }
        if (bytes >= 2) exact(mem, p, bytes, BCD);
    }

    // As a number: 16 bits (1000 or more) or 24 bits, low byte first or last
    void number(const uint8_t* mem, const char* t) {
        unsigned long v = strtoul(t, nullptr, 10);
        if (strlen(t) > 8 || v < 1000 || v > 0xFFFFFF) return;
        uint8_t p[3];
        size_t n = v > 0xFFFF ? 3 : 2;
        for (size_t k = 0; k < n; k++) p[k] = (uint8_t) (v >> (8 * k));
        exact(mem, p, n, BIN);
        uint8_t q[3];
        for (size_t k = 0; k < n; k++) q[k] = p[n - 1 - k];
        if (memcmp(p, q, n) != 0) exact(mem, q, n, BIN);
    }
};

// The table around a find
struct Table {
    uint16_t addr = 0, len = 0;
    uint8_t stride = 0;         // 0: only the find (no entries like it around it)
    uint8_t entries = 0;
    uint16_t first = 0;         // the find's field in the first entry
};

// Is the field at mem[i] (the find's length) one like the find? (an entry's name or score)
inline bool field_like(const uint8_t* mem, uint32_t i, const Hit& h) {
    if (i + h.len > MEM_SIZE) return false;
    const uint8_t* f = mem + i;
    switch (h.enc) {
    case REL_DIGITS: {
        // digits at the end (2 at least), the rest digits or all one filler (a space, a 0 shown blank)
        int digits = 0;
        int k = h.len - 1;
        for (; k >= 0 && (uint8_t) (f[k] - h.base) <= 9; k--) digits++;
        if (digits < 2) return false;
        for (int j = 0; j < k; j++) {
            if (f[j] != f[k]) return false;
        }
        return true;
    }
    case REL_LETTERS: {
        // starts with a letter, and at least half of it letters (a short name has filler after)
        if ((uint8_t) (f[0] - h.base) > 25 && (uint8_t) ((f[0] & 0x7F) - h.base) > 25) return false;
        int letters = 0;
        for (int k = 0; k < h.len; k++) {
            if ((uint8_t) ((f[k] & 0x7F) - h.base) <= 25) letters++;
        }
        return letters * 2 >= h.len;
    }
    case TEXT: {
        int ok = 0;
        for (int k = 0; k < h.len; k++) {
            uint8_t b = f[k] & 0x7F;
            if (b >= 32 && b < 127) ok++;
        }
        return ok == h.len;
    }
    case BCD:
        for (int k = 0; k < h.len; k++) {
            if ((f[k] >> 4) > 9 || (f[k] & 15) > 9) return false;
        }
        return true;
    default:
        return false;       // a number could be anything: no table is found from it
    }
}

// The class of a byte as the find sees it: 1 its kind of character, 2 another, for comparing
// entries byte by byte
inline uint8_t byte_class(uint8_t b, const Hit& h) {
    switch (h.enc) {
    case REL_DIGITS: return (uint8_t) (b - h.base) <= 9 ? 1 : 2;
    case REL_LETTERS: return (uint8_t) ((b & 0x7F) - h.base) <= 25 ? 1 : 2;
    case BCD: return (b >> 4) <= 9 && (b & 15) <= 9 ? 1 : 2;
    default: return (b & 0x7F) >= 32 && (b & 0x7F) < 127 ? 1 : 2;
    }
}

// Is the run of bytes like the rest of the entries (what is between their fields)? Those are
// mostly one or two values used again and again (a space, a leading 0) and characters in a
// range of at most 45 codes (letters, or digits).
struct GapLook {
    uint8_t f1 = 0, f2 = 0, lo = 255, hi = 0;
    bool ok = false;
    void learn(const uint8_t* mem, int32_t first, int32_t last, int s, int len) {
        uint16_t counts[256];
        memset(counts, 0, sizeof(counts));
        for (int32_t e = first; e < last; e += s) {
            for (int32_t i = e + len; i < e + s; i++) counts[mem[i]]++;
        }
        int a = 0, b = -1;
        for (int v = 1; v < 256; v++) if (counts[v] > counts[a]) a = v;
        for (int v = 0; v < 256; v++) if (v != a && counts[v] != 0 && (b < 0 || counts[v] > counts[b])) b = v;
        f1 = (uint8_t) a;
        f2 = (uint8_t) (b < 0 ? a : b);
        for (int v = 0; v < 256; v++) {
            if (counts[v] != 0 && v != a && v != b) {
                if (v < lo) lo = (uint8_t) v;
                if (v > hi) hi = (uint8_t) v;
            }
        }
        ok = lo > hi || hi - lo <= 45;
    }
    bool like(const uint8_t* mem, int32_t from, int32_t to) const {
        if (!ok || from < 0 || to > (int32_t) MEM_SIZE || from >= to) return false;
        int n = 0;
        for (int32_t i = from; i < to; i++) {
            if (mem[i] == f1 || mem[i] == f2 || (mem[i] >= lo && mem[i] <= hi)) n++;
        }
        return n * 4 >= (to - from) * 3;
    }
};

// The other half of an entry: a name's score (digits) or a score's name (letters). A real
// table has it in every entry at the same place; menu text or other words have no scores
inline bool partner_byte(uint8_t b, const Hit& h) {
    b &= 0x7F;
    if (h.enc == REL_DIGITS || h.enc == BCD) {
        if (b >= 'A' && b <= 'Z') return true;
        if (h.enc == REL_DIGITS) {
            uint8_t l1 = (uint8_t) (h.base + 17), l2 = (uint8_t) (h.base + 10);
            if ((uint8_t) (b - l1) <= 25 || (uint8_t) (b - l2) <= 25) return true;
        }
        return false;
    }
    if (b >= '0' && b <= '9') return true;
    if (h.enc == REL_LETTERS && h.base != 'A') {
        uint8_t d1 = (uint8_t) (h.base - 17), d2 = (uint8_t) (h.base - 10);
        if ((uint8_t) (b - d1) <= 9 || (uint8_t) (b - d2) <= 9) return true;
    }
    return false;
}

// A byte of a name like the find (a letter in its letters, or printable text)
inline bool field_like_byte(uint8_t b, const Hit& h) {
    if (h.enc == REL_LETTERS) return (uint8_t) ((b & 0x7F) - h.base) <= 25;
    return (b & 0x7F) >= 32 && (b & 0x7F) < 127;
}

struct Partner {
    int lo = 0, hi = 0;         // its bytes, from the field's first (lo to hi, hi not in it)
    int back = 0, fwd = 0;      // the entries it is in, around the find
    bool found = false;
};

// Look for the partner nearest the field, before it or after it, in the entries at stride s
// (back and fwd of them around the find): the columns where all of them have it
inline Partner partner_for(const uint8_t* mem, uint32_t at, int s, int back, int fwd, const Hit& h) {
    if (h.enc == BIN || s <= h.len) return Partner();
    const int need = h.enc == REL_DIGITS || h.enc == BCD ? 2 : 3;
    auto col = [&](int o, int& b, int& f) {
        // the entries in a row around the find with the partner at offset o
        auto has = [&](int e) {
            int64_t i = (int64_t) at + (int64_t) e * s + o;
            return i >= 0 && i < (int64_t) MEM_SIZE && partner_byte(mem[i], h);
        };
        b = 0; f = 0;
        if (!has(0)) return false;
        while (b < back && has(-(b + 1))) b++;
        while (f < fwd && has(f + 1)) f++;
        return 1 + b + f >= ENTRIES_MIN;
    };
    // the nearest on each side, after the field and before it: the one in more entries, or
    // the usual side (a name before its score) when it is in as many
    Partner sides[2];
    for (int side = 0; side < 2; side++) {
        Partner& best = sides[side];
        const int span = s - h.len;
        for (int k = 0; k < span; k++) {
            int o = side == 0 ? h.len + k : -1 - k;
            int b, f;
            if (!col(o, b, f)) continue;
            // the run of columns from there, away from the field, in the same entries
            int lo = o, hi = o + 1, n = 1;
            for (int j = k + 1; j < span; j++) {
                int o2 = side == 0 ? h.len + j : -1 - j;
                int b2, f2;
                if (!col(o2, b2, f2) || b2 < b || f2 < f) break;
                if (side == 0) hi = o2 + 1; else lo = o2;
                n++;
            }
            if (n < need) continue;
            if (side == 1) {
                // a name: take in the rest of it, the letters and stops before (an initial
                // and a dot), in the same entries
                const bool score = h.enc == REL_DIGITS || h.enc == BCD;
                for (int j = n; score && lo > -(s - h.len); j++) {
                    bool all = true;
                    for (int e = -b; e <= f && all; e++) {
                        int64_t i = (int64_t) at + (int64_t) e * s + lo - 1;
                        uint8_t c = i >= 0 ? mem[i] & 0x7F : 0;
                        all = partner_byte(c, h) || c == '.' || c == '@' || c == ' ' || c == '-';
                    }
                    if (!all) break;
                    lo--;
                }
            }
            best.found = true;
            best.lo = lo;
            best.hi = hi;
            best.back = b;
            best.fwd = f;
            break;
        }
    }
    const bool score = h.enc == REL_DIGITS || h.enc == BCD;
    const Partner& a = sides[0];
    const Partner& bf = sides[1];
    if (!a.found) return bf;
    if (!bf.found) return a;
    int na = a.back + a.fwd, nb = bf.back + bf.fwd;
    if (na != nb) return na > nb ? a : bf;
    return score ? bf : a;
}

inline Table table_for(const uint8_t* mem, const Hit& h) {
    Table best;
    const uint32_t at = (uint32_t) (h.addr - MEM_START);
    best.addr = h.addr;
    best.len = h.len;
    best.first = h.addr;
    best.entries = 1;
    int best_back = 0, best_fwd = 0, best_score = -1;
    Partner best_pt;
    for (int s = h.len; s <= STRIDE_MAX; s++) {
        int back = 0, fwd = 0;
        while (back < 30 && (int64_t) at - (int64_t) (back + 1) * s >= 0 && field_like(mem, at - (back + 1) * s, h)) back++;
        while (fwd < 30 && field_like(mem, at + (fwd + 1) * s, h)) fwd++;
        int n = 1 + back + fwd;
        if (n < ENTRIES_MIN) continue;
        // the entries the same byte by byte (each byte's class in all of them) around the field:
        // the right size of an entry lines them all up, one a byte out does not
        int same = 0;
        const int32_t first = (int32_t) at - back * s;
        for (int d = -4; d < h.len + 4; d++) {
            int32_t p0 = first + d;
            if (p0 < 0 || p0 + (n - 1) * s >= (int32_t) MEM_SIZE) continue;
            uint8_t c0 = byte_class(mem[p0], h);
            bool all = true;
            for (int e = 1; e < n && all; e++) all = byte_class(mem[p0 + e * s], h) == c0;
            if (all) same++;
        }
        // entries with the other half (a name's score, a score's name) win over more of them
        // without it: menu words line up like names but have no scores
        Partner pt = partner_for(mem, at, s, back, fwd, h);
        if (pt.found) {
            back = pt.back;
            fwd = pt.fwd;
            n = 1 + back + fwd;
        }
        int score = (pt.found ? 10000 : 0) + n * 100 + same;
        if (score > best_score) {
            best_score = score;
            best.entries = (uint8_t) n;
            best.stride = (uint8_t) s;
            best_back = back;
            best_fwd = fwd;
            best_pt = pt;
        }
    }
    if (best.stride == 0) {
        best.entries = 1;
        return best;
    }
    const int s = best.stride;
    const int32_t first = (int32_t) at - best_back * s;
    const int32_t last = (int32_t) at + best_fwd * s;
    int32_t start = first, end = last + h.len;
    if (best_pt.found) {
        // from the field to its partner in every entry; a name before its score is taken to
        // the score, or after it to the end of its letters (a short name, filler after)
        if (best_pt.lo < 0) {
            start = first + best_pt.lo;
            if (h.enc == REL_LETTERS || h.enc == TEXT) {
                const uint8_t fill = mem[last + h.len < (int32_t) MEM_SIZE ? last + h.len : last];
                while (end < last + s + best_pt.lo && end < (int32_t) MEM_SIZE
                        && (field_like_byte(mem[end], h) || mem[end] == fill)) end++;
            }
        } else {
            end = last + best_pt.hi;
            if (h.enc == REL_DIGITS || h.enc == BCD) {
                const uint8_t fill = end < (int32_t) MEM_SIZE ? mem[end] : 0;
                while (end < last + s && end < (int32_t) MEM_SIZE
                        && (partner_byte(mem[end], h) || mem[end] == fill)) end++;
            }
        }
    } else if (s > h.len) {
        // the bytes between the fields are the rest of each entry, before the field or after
        // it: what is before the first field and after the last is taken in if it is like them
        GapLook g;
        g.learn(mem, first, last, s, h.len);
        bool front = g.like(mem, first - (s - h.len), first);
        bool back = g.like(mem, last + h.len, last + s);
        if (front) start = first - (s - h.len);
        if (back) end = last + s;
    }
    if (end - start > TABLE_MAX) {
        best.stride = 0;
        best.entries = 1;
        return best;
    }
    best.addr = (uint16_t) (MEM_START + start);
    best.len = (uint16_t) (end - start);
    best.first = (uint16_t) (MEM_START + first);
    return best;
}

inline void Finder::rank(const uint8_t* mem) {
    uint8_t entries[HITS_KEEP];
    for (int i = 0; i < kept; i++) entries[i] = table_for(mem, hits[i]).entries;
    auto better = [](const Hit& a, uint8_t ea, const Hit& b, uint8_t eb) {
        if (ea != eb) return ea > eb;
        return usual(a.enc, a.base) && !usual(b.enc, b.base);
    };
    for (int i = 1; i < kept; i++) {
        Hit h = hits[i];
        uint8_t e = entries[i];
        int j = i;
        while (j > 0 && better(h, e, hits[j - 1], entries[j - 1])) {
            hits[j] = hits[j - 1];
            entries[j] = entries[j - 1];
            j--;
        }
        hits[j] = h;
        entries[j] = e;
    }
}

// A byte shown as a letter: by the find's character set, else as ASCII, else '.'
inline char show(uint8_t b, const Hit& h) {
    if (h.enc == REL_DIGITS && (uint8_t) (b - h.base) <= 9) return (char) ('0' + (b - h.base));
    if (h.enc == REL_LETTERS && (uint8_t) ((b & 0x7F) - h.base) <= 25) return (char) ('A' + ((b & 0x7F) - h.base));
    if (h.enc == REL_DIGITS && h.base != '0' && h.base <= 255 - 35) {
        // a game's own characters: the letters often come after the digits
        uint8_t v = (uint8_t) ((b & 0x7F) - (h.base + 10));
        if (v <= 25) return (char) ('A' + v);
    }
    if (h.enc == REL_LETTERS && h.base != 'A' && h.base >= 10) {
        uint8_t v = (uint8_t) (b - (h.base - 10));
        if (v <= 9) return (char) ('0' + v);
    }
    uint8_t a = b & 0x7F;
    if (a >= 32 && a < 127) return (char) a;
    return '.';
}

// The bytes from p as text for the screen (n of them, at most outsize - 1)
inline void show_bytes(const uint8_t* p, int n, const Hit& h, char* out, size_t outsize) {
    int k = 0;
    for (; k < n && k + 1 < (int) outsize; k++) {
        char c = show(p[k], h);
        if (h.enc == BCD || h.enc == BIN) {
            // (no characters to go by: hex)
            static const char HEX[] = "0123456789ABCDEF";
            if (2 * k + 2 >= (int) outsize) break;
            out[2 * k] = HEX[p[k] >> 4];
            out[2 * k + 1] = HEX[p[k] & 15];
            continue;
        }
        out[k] = c;
    }
    if (h.enc == BCD || h.enc == BIN) k *= 2;
    out[k] = '\0';
}

// Does the memory where the table should be look like this game's table (the one saved)? Before
// a saved table is put into the Spectrum, as the game in it may not be that game (a tape played
// from the Virtual Tape Player or the Bluetooth, which the Spectrum's own ROM loads), or not yet
// set up. A table of the same game, its own one too, has the same layout: the same fillers and
// fixed characters, many bytes the same, and all of it in the same character ranges (the saved
// table's values, close ones together). Other code or data almost never does.
// Fed in pieces: learn() with all of the saved table, then compare() with both.
struct Match {
    uint8_t seen[32] = {};      // the values in the saved table (a bit each)
    uint8_t in[32] = {};        // the ranges they make (values at most RANGE_GAP apart joined)
    uint32_t n = 0, equal = 0, inrange = 0;
    static const int RANGE_GAP = 6;
    void learn(const uint8_t* saved, size_t k) {
        for (size_t i = 0; i < k; i++) seen[saved[i] >> 3] |= (uint8_t) (1 << (saved[i] & 7));
    }
    void ranges() {
        int last = -100;
        for (int v = 0; v < 256; v++) {
            if (seen[v >> 3] & (1 << (v & 7))) {
                if (v - last <= RANGE_GAP) {
                    for (int u = last + 1; u < v; u++) in[u >> 3] |= (uint8_t) (1 << (u & 7));
                }
                in[v >> 3] |= (uint8_t) (1 << (v & 7));
                last = v;
            }
        }
    }
    uint8_t ramseen[32] = {};   // the values in the memory (cleared memory is not a table)
    void compare(const uint8_t* ram, const uint8_t* saved, size_t k) {
        for (size_t i = 0; i < k; i++) {
            ramseen[ram[i] >> 3] |= (uint8_t) (1 << (ram[i] & 7));
            n++;
            if (ram[i] == saved[i]) equal++;
            if (in[ram[i] >> 3] & (1 << (ram[i] & 7))) inrange++;
        }
    }
    static int count(const uint8_t* bits) {
        int c = 0;
        for (int i = 0; i < 32; i++) for (int b = 0; b < 8; b++) c += (bits[i] >> b) & 1;
        return c;
    }
    bool ok() const {
        if (n == 0) return false;
        // as many different values as a table has, near enough (a quarter, 3 at least)
        int want = count(seen) / 4;
        if (want < 3) want = 3;
        if (count(ramseen) < want) return false;
        if (equal * 2 >= n) return true;
        return inrange * 10 >= n * 9 && equal * 100 >= n * 15;
    }
};

inline bool table_matches(const uint8_t* ram, const uint8_t* saved, size_t len) {
    Match m;
    m.learn(saved, len);
    m.ranges();
    m.compare(ram, saved, len);
    return m.ok();
}

// The .txt that says where the table is (address and length, decimal)
inline int write_def(char* out, size_t outsize, uint16_t addr, uint16_t len) {
    return snprintf(out, outsize,
        "; PicoMenu: where the game keeps its high score table\r\n"
        "; (the table itself is in the .bin)\r\n"
        "address = %u\r\n"
        "length = %u\r\n", (unsigned) addr, (unsigned) len);
}

// Reads one: false if it has no address and length that can be a table
inline bool read_def(const char* text, size_t n, uint16_t& addr, uint16_t& len) {
    long a = -1, l = -1;
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && text[e] != '\n' && text[e] != '\r') e++;
        const char* line = text + i;
        size_t ll = e - i;
        while (ll > 0 && *line == ' ') { line++; ll--; }
        if (ll > 0 && *line != ';' && *line != '#') {
            const char* eq = (const char*) memchr(line, '=', ll);
            if (eq != nullptr) {
                size_t kl = (size_t) (eq - line);
                while (kl > 0 && line[kl - 1] == ' ') kl--;
                char v[16];
                size_t vl = ll - (size_t) (eq + 1 - line);
                if (vl >= sizeof(v)) vl = sizeof(v) - 1;
                memcpy(v, eq + 1, vl);
                v[vl] = '\0';
                long x = strtol(v, nullptr, 0);
                if (kl == 7 && strncasecmp(line, "address", 7) == 0) a = x;
                if (kl == 6 && strncasecmp(line, "length", 6) == 0) l = x;
            }
        }
        i = e + 1;
    }
    if (a < MEM_START || a > 0xFFFF || l < 1 || l > TABLE_MAX || a + l > 0x10000) return false;
    addr = (uint16_t) a;
    len = (uint16_t) l;
    return true;
}


// ---- The table on the screen (put back with the table) ----
// A game that draws its table once (a snapshot showing it, or a game that shows it as it
// finishes loading) still shows the old one when the saved table is put in its memory. So the
// screen is looked at for the old table's text, drawn with a font: the game's own (somewhere in
// its memory, found from the screen) or the Spectrum's (the ROM's, from 48.rom). Where a run of
// the text is drawn in that font, character by character, the characters that change are drawn
// again with the new ones. Nothing else on the screen is touched, and if the text isn't found
// clearly nothing is.
static const int RD_RUNS = 32;          // runs of the table's text found on the screen
static const int RD_CHARS = 40;         // the characters of the old table (and their glyphs)

struct Redraw {
    struct Run { uint8_t y, c, n; uint16_t k; };    // pixel row (0-184), column, length, table offset
    Run run[RD_RUNS];
    uint8_t runs = 0;
    uint8_t chars = 0;
    uint8_t ch[RD_CHARS];               // the old table's characters
    uint8_t glyph[RD_CHARS][8];         // each one's glyph (what it is on the screen)
    uint8_t known[RD_CHARS];            // 1 from the font (and seen on the screen), 2 learned from the screen
    int32_t font = -1;                  // the game's font: the Spectrum address of character 0's glyph
    bool rom = false;                   // the ROM's font (48.rom) instead
    uint16_t addr = 0, len = 0;
};

inline uint16_t screen_at(int y, int c) {
    return (uint16_t) (((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2) | c);
}
inline void screen_cell(const uint8_t* scr, int y, int c, uint8_t* out) {
    for (int i = 0; i < 8; i++) out[i] = scr[screen_at(y + i, c)];
}
inline bool glyph_like(const uint8_t* g) {
    // a character's glyph: not blank, not all one pattern (a fill)
    for (int i = 1; i < 8; i++) if (g[i] != g[0]) return true;
    return false;
}
// The screen's glyph-like cells (each 8 pixel rows of a column, at any row), for looking up a
// glyph at once: an open-addressed table of their positions (y * 32 + c), in scratch memory
struct ScreenIndex {
    uint16_t* slot = nullptr;
    uint32_t mask = 0;
    const uint8_t* scr = nullptr;
    static uint32_t hash(const uint8_t* g) {
        uint32_t h = 2166136261u;
        for (int i = 0; i < 8; i++) h = (h ^ g[i]) * 16777619u;
        return h ^ (h >> 15);
    }
    bool same(uint16_t pos, const uint8_t* g) const {
        int y = pos >> 5, c = pos & 31;
        for (int i = 0; i < 8; i++) if (scr[screen_at(y + i, c)] != g[i]) return false;
        return true;
    }
    // slots: a power of two, more than the cells (5920)
    void build(const uint8_t* s, uint16_t* mem, uint32_t slots) {
        scr = s;
        slot = mem;
        mask = slots - 1;
        for (uint32_t i = 0; i < slots; i++) slot[i] = 0xFFFF;
        for (int y = 0; y <= 184; y++) {
            for (int c = 0; c < 32; c++) {
                uint8_t g[8];
                screen_cell(scr, y, c, g);
                if (!glyph_like(g)) continue;
                uint32_t h = hash(g) & mask;
                while (slot[h] != 0xFFFF && !same(slot[h], g)) h = (h + 1) & mask;
                if (slot[h] == 0xFFFF) slot[h] = (uint16_t) (y * 32 + c);
            }
        }
    }
    bool has(const uint8_t* g) const {
        uint32_t h = hash(g) & mask;
        while (slot[h] != 0xFFFF) {
            if (same(slot[h], g)) return true;
            h = (h + 1) & mask;
        }
        return false;
    }
};
static const uint32_t RD_INDEX_SLOTS = 8192;
static const uint32_t RD_VOTE_SLOTS = 2048;
// The scratch memory redraw_find needs
static const uint32_t RD_SCRATCH = RD_INDEX_SLOTS * 2 + RD_VOTE_SLOTS * 6;

// Finds where the old table (mem: the Spectrum's memory, 0x4000-0xFFFF, as mem(address); scr:
// the screen shown, 6912 bytes) is drawn. rom(ch, out): the ROM's glyph for ch (false: none).
// scratch: RD_SCRATCH bytes, 2-byte aligned. False: not found clearly, nothing to redraw.
template <typename Mem, typename Rom>
bool redraw_find(Redraw& w, Mem mem, const uint8_t* scr, uint16_t addr, uint16_t len, Rom rom, uint8_t* scratch) {
    w = Redraw();
    w.addr = addr;
    w.len = len;
    if (len == 0 || len > TABLE_MAX) return false;
    for (uint16_t k = 0; k < len; k++) {
        uint8_t b = mem((uint16_t) (addr + k));
        int i = 0;
        while (i < w.chars && w.ch[i] != b) i++;
        if (i == w.chars) {
            if (w.chars == RD_CHARS) return false;
            w.ch[w.chars++] = b;
        }
    }
    ScreenIndex six;
    six.build(scr, reinterpret_cast<uint16_t*>(scratch), RD_INDEX_SLOTS);
    // how many of the characters a font has drawn on the screen
    auto score = [&](auto glyph_of) {
        int n = 0;
        for (int i = 0; i < w.chars; i++) {
            uint8_t g[8];
            if (glyph_of(w.ch[i], g) && glyph_like(g) && six.has(g)) n++;
        }
        return n;
    };
    // 1. the game's font: each place in memory that holds a glyph on the screen is a vote for
    // fonts that would put each of the table's characters there
    uint32_t* vkey = reinterpret_cast<uint32_t*>(scratch + RD_INDEX_SLOTS * 2);
    uint16_t* vn = reinterpret_cast<uint16_t*>(scratch + RD_INDEX_SLOTS * 2 + RD_VOTE_SLOTS * 4);
    for (uint32_t i = 0; i < RD_VOTE_SLOTS; i++) vn[i] = 0;
    for (uint32_t p = 0x5B00; p + 8 <= 0x10000; p++) {
        uint8_t g[8];
        for (int r = 0; r < 8; r++) g[r] = mem((uint16_t) (p + r));
        if (!glyph_like(g) || !six.has(g)) continue;
        for (int i = 0; i < w.chars; i++) {
            uint32_t f = p - 8u * w.ch[i] + 0x1000;         // (kept positive)
            uint32_t h = (f * 2654435761u >> 21) & (RD_VOTE_SLOTS - 1);
            for (uint32_t t = 0; t < 16; t++, h = (h + 1) & (RD_VOTE_SLOTS - 1)) {
                if (vn[h] == 0) { vkey[h] = f; vn[h] = 1; break; }
                if (vkey[h] == f) { if (vn[h] < 0xFFFF) vn[h]++; break; }
            }
        }
    }
    int best = 0;
    int32_t best_f = -1;
    for (int pick = 0; pick < 6; pick++) {
        // the fonts with the most votes, checked
        uint32_t top = 0;
        for (uint32_t h = 1; h < RD_VOTE_SLOTS; h++) if (vn[h] > vn[top]) top = h;
        if (vn[top] < 4) break;
        vn[top] = 0;
        int32_t f = (int32_t) vkey[top] - 0x1000;
        int n = score([&](uint8_t c, uint8_t* g) {
            int32_t a = f + 8 * c;
            if (a < 0x4000 || a + 8 > 0x10000) return false;
            for (int r = 0; r < 8; r++) g[r] = mem((uint16_t) (a + r));
            return true;
        });
        if (n > best) { best = n; best_f = f; }
    }
    // 2. the ROM's font
    int rom_n = score([&](uint8_t c, uint8_t* g) { return rom(c, g); });
    int need = w.chars / 2;
    if (need < 4) need = 4;
    if (best < need && rom_n < need) return false;
    if (rom_n > best) w.rom = true;
    else w.font = best_f;
    for (int i = 0; i < w.chars; i++) {
        uint8_t* g = w.glyph[i];
        bool have;
        if (w.rom) {
            have = rom(w.ch[i], g);
        } else {
            int32_t a = w.font + 8 * w.ch[i];
            have = a >= 0x4000 && a + 8 <= 0x10000;
            for (int r = 0; have && r < 8; r++) g[r] = mem((uint16_t) (a + r));
        }
        // known: drawn on the screen in that font (a blank one is a space)
        w.known[i] = have && (!glyph_like(g) ? (g[0] == 0) : six.has(g)) ? 1 : 0;
    }
    auto index = [&](uint8_t b) { int i = 0; while (w.ch[i] != b) i++; return i; };
    // 3. the runs: the table's text drawn left to right, a character a cell, at any pixel row;
    // characters not known match anything (learned after). The ones with the most known
    // characters are kept, none sharing a character of the table or the screen.
    struct Cand { uint8_t known, y, c, n; uint16_t k; };
    static const int CANDS = 64;
    Cand cand[CANDS];
    int cands = 0;
    for (int y = 0; y <= 184; y++) {
        uint8_t id[32];             // the known character drawn there (its index), else 0xFF
        for (int c = 0; c < 32; c++) {
            uint8_t g[8];
            screen_cell(scr, y, c, g);
            id[c] = 0xFF;
            for (int i = 0; i < w.chars; i++) {
                if (w.known[i] == 1 && memcmp(w.glyph[i], g, 8) == 0) { id[c] = (uint8_t) i; break; }
            }
        }
        for (int c = 0; c < 32; c++) {
            if (id[c] == 0xFF || !glyph_like(w.glyph[id[c]])) continue;
            for (uint16_t k = 0; k < len; k++) {
                if (mem((uint16_t) (addr + k)) != w.ch[id[c]]) continue;
                if (k > 0 && c > 0) {
                    // (only from where the run starts)
                    int pi = index(mem((uint16_t) (addr + k - 1)));
                    if (w.known[pi] == 1 && id[c - 1] == pi) continue;
                }
                int n = 0, kn = 0, last = 0;
                while (c + n < 32 && k + n < len) {
                    int i = index(mem((uint16_t) (addr + k + n)));
                    if (w.known[i] == 1) {
                        if (id[c + n] != i) break;
                        if (glyph_like(w.glyph[i])) kn++;
                        last = n + 1;
                    }
                    n++;
                }
                n = last;
                if (kn < 3 || n < 4) continue;
                Cand x = { (uint8_t) kn, (uint8_t) y, (uint8_t) c, (uint8_t) n, k };
                if (cands < CANDS) cand[cands++] = x;
                else {
                    int lo = 0;
                    for (int j = 1; j < CANDS; j++) if (cand[j].known < cand[lo].known) lo = j;
                    if (cand[lo].known < x.known) cand[lo] = x;
                }
            }
        }
    }
    for (int i = 1; i < cands; i++) {
        for (int j = i; j > 0 && cand[j].known > cand[j - 1].known; j--) {
            Cand t = cand[j]; cand[j] = cand[j - 1]; cand[j - 1] = t;
        }
    }
    int matched = 0;
    for (int i = 0; i < cands && w.runs < RD_RUNS; i++) {
        const Cand& x = cand[i];
        bool clash = false;
        for (int r = 0; r < w.runs && !clash; r++) {
            const Redraw::Run& o = w.run[r];
            if (x.k < o.k + o.n && o.k < x.k + x.n) clash = true;           // the same table bytes
            int dy = x.y - o.y;
            if (dy < 8 && dy > -8 && x.c < o.c + o.n && o.c < x.c + x.n) clash = true;  // the same cells
        }
        if (clash) continue;
        w.run[w.runs++] = { x.y, x.c, x.n, x.k };
        matched += x.known;
    }
    if (w.runs == 0 || matched < 8) return false;
    // the characters not known: what they are on the screen, if the same everywhere
    for (int r = 0; r < w.runs; r++) {
        const Redraw::Run& o = w.run[r];
        for (int j = 0; j < o.n; j++) {
            int i = index(mem((uint16_t) (addr + o.k + j)));
            if (w.known[i] == 1) continue;
            uint8_t g[8];
            screen_cell(scr, o.y, o.c + j, g);
            if (w.known[i] == 0) { memcpy(w.glyph[i], g, 8); w.known[i] = 2; }
            else if (w.known[i] == 2 && memcmp(w.glyph[i], g, 8) != 0) w.known[i] = 3;   // (not the same)
        }
    }
    return true;
}

// Draws the new table (now(k): its bytes) where redraw_find found the old one (old(k)); mem for
// the game's font: put(offset in the screen, byte) for each byte that changes. False (and
// nothing put) if a new character can't be drawn.
template <typename Mem, typename Old, typename Now, typename Rom, typename Put>
bool redraw_apply(const Redraw& w, Mem mem, Old old, Now now, const uint8_t* scr, Rom rom, Put put) {
    auto glyph = [&](uint8_t b, uint8_t* g) {
        for (int i = 0; i < w.chars; i++) {
            if (w.ch[i] == b) {
                if (w.known[i] == 1 || w.known[i] == 2) { memcpy(g, w.glyph[i], 8); return true; }
                break;
            }
        }
        if (w.rom) return rom(b, g);
        int32_t a = w.font + 8 * b;
        if (w.font < 0 || a < 0x4000 || a + 8 > 0x10000) return false;
        for (int r = 0; r < 8; r++) g[r] = mem((uint16_t) (a + r));
        return true;
    };
    for (int pass = 0; pass < 2; pass++) {
        for (int r = 0; r < w.runs; r++) {
            const Redraw::Run& o = w.run[r];
            for (int j = 0; j < o.n; j++) {
                uint8_t b = now(o.k + j);
                if (b == old(o.k + j)) continue;
                uint8_t g[8];
                if (!glyph(b, g)) return false;
                if (pass == 0) continue;
                for (int i = 0; i < 8; i++) {
                    uint16_t at = screen_at(o.y + i, o.c + j);
                    if (scr[at] != g[i]) put(at, g[i]);
                }
            }
        }
    }
    return true;
}

}   // namespace hiscore
