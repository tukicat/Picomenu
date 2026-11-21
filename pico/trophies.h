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
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

// A game's own trophies (Y - Your trophies). Each game has its trophies in
// /Trophies/<game>.txt, made on Y's screen or written on a PC (the WiFi file transfer's Edit):
//
//   checks = 1                (how often its memory is looked at, a second: 0 never, up to 10)
//   [Trophy]
//   name = Top Gun
//   about = Score 50,000 or more
//   score = 50000
//
// and what has been won in /Trophies/<game>.won (picoface.cxx: tr_prog_read). What wins a
// trophy: the game's high score table, read when it is saved, with a new score
//   score = N    of N or more
//   top = 1      above the top score the table came with
//   place = N    in the top N of the table
// or the game's memory, looked at while it runs (an NMI, checks a second):
//   peek = 23610 >= 3         a byte (an address in decimal, or $5C3A in hex)
//   peek = 30000w > 1000      a word (two bytes, low first)
//   peek = $8000 changed      (also: = <> >= <= > <, changed, up, down)
//   peek = 44128 & 127 = 8    only some of its bits (& 127: all but bit 7)
//   peek = 65000d6 rises 400  6 bytes as a number's digits (as numbers 0-9 or as characters
//                             "0"-"9"), and "rises n": up by n or more (since it began to rise,
//                             within the trophy's "within" seconds; or since the last look)
// "within = 2": a peek true counts as true for 2 seconds after, so its peeks needn't be true at
// the same look (a throw, then the points for it).
// up to 4 peeks: all of them true at once (or "match = any": one of them), and "held = 5": true
// for 5 seconds together. Lines it doesn't know are kept in the file and left alone.
namespace trophies {

static const int MAX = 16;                  // trophies of a game shown (and checked)
static const int NAME_MAX = 18;             // letters of a name (the list)
static const int ABOUT_MAX = 60;            // and of what it means (two lines of 30)
static const int ABOUT_LINE = 30;
static const int SCORES = 10;               // scores of a table kept
static const int PEEKS = 4;                 // peeks of a trophy

enum Kind : uint8_t { K_NONE, K_SCORE, K_TOP, K_PLACE, K_MEM, K_NEEDS };
// (O_UPBY, "up by 1": up by exactly that much since the look before: a life picked up, not the
// lives filled up again for a new game)
// (O_NOTDOWN, "not down": true at every look but the first unless it went down since the look
// before: with held = 300, five minutes without a life lost)
enum Op : uint8_t { O_EQ, O_NE, O_GE, O_LE, O_GT, O_LT, O_CHANGED, O_UP, O_DOWN, O_RISES, O_UPBY, O_NOTDOWN, O_COUNT };
static const char* const OP_TEXT[O_COUNT] = { "=", "<>", ">=", "<=", ">", "<", "changed", "up", "down", "rises", "up by",
    "not down" };
inline bool op_has_value(uint8_t op) { return op < O_CHANGED || op == O_RISES || op == O_UPBY; }
static const int DIGITS_MAX = 8;

struct Peek {
    uint16_t addr;
    uint8_t op;
    uint8_t word;               // two bytes (low first)
    uint32_t value;
    uint16_t mask;              // "& n": only these bits of it looked at (0: all)
    uint8_t digits;             // a number's digits, one a byte, this many (0: a byte or a word)
};

// The bytes a peek reads
inline int peek_bytes(const Peek& p) { return p.digits ? p.digits : p.word ? 2 : 1; }

// What a peek read, as a number: digits as numbers 0-9 or characters "0"-"9" (a space as 0)
inline uint32_t peek_value(const Peek& p, const uint8_t* b) {
    uint32_t v;
    if (p.digits) {
        v = 0;
        for (int i = 0; i < p.digits; i++) {
            uint8_t c = b[i];
            if (c >= '0' && c <= '9') c = (uint8_t) (c - '0');
            else if (c > 9) c = 0;
            v = v * 10 + c;
        }
    } else {
        v = p.word ? (uint32_t) (b[0] | b[1] << 8) : b[0];
    }
    return p.mask ? (v & p.mask) : v;
}

struct Def {
    char name[NAME_MAX + 1];
    char about[ABOUT_MAX + 1];
    uint8_t kind;
    uint32_t value;
    Peek peek[PEEKS];           // K_MEM
    uint8_t npeek;
    uint8_t any;                // "match = any": one peek true is enough
    uint16_t held;              // seconds they must stay true
    uint16_t within;            // seconds a peek true still counts after (and "rises" has to rise in)
    uint32_t need[MAX - 1];     // K_NEEDS: the other trophies it needs won (their names' hashes)
    uint8_t nneed;
};

// A number: decimal, or hex after $ or 0x. False if there is none
inline bool parse_num(const char*& s, uint32_t& v) {
    while (*s == ' ' || *s == '\t') s++;
    int base = 10;
    if (*s == '$' || *s == '#') { base = 16; s++; }
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    v = 0;
    int n = 0;
    for (;; s++, n++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : base == 16 && *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 :
            base == 16 && *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
        if (d < 0) break;
        v = v * (uint32_t) base + (uint32_t) d;
    }
    return n > 0;
}

// "<address>[w] <test> [value]"
inline bool parse_peek(const char* s, Peek& p) {
    uint32_t a, v = 0;
    if (!parse_num(s, a) || a > 0xFFFF) return false;
    p.addr = (uint16_t) a;
    p.word = 0;
    p.digits = 0;
    if (*s == '.' && (s[1] == 'w' || s[1] == 'W' || s[1] == 'd' || s[1] == 'D')) s++;
    if (*s == 'w' || *s == 'W') {
        p.word = 1;
        s++;
    } else if ((*s == 'd' || *s == 'D') && s[1] >= '1' && s[1] <= '0' + DIGITS_MAX) {
        p.digits = (uint8_t) (s[1] - '0');
        s += 2;
    }
    while (*s == ' ' || *s == '\t') s++;
    p.mask = 0;
    if (*s == '&') {
        uint32_t m;
        s++;
        if (!parse_num(s, m) || m > 0xFFFF) return false;
        p.mask = (uint16_t) m;
        while (*s == ' ' || *s == '\t') s++;
    }
    char t[8];
    int n = 0;
    const bool sym = *s == '=' || *s == '!' || *s == '<' || *s == '>';
    while (n < 7 && *s != '\0' && (sym ? (*s == '=' || *s == '!' || *s == '<' || *s == '>') :
            ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')))) {
        t[n++] = (char) tolower((unsigned char) *s++);
    }
    t[n] = '\0';
    if (strcmp(t, "==") == 0) strcpy(t, "=");
    if (strcmp(t, "!=") == 0) strcpy(t, "<>");
    int op = -1;
    for (int i = 0; i < O_COUNT; i++) {
        if (strcmp(t, OP_TEXT[i]) == 0) op = i;
    }
    if (op < 0 && strcmp(t, "not") == 0) {
        // "not down"
        const char* b = s;
        while (*b == ' ' || *b == '\t') b++;
        if (tolower((unsigned char) b[0]) == 'd' && tolower((unsigned char) b[1]) == 'o' && tolower((unsigned char) b[2]) == 'w' &&
                tolower((unsigned char) b[3]) == 'n' && !isalpha((unsigned char) b[4])) {
            op = O_NOTDOWN;
            s = b + 4;
        }
    }
    if (op < 0) return false;
    if (op == O_UP) {
        // "up by n"
        const char* b = s;
        while (*b == ' ' || *b == '\t') b++;
        if ((b[0] == 'b' || b[0] == 'B') && (b[1] == 'y' || b[1] == 'Y') && !isalpha((unsigned char) b[2])) {
            op = O_UPBY;
            s = b + 2;
        }
    }
    p.op = (uint8_t) op;
    if (op_has_value(p.op)) {
        if (!parse_num(s, v)) return false;
    }
    p.value = v;
    return true;
}

// As the file has it, and Y's screen shows it
inline int peek_text(char* out, size_t size, const Peek& p) {
    char m[10] = "", k[6] = "";
    if (p.mask != 0) snprintf(m, sizeof(m), " & %u", (unsigned) p.mask);
    if (p.digits) snprintf(k, sizeof(k), "d%u", (unsigned) p.digits);
    else if (p.word) snprintf(k, sizeof(k), "w");
    if (op_has_value(p.op)) {
        return snprintf(out, size, "%u%s%s %s %lu", (unsigned) p.addr, k, m, OP_TEXT[p.op], (unsigned long) p.value);
    }
    return snprintf(out, size, "%u%s%s %s", (unsigned) p.addr, k, m, OP_TEXT[p.op]);
}

// A description's first line, at most width letters, broken at a space: its length (the second
// line starts after it, and after a space there)
inline int wrap_first(const char* s, int width) {
    const int n = (int) strlen(s);
    if (n <= width) return n;
    for (int i = width; i > 0; i--) {
        if (s[i] == ' ') return i;
    }
    return width;
}
inline const char* wrap_rest(const char* s, int width) {
    s += wrap_first(s, width);
    while (*s == ' ') s++;
    return s;
}

// Is a peek true for v (as peek_value has it; prev: what was read the time before, if have;
// base: where "rises" counts from)?
inline bool peek_true(const Peek& p, uint32_t v, uint32_t prev, bool have, uint32_t base = 0) {
    switch (p.op) {
    case O_EQ: return v == p.value;
    case O_NE: return v != p.value;
    case O_GE: return v >= p.value;
    case O_LE: return v <= p.value;
    case O_GT: return v > p.value;
    case O_LT: return v < p.value;
    case O_CHANGED: return have && v != prev;
    case O_UP: return have && v > prev;
    case O_DOWN: return have && v < prev;
    case O_RISES: return have && v >= base && v - base >= p.value;
    case O_UPBY: return have && v > prev && v - prev == p.value;
    case O_NOTDOWN: return have && v >= prev;
    default: return false;
    }
}

// The scores of a high score table, highest first
struct Table {
    uint32_t v[SCORES];
    uint8_t n;
};

// A cup, for the list and the banner (a character's 8 rows)
static const uint8_t CUP[8] = { 0x7E, 0xBD, 0xBD, 0x7E, 0x3C, 0x18, 0x3C, 0x7E };

// A name as a number (FNV-1a), not minding case: what has been won is kept as these
inline uint32_t name_hash(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s != '\0'; s++) {
        char c = *s;
        if (c >= 'a' && c <= 'z') c = (char) (c - 32);
        h = (h ^ (uint8_t) c) * 16777619u;
    }
    return h == 0 ? 1 : h;
}

// Reads the definitions a line at a time: line() and finish() say true when d holds a whole
// trophy (until the next call): line() at the [ line after it, finish() at the end of the file.
// A trophy with no name is left out.
struct Parser {
    Def d = {};
    bool open = false, next_open = false, restart = false;
    uint8_t checks = 1;         // "checks = n" before the first trophy: the memory looked at n times a second

    static void copy(char* to, int max, const char* from) {
        int n = 0;
        while (from[n] != '\0' && from[n] != '\r' && from[n] != '\n' && n < max) {
            to[n] = from[n];
            n++;
        }
        while (n > 0 && to[n - 1] == ' ') n--;
        to[n] = '\0';
    }
    static bool key_is(const char* s, size_t n, const char* key) {
        size_t k = strlen(key);
        if (n != k) return false;
        for (size_t i = 0; i < k; i++) {
            if (tolower((unsigned char) s[i]) != key[i]) return false;
        }
        return true;
    }
    bool line(const char* s) {
        if (restart) {
            memset(&d, 0, sizeof(d));
            open = next_open;
            restart = false;
        }
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0' || *s == '\r' || *s == '\n' || *s == ';' || *s == '#') return false;
        if (*s == '[') {
            next_open = strlen(s) >= 8 && key_is(s + 1, 6, "trophy") && s[7] == ']';
            if (open && d.name[0] != '\0') {
                restart = true;
                return true;
            }
            memset(&d, 0, sizeof(d));
            open = next_open;
            return false;
        }
        const char* eq = strchr(s, '=');
        if (eq == nullptr) return false;
        size_t kn = (size_t) (eq - s);
        while (kn > 0 && (s[kn - 1] == ' ' || s[kn - 1] == '\t')) kn--;
        const char* v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (!open) {
            uint32_t c;
            if (key_is(s, kn, "checks") && parse_num(v, c)) checks = (uint8_t) (c > 10 ? 10 : c);
            return false;
        }
        if (key_is(s, kn, "peek")) {
            if (d.npeek < PEEKS && parse_peek(v, d.peek[d.npeek])) {
                d.npeek++;
                d.kind = K_MEM;
            }
        } else if (key_is(s, kn, "match")) {
            d.any = tolower((unsigned char) *v) == 'a' && tolower((unsigned char) v[1]) == 'n';
        } else if (key_is(s, kn, "held")) {
            uint32_t h;
            if (parse_num(v, h)) d.held = (uint16_t) (h > 3600 ? 3600 : h);
        } else if (key_is(s, kn, "within")) {
            uint32_t h;
            if (parse_num(v, h)) d.within = (uint16_t) (h > 600 ? 600 : h);
        } else if (key_is(s, kn, "need") || key_is(s, kn, "needs")) {
            // a name, or names with commas between
            while (*v != '\0') {
                const char* e = v;
                while (*e != '\0' && *e != ',' && *e != '\r' && *e != '\n') e++;
                char nm[NAME_MAX + 1];
                size_t l = (size_t) (e - v) < NAME_MAX ? (size_t) (e - v) : NAME_MAX;
                memcpy(nm, v, l);
                nm[l] = '\0';
                copy(nm, NAME_MAX, nm);
                if (nm[0] != '\0' && d.nneed < MAX - 1 && (d.kind == K_NONE || d.kind == K_NEEDS)) {
                    d.need[d.nneed++] = name_hash(nm);
                    d.kind = K_NEEDS;
                }
                if (*e != ',') break;
                v = e + 1;
                while (*v == ' ' || *v == '\t') v++;
            }
        } else if (key_is(s, kn, "name")) {
            copy(d.name, NAME_MAX, v);
        } else if (key_is(s, kn, "about")) {
            copy(d.about, ABOUT_MAX, v);
        } else if (key_is(s, kn, "score") || key_is(s, kn, "top") || key_is(s, kn, "place")) {
            uint32_t n = 0;
            while (*v >= '0' && *v <= '9') n = n * 10 + (uint32_t) (*v++ - '0');
            if (d.kind != K_MEM) {
                d.kind = key_is(s, kn, "score") ? K_SCORE : key_is(s, kn, "top") ? K_TOP : K_PLACE;
                d.value = n;
            }
        }
        return false;
    }
    bool finish() {
        if (restart) {
            memset(&d, 0, sizeof(d));
            open = next_open;
            restart = false;
        }
        const bool got = open && d.name[0] != '\0';
        open = false;
        return got;
    }
};

// The start of a new file of trophies
static const char FILE_HEAD[] =
    "; PicoMenu's trophies for this game: made with Y on the menu, or written here.\r\n"
    "; Each one: [Trophy], name = (18 letters), about = (60 letters), and what wins it:\r\n"
    ";   score = 50000   a new score of 50000 or more in its high score table\r\n"
    ";   top = 1         a new score above the top one the table came with\r\n"
    ";   place = 3       a new score in the top 3 of the table\r\n"
    ";   peek = 23610 >= 3   its memory: an address (decimal, or $5C3A), w for a word,\r\n"
    ";                   & 127 for some of its bits only, then = <> >= <= > <\r\n"
    ";                   and a number, or changed, up, down; up by 1: up by exactly 1;\r\n"
    ";                   not down: it hasn't gone down (with held = 300: for 5 minutes)\r\n"
    ";                   65000d6: 6 bytes as a number's digits; rises 400: up by 400\r\n"
    ";   (up to 4 peeks, all true; match = any: one of them; held = 5: for 5 seconds;\r\n"
    ";   within = 2: each counts as true for 2 seconds after, and rises within them)\r\n"
    ";   need = Top 3: Swimming   won once that trophy of this game is won too: a need\r\n"
    ";                   line for each (or names with commas between); all of them\r\n"
    "; checks = 1 (at the top): its memory is looked at once a second (0 never, up to 10).\r\n";

// One trophy as the file has it
inline int write_def(char* out, size_t size, const Def& d) {
    static const char* const KEY[] = { "", "score", "top", "place" };
    int n = snprintf(out, size, "\r\n[Trophy]\r\nname = %s\r\nabout = %s\r\n", d.name, d.about);
    if (d.kind != K_NONE && d.kind <= K_PLACE && n > 0 && (size_t) n < size) {
        n += snprintf(out + n, size - (size_t) n, "%s = %lu\r\n", KEY[d.kind], (unsigned long) d.value);
    }
    if (d.kind == K_MEM) {
        for (int i = 0; i < d.npeek && n > 0 && (size_t) n < size; i++) {
            n += snprintf(out + n, size - (size_t) n, "peek = ");
            if ((size_t) n < size) n += peek_text(out + n, size - (size_t) n, d.peek[i]);
            if ((size_t) n < size) n += snprintf(out + n, size - (size_t) n, "\r\n");
        }
        if (d.any && (size_t) n < size) n += snprintf(out + n, size - (size_t) n, "match = any\r\n");
        if (d.held > 0 && (size_t) n < size) n += snprintf(out + n, size - (size_t) n, "held = %u\r\n", (unsigned) d.held);
        if (d.within > 0 && (size_t) n < size) n += snprintf(out + n, size - (size_t) n, "within = %u\r\n", (unsigned) d.within);
    }
    return n;
}

// What wins it, in words (Y's screen)
inline void describe(const Def& d, char* out, size_t size) {
    switch (d.kind) {
    case K_SCORE: snprintf(out, size, "A score of %lu or more", (unsigned long) d.value); break;
    case K_TOP: snprintf(out, size, "Beat the table's top score"); break;
    case K_PLACE:
        if (d.value <= 1) snprintf(out, size, "First place in the table");
        else snprintf(out, size, "Into the top %lu of the table", (unsigned long) d.value);
        break;
    case K_MEM: {
        int n = peek_text(out, size, d.peek[0]);
        if (d.npeek > 1 && n > 0 && (size_t) n < size) n += snprintf(out + n, size - (size_t) n, d.any ? " or %d more" : " and %d more", d.npeek - 1);
        if (d.within > 0 && n > 0 && (size_t) n < size) n += snprintf(out + n, size - (size_t) n, " in %us", (unsigned) d.within);
        if (d.held > 0 && n > 0 && (size_t) n < size) snprintf(out + n, size - (size_t) n, " for %us", (unsigned) d.held);
        break;
    }
    case K_NEEDS:
        if (d.nneed == 1) snprintf(out, size, "Won with 1 other trophy");
        else snprintf(out, size, "Won with %d other trophies", d.nneed);
        break;
    default: snprintf(out, size, "(not checked by this PicoMenu)"); break;
    }
}

// A peek in words: "40285 is at least 128", "65000 (6 digits) rises by 400 or more"
inline int peek_words(char* out, size_t size, const Peek& p) {
    char at[32];
    if (p.digits) snprintf(at, sizeof(at), "%u (%u digits)", (unsigned) p.addr, (unsigned) p.digits);
    else if (p.word) snprintf(at, sizeof(at), "%u (a word)", (unsigned) p.addr);
    else if (p.mask) snprintf(at, sizeof(at), "%u & %u", (unsigned) p.addr, (unsigned) p.mask);
    else snprintf(at, sizeof(at), "%u", (unsigned) p.addr);
    static const char* const W[O_COUNT] = { "is", "is not", "is at least", "is at most", "is more than",
        "is less than", "changes", "goes up", "goes down", "rises by", "goes up by exactly", "doesn't go down" };
    const char* w = p.op < O_COUNT ? W[p.op] : "?";
    if (p.op == O_RISES) return snprintf(out, size, "%s %s %lu or more", at, w, (unsigned long) p.value);
    if (op_has_value(p.op)) return snprintf(out, size, "%s %s %lu", at, w, (unsigned long) p.value);
    return snprintf(out, size, "%s %s", at, w);
}

// What wins it, as a sentence (Y shows it before a new one is saved). A K_NEEDS one's names
// aren't known here: "all of these are won:", for the caller to follow with them.
inline void sentence(const Def& d, char* out, size_t size) {
    int n = 0;
    auto add = [&](const char* f, auto... a) {
        if (n >= 0 && (size_t) n < size) n += snprintf(out + n, size - (size_t) n, f, a...);
    };
    switch (d.kind) {
    case K_SCORE: add("Won when a new score of %lu or more is in the high score table.", (unsigned long) d.value); break;
    case K_TOP: add("%s", "Won when a new score beats the top one the high score table came with."); break;
    case K_PLACE:
        if (d.value <= 1) add("%s", "Won when a new score takes first place in the high score table.");
        else add("Won when a new score is in the top %lu of the high score table.", (unsigned long) d.value);
        break;
    case K_MEM:
        add("%s", "Won when ");
        for (int i = 0; i < d.npeek; i++) {
            if (i > 0) add("%s", d.any ? " or " : " and ");
            if ((size_t) n < size) n += peek_words(out + n, size - (size_t) n, d.peek[i]);
        }
        if (d.within > 0 && (d.npeek > 1 || (d.npeek == 1 && d.peek[0].op == O_RISES)))
            add(d.npeek > 1 ? ", within %u seconds of each other" : ", within %u seconds", (unsigned) d.within);
        if (d.held > 0 && d.held % 60 == 0) add(", for %u minute%s", (unsigned) (d.held / 60), d.held == 60 ? "" : "s");
        else if (d.held > 0) add(", for %u seconds", (unsigned) d.held);
        add("%s", ".");
        break;
    case K_NEEDS: add("%s", "Won when all of these are won:"); break;
    default: add("%s", "Not checked by this PicoMenu."); break;
    }
}

// The scores of now that were not in before (a score there twice counts twice): their places
// in now, from 0. How many there are.
inline int new_places(const Table& now, const Table& before, uint8_t* places) {
    bool used[SCORES] = {};
    int n = 0;
    for (int i = 0; i < now.n; i++) {
        bool old = false;
        for (int j = 0; j < before.n && !old; j++) {
            if (!used[j] && before.v[j] == now.v[i]) {
                used[j] = true;
                old = true;
            }
        }
        if (!old) places[n++] = (uint8_t) i;
    }
    return n;
}

// Does a new score win it? (base_top: the top score the table came with)
inline bool wins(const Def& d, const Table& now, const uint8_t* places, int nnew, uint32_t base_top) {
    for (int k = 0; k < nnew; k++) {
        const uint32_t s = now.v[places[k]];
        switch (d.kind) {
        case K_SCORE: if (d.value > 0 && s >= d.value) return true; break;
        case K_TOP: if (s > base_top) return true; break;
        case K_PLACE: if (places[k] < d.value) return true; break;
        default: break;
        }
    }
    return false;
}

}
