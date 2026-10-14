// Helpers for the quick-save, reload, file details, Poke lists and diagnostics.
// Nothing here touches the hardware, so it is tested on a PC (Docs/Instructions.txt).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <initializer_list>
#include "snapload.h"

namespace extras {

// ---------------------------------------------------------------------------
// Quick-save names

static const char QUICK_SUFFIX[] = " Save Game";
static const size_t QUICK_SUFFIX_LEN = sizeof(QUICK_SUFFIX) - 1;

// The length of name without a " Save Game" on the end (any case), so a quick-save
// of a quick-saved game does not keep adding it
// (or " Save Game 2", " Save Game 3" ... the numbered saves: see numbered_name)
inline size_t base_name_length(const char* name) {
    size_t len = strlen(name);
    size_t digits = 0;
    while (digits < len && name[len - 1 - digits] >= '0' && name[len - 1 - digits] <= '9') {
        digits++;
    }
    if (digits > 0 && digits <= 3 && len > digits + 1 + QUICK_SUFFIX_LEN && name[len - digits - 1] == ' ' &&
            strncasecmp(name + len - digits - 1 - QUICK_SUFFIX_LEN, QUICK_SUFFIX, QUICK_SUFFIX_LEN) == 0) {
        len -= digits + 1;
    }
    if (len > QUICK_SUFFIX_LEN && strncasecmp(name + len - QUICK_SUFFIX_LEN, QUICK_SUFFIX, QUICK_SUFFIX_LEN) == 0) {
        len -= QUICK_SUFFIX_LEN;
    }
    return len;
}

// The name of save number number (2 or more) of a game: its quick-save name, a blank
// and the number: "Exolon Save Game 2". Returns the length, or 0 if it does not fit in maxlen.
inline size_t numbered_name(const char* quick_name, int number, size_t maxlen, char* out) {
    int n = snprintf(out, maxlen + 1, "%s %d", quick_name, number);
    return (n > 0 && (size_t) n <= maxlen) ? (size_t) n : 0;
}

// Makes the quick-save name for the game called last: its name and " Save Game".
// A long name is cut so the whole fits in maxlen characters. Returns the length
// of what it wrote to out (maxlen + 1 bytes), or 0 if there is no name to use.
inline size_t quick_save_name(const char* last, size_t maxlen, char* out) {
    size_t base = base_name_length(last);
    if (maxlen <= QUICK_SUFFIX_LEN) {
        return 0;
    }
    if (base > maxlen - QUICK_SUFFIX_LEN) {
        base = maxlen - QUICK_SUFFIX_LEN;
    }
    while (base > 0 && last[base - 1] == ' ') {
        base--;                                 // not a space before the suffix's own
    }
    if (base == 0) {
        return 0;
    }
    memcpy(out, last, base);
    memcpy(out + base, QUICK_SUFFIX, QUICK_SUFFIX_LEN + 1);
    return base + QUICK_SUFFIX_LEN;
}

// ---------------------------------------------------------------------------
// The details shown for a file in the Browse Games list

// "999B", " 49K", "131K", "  2M": always four characters
inline void format_size(uint32_t size, char out[5]) {
    if (size < 1000) {
        snprintf(out, 5, "%3luB", (unsigned long) size);
        return;
    }
    uint32_t k = (uint32_t) (((uint64_t) size + 512) / 1024);
    if (k < 1000) {
        snprintf(out, 5, "%3luK", (unsigned long) k);
        return;
    }
    uint32_t m = (uint32_t) (((uint64_t) size + 524288) / 1048576);
    if (m > 999) {
        m = 999;
    }
    snprintf(out, 5, "%3luM", (unsigned long) m);
}

// What a snapshot needs, from the start of the file
enum Needs {
    NEEDS_UNKNOWN = -1,     // a tape: the loader decides
    NEEDS_48K = 0,          // runs as a 48K game
    NEEDS_128K = 1,         // a 128K snapshot
    NEEDS_NOT_SUPPORTED = 2 // damaged, or a machine that cannot be loaded
};

// ext is the file's extension, size its size and head its first headlen bytes (at
// least 64 if the file has that many). The rules are the ones the loaders use.
inline Needs snapshot_needs(const char* ext, uint32_t size, const uint8_t* head, size_t headlen) {
    if (strcasecmp(ext, "sna") == 0) {
        return snapload::is_sna128(size) ? NEEDS_128K : NEEDS_48K;
    }
    if (strcasecmp(ext, "z80") == 0) {
        if (headlen < 30) {
            return NEEDS_NOT_SUPPORTED;
        }
        if ((head[6] | (head[7] << 8)) != 0 || size == snapload::Z80V1_SIZE) {
            return NEEDS_48K;                   // version 1
        }
        if (headlen < 35) {
            return NEEDS_NOT_SUPPORTED;
        }
        uint32_t extlen = head[30] | (head[31] << 8);
        if (extlen < 23 || extlen > 64) {
            return NEEDS_NOT_SUPPORTED;
        }
        uint8_t hardware = head[34];
        bool is48K = extlen == 23 ? hardware <= 2 : (hardware <= 3 || hardware == 14);
        return is48K ? NEEDS_48K : NEEDS_128K;
    }
    if (strcasecmp(ext, "szx") == 0) {
        if (headlen < 8 || memcmp(head, "ZXST", 4) != 0) {
            return NEEDS_NOT_SUPPORTED;
        }
        uint8_t machine = head[6];
        if (machine == 0 || machine == 1 || machine == 8 || machine == 15) {
            return NEEDS_48K;
        }
        if (machine == 2 || machine == 3 || machine == 16) {
            return NEEDS_128K;
        }
        return NEEDS_NOT_SUPPORTED;
    }
    return NEEDS_UNKNOWN;
}

// The eight characters that follow a file's name in the list: its size and "128" for
// a snapshot that needs a 128K machine, "48" for one that runs on a 48K machine ("???" if it cannot be loaded,
// blank for a tape, which the loader decides)
inline void describe_file(Needs needs, uint32_t size, char out[9]) {
    char sz[5];
    format_size(size, sz);
    snprintf(out, 9, "%s %s", sz,
        needs == NEEDS_128K ? "128" : needs == NEEDS_48K ? " 48" : needs == NEEDS_NOT_SUPPORTED ? "???" : "   ");
}

// ---------------------------------------------------------------------------
// Sizes in the diagnostics: "812K", "1.4M", "58.2G"

inline void format_big(uint64_t bytes, char* out, size_t outsize) {
    if (bytes < 1024ull * 1024) {
        snprintf(out, outsize, "%luK", (unsigned long) (bytes / 1024));
    }
    else if (bytes < 1024ull * 1024 * 1024) {
        uint64_t tenths = bytes * 10 / (1024ull * 1024);
        snprintf(out, outsize, "%lu.%luM", (unsigned long) (tenths / 10), (unsigned long) (tenths % 10));
    }
    else {
        uint64_t tenths = bytes * 10 / (1024ull * 1024 * 1024);
        snprintf(out, outsize, "%lu.%luG", (unsigned long) (tenths / 10), (unsigned long) (tenths % 10));
    }
}

// ---------------------------------------------------------------------------
// Poke lists: .pok files, the format of the Pokes Database
//
//   NName of the cheat
//   M  bank address value original      more pokes for this cheat follow
//   Z  bank address value original      the last poke of this cheat
//   Y                                   the end of the file
//
// A bank of 0 to 7 is a 128K RAM bank, 8 and above means the address as the
// processor sees it. A value of 256 means the player chooses it.

struct PokPoke {
    uint16_t address;
    uint16_t value;         // 0 to 255, or 256 for "ask"
    uint8_t bank;           // 0 to 7, or 8 for none
};

static const int POK_MAX_CHEATS = 40;
static const int POK_MAX_POKES = 200;
static const int POK_PER_CHEAT = 16;        // the most one cheat can have
static const int POK_NAME_MAX = 64;         // the longest name kept (the poke list scrolls what does not fit)

struct PokCheat {
    char name[POK_NAME_MAX + 1];
    uint16_t first;
    uint8_t count;
    bool ask;               // has a value the player must choose
};

class PokFile {
public:
    PokCheat cheats[POK_MAX_CHEATS];
    PokPoke pokes[POK_MAX_POKES];
    int ncheats = 0;
    int npokes = 0;
    int skipped = 0;        // cheats left out because there was no room

    void clear() {
        ncheats = 0;
        npokes = 0;
        skipped = 0;
    }

    // Reads a file with read(buf, n) returning the number of bytes it got. Returns
    // the number of cheats.
    template <class File>
    int load(File& f) {
        clear();
        uint8_t buf[64];
        char line[96];
        size_t len = 0;
        bool skipline = false;
        bool done = false;
        for (;;) {
            int got = f.read(buf, sizeof(buf));
            if (got <= 0) {
                break;
            }
            for (int i = 0; i < got && !done; i++) {
                char c = (char) buf[i];
                if (c == '\n' || c == '\r') {
                    if (len > 0 && !skipline) {
                        line[len] = '\0';
                        done = take_line(line);
                    }
                    len = 0;
                    skipline = false;
                }
                else if (len < sizeof(line) - 1) {
                    line[len++] = c;
                }
                else {
                    skipline = true;            // a line this long is not a poke
                }
            }
            if (done) {
                break;
            }
        }
        if (!done && len > 0 && !skipline) {
            line[len] = '\0';
            take_line(line);
        }
        end_cheat();
        return ncheats;
    }

private:
    int current = -1;       // the cheat being filled in

    void end_cheat() {
        if (current >= 0 && cheats[current].count == 0) {
            ncheats--;                          // a name with no pokes
        }
        current = -1;
    }

    static bool number(const char*& p, int& v) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p < '0' || *p > '9') {
            return false;
        }
        int n = 0;
        while (*p >= '0' && *p <= '9') {
            n = n * 10 + (*p - '0');
            if (n > 100000) {
                n = 100000;
            }
            p++;
        }
        v = n;
        return true;
    }

    // true at the end of the file ("Y")
    bool take_line(const char* line) {
        char kind = line[0];
        if (kind == 'Y') {
            return true;
        }
        if (kind == 'N') {
            end_cheat();
            if (ncheats >= POK_MAX_CHEATS) {
                skipped++;
                return false;
            }
            PokCheat& c = cheats[ncheats];
            const char* s = line + 1;
            while (*s == ' ') {
                s++;
            }
            size_t n = 0;
            for (; *s != '\0' && n < (size_t) POK_NAME_MAX; s++, n++) {
                c.name[n] = (*s >= 32 && *s <= 126) ? *s : '?';
            }
            while (n > 0 && c.name[n - 1] == ' ') {
                n--;
            }
            c.name[n] = '\0';
            c.first = (uint16_t) npokes;
            c.count = 0;
            c.ask = false;
            current = ncheats++;
            return false;
        }
        if ((kind == 'M' || kind == 'Z') && current >= 0) {
            const char* p = line + 1;
            int bank, address, value, original;
            bool ok = number(p, bank) && number(p, address) && number(p, value) && number(p, original);
            if (ok && address < 65536 && value <= 256 && npokes < POK_MAX_POKES &&
                cheats[current].count < POK_PER_CHEAT) {
                PokPoke& k = pokes[npokes++];
                k.address = (uint16_t) address;
                k.value = (uint16_t) value;
                k.bank = bank >= 8 ? 8 : (uint8_t) bank;
                cheats[current].count++;
                if (value == 256) {
                    cheats[current].ask = true;
                }
            }
            if (kind == 'Z') {
                end_cheat();
            }
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// Writing the poke list for the NMI ROM

// ACTION_POKE_LIST: dest[0] count (at most 10), dest[1] non-zero if more follow,
// then a word for each name's offset from base, then the names. base is the
// start of the NMI ROM's data; dest is an offset into it.
inline void poke_list_page(const PokFile& pok, int start, uint8_t* base, uint16_t dest) {
    uint16_t strings = (uint16_t) (dest + 2 + 2 * 10);
    int count = 0;
    for (int i = start; i < pok.ncheats && count < 10; i++, count++) {
        base[dest + 2 + 2 * count] = (uint8_t) (strings & 0xFF);
        base[dest + 2 + 2 * count + 1] = (uint8_t) (strings >> 8);
        size_t n = strlen(pok.cheats[i].name);
        memcpy(base + strings, pok.cheats[i].name, n + 1);
        strings = (uint16_t) (strings + n + 1);
    }
    base[dest] = (uint8_t) count;
    base[dest + 1] = start + count < pok.ncheats ? 1 : 0;
}

// ACTION_POKE_GET: dest[0] the number of pokes, dest[1] non-zero if one asks
// for a value, then five bytes each: address (word), value (word), bank
inline bool poke_get(const PokFile& pok, int cheat, uint8_t* base, uint16_t dest) {
    if (cheat < 0 || cheat >= pok.ncheats) {
        return false;
    }
    const PokCheat& c = pok.cheats[cheat];
    base[dest] = c.count;
    base[dest + 1] = c.ask ? 1 : 0;
    for (int i = 0; i < c.count; i++) {
        const PokPoke& k = pok.pokes[c.first + i];
        uint8_t* d = base + dest + 2 + 5 * i;
        d[0] = (uint8_t) (k.address & 0xFF);
        d[1] = (uint8_t) (k.address >> 8);
        d[2] = (uint8_t) (k.value & 0xFF);
        d[3] = (uint8_t) (k.value >> 8);
        d[4] = k.bank;
    }
    return true;
}


// ---------------------------------------------------------------------------
// The last game, kept in a file in the internal flash so that L-Reload and the
// Save screen's name survive power off

static const size_t LG_DIR_MAX = 128;       // sizeof(sd_cwd) in picoface.cxx
static const size_t LG_RELOAD_MAX = 260;    // name and extension, two strings
static const size_t LG_NAME_MAX = 64;

struct LastGame {
    bool reload_known = false;
    uint8_t reload_drive = 0;               // 0 internal flash, 1 SD card
    char reload_dir[LG_DIR_MAX] = "";
    char reload_name[LG_RELOAD_MAX] = "";   // "name\0ext\0"
    bool last_known = false;
    uint8_t last_drive = 0;
    char last_dir[LG_DIR_MAX] = "";
    char last_name[LG_NAME_MAX] = "";
    uint8_t format = 0;                     // ACTION_SNA_SAVE_TYPE_*
};

static const uint8_t LG_MAGIC = 0x4C;
static const uint8_t LG_VERSION = 1;
static const size_t LG_FILE_MAX = 4 + LG_DIR_MAX + LG_RELOAD_MAX + LG_DIR_MAX + LG_NAME_MAX + 8;

// The bytes of the file: magic, version, flags, format, then the strings reload dir,
// reload name, reload extension, last game dir and last game name, each with its NUL.
// Returns the length.
inline size_t lastgame_encode(const LastGame& g, uint8_t* out) {
    size_t n = 0;
    out[n++] = LG_MAGIC;
    out[n++] = LG_VERSION;
    out[n++] = (g.reload_known ? 1 : 0) | (g.last_known ? 2 : 0) | (g.reload_drive == 1 ? 4 : 0) | (g.last_drive == 1 ? 8 : 0);
    out[n++] = g.format;
    auto put = [&](const char* str) { size_t l = strlen(str) + 1; memcpy(out + n, str, l); n += l; };
    put(g.reload_dir);
    size_t first = strlen(g.reload_name);
    put(g.reload_name);
    put(g.reload_name + first + 1);
    put(g.last_dir);
    put(g.last_name);
    return n;
}

// Reads a file back.  False (and g unchanged) if it is not one this version wrote or is damaged.
inline bool lastgame_decode(const uint8_t* d, size_t len, LastGame& g) {
    if (len < 4 + 5 || d[0] != LG_MAGIC || d[1] != LG_VERSION || (d[2] & ~0x0F) != 0 || d[len - 1] != 0) {
        return false;
    }
    LastGame t;
    t.reload_known = (d[2] & 1) != 0;
    t.last_known = (d[2] & 2) != 0;
    t.reload_drive = (d[2] & 4) ? 1 : 0;
    t.last_drive = (d[2] & 8) ? 1 : 0;
    t.format = d[3];
    size_t pos = 4;
    const char* strs[5];
    size_t lens[5];
    for (int i = 0; i < 5; i++) {
        if (pos >= len) {
            return false;
        }
        strs[i] = reinterpret_cast<const char*>(d + pos);
        lens[i] = strnlen(strs[i], len - pos);
        if (pos + lens[i] >= len) {
            return false;                   // no NUL
        }
        pos += lens[i] + 1;
    }
    if (pos != len || lens[0] >= LG_DIR_MAX || lens[1] + lens[2] + 2 > LG_RELOAD_MAX || lens[3] >= LG_DIR_MAX || lens[4] >= LG_NAME_MAX) {
        return false;
    }
    memcpy(t.reload_dir, strs[0], lens[0] + 1);
    memcpy(t.reload_name, strs[1], lens[1] + 1);
    memcpy(t.reload_name + lens[1] + 1, strs[2], lens[2] + 1);
    memcpy(t.last_dir, strs[3], lens[3] + 1);
    memcpy(t.last_name, strs[4], lens[4] + 1);
    if ((t.reload_known && t.reload_name[0] == '\0') || (t.last_known && t.last_name[0] == '\0')) {
        return false;
    }
    g = t;
    return true;
}

// ---------------------------------------------------------------------------
// The games played recently (E on the menu): the last RECENT_MAX files loaded from the
// Browse Games list or the Recently played list, newest first, kept in a file in the
// internal flash so they survive power off.

static const int RECENT_MAX = 10;
static const size_t RECENT_NAME_MAX = 64;       // a name this long or longer is not kept
static const size_t RECENT_EXT_MAX = 8;

struct RecentEntry {
    uint8_t drive = 0;                          // 0 internal flash, 1 SD card
    char dir[LG_DIR_MAX] = "";                  // same form as sd_cwd
    char name[RECENT_NAME_MAX] = "";
    char ext[RECENT_EXT_MAX] = "";
};

static const uint8_t RECENT_MAGIC = 0x52;
static const uint8_t RECENT_VERSION = 1;
static const size_t RECENT_FILE_MAX = 3 + RECENT_MAX * (1 + LG_DIR_MAX + RECENT_NAME_MAX + RECENT_EXT_MAX);

class RecentList {
public:
    RecentEntry entry[RECENT_MAX];
    int count = 0;

    // Puts the file first, taking it out of where it was. Returns false if it was first
    // already (nothing changed) or cannot be kept (name, extension or folder too long).
    bool add(uint8_t drive, const char* dir, const char* name, const char* ext) {
        drive = drive == 1 ? 1 : 0;
        if (name[0] == '\0' || strlen(name) >= RECENT_NAME_MAX || strlen(ext) >= RECENT_EXT_MAX ||
            strlen(dir) >= LG_DIR_MAX) {
            return false;
        }
        int at = -1;
        for (int i = 0; i < count; i++) {
            if (same(entry[i], drive, dir, name, ext)) {
                at = i;
                break;
            }
        }
        if (at == 0) {
            return false;
        }
        if (at < 0) {
            at = count < RECENT_MAX ? count++ : RECENT_MAX - 1;     // the oldest goes
        }
        for (int i = at; i > 0; i--) {
            entry[i] = entry[i - 1];
        }
        RecentEntry& e = entry[0];
        e.drive = drive;
        snprintf(e.dir, sizeof(e.dir), "%s", dir);
        snprintf(e.name, sizeof(e.name), "%s", name);
        snprintf(e.ext, sizeof(e.ext), "%s", ext);
        return true;
    }

    // magic, version, count, then for each: drive, dir, name, extension (strings with their NULs)
    size_t encode(uint8_t* out) const {
        size_t n = 0;
        out[n++] = RECENT_MAGIC;
        out[n++] = RECENT_VERSION;
        out[n++] = (uint8_t) count;
        for (int i = 0; i < count; i++) {
            out[n++] = entry[i].drive;
            for (const char* s : { entry[i].dir, entry[i].name, entry[i].ext }) {
                size_t l = strlen(s) + 1;
                memcpy(out + n, s, l);
                n += l;
            }
        }
        return n;
    }

    // False (and nothing changed) if the file is not one this version wrote or is damaged
    bool decode(const uint8_t* d, size_t len) {
        if (len < 3 || d[0] != RECENT_MAGIC || d[1] != RECENT_VERSION || d[2] > RECENT_MAX) {
            return false;
        }
        RecentList t;
        size_t pos = 3;
        for (int i = 0; i < d[2]; i++) {
            if (pos >= len) {
                return false;
            }
            RecentEntry& e = t.entry[i];
            e.drive = d[pos++] == 1 ? 1 : 0;
            char* dest[3] = { e.dir, e.name, e.ext };
            size_t room[3] = { LG_DIR_MAX, RECENT_NAME_MAX, RECENT_EXT_MAX };
            for (int k = 0; k < 3; k++) {
                if (pos >= len) {
                    return false;
                }
                size_t l = strnlen(reinterpret_cast<const char*>(d + pos), len - pos);
                if (pos + l >= len || l >= room[k]) {
                    return false;
                }
                memcpy(dest[k], d + pos, l + 1);
                pos += l + 1;
            }
            if (e.name[0] == '\0') {
                return false;
            }
            t.count++;
        }
        if (pos != len) {
            return false;
        }
        *this = t;
        return true;
    }

private:
    static bool same(const RecentEntry& e, uint8_t drive, const char* dir, const char* name, const char* ext) {
        return e.drive == drive && strcmp(e.dir, dir) == 0 && strcasecmp(e.name, name) == 0 &&
               strcasecmp(e.ext, ext) == 0;
    }
};

// The text of the L item on the NMI menu: "Reload: <name>" with the name cut to fit, or
// "No game detected" if there is nothing to reload (none loaded yet, or it is on an SD
// card that is not there).  out holds LABEL_SIZE bytes.
static const size_t LABEL_SIZE = 65;
static const size_t LABEL_NAME_MAX = 56;
inline void reload_label(bool known, bool available, const char* name, char* out) {
    if (!known || !available || name[0] == '\0') {
        snprintf(out, LABEL_SIZE, "No game detected");
        return;
    }
    size_t n = strlen(name);
    if (n > LABEL_NAME_MAX) {
        n = LABEL_NAME_MAX;
    }
    while (n > 0 && name[n - 1] == ' ') {
        n--;
    }
    snprintf(out, LABEL_SIZE, "Reload: %.*s", (int) n, name);
}

// ---- the button
// What a press of the button was.  Called as soon as the button is seen down: down() says
// whether it is down now, wait(ms) waits.  Held for HOLD_MS or more is LONG.  Let go sooner,
// and a second press within DOUBLE_WINDOW_MS of letting go is DOUBLE, otherwise SHORT.  The
// contact is given SETTLE_MS to stop bouncing after each change, so a bounce is not a press.
enum class Press { SHORT, DOUBLE, LONG };
static const uint32_t HOLD_MS = 1000;
static const uint32_t DOUBLE_WINDOW_MS = 350;
static const uint32_t SETTLE_MS = 25;
template <class Down, class Wait>
inline Press classify_press(Down down, Wait wait) {
    uint32_t held = 0;
    while (held < HOLD_MS && down()) {
        wait(1);
        held++;
    }
    if (held >= HOLD_MS) {
        return Press::LONG;
    }
    wait(SETTLE_MS);
    for (uint32_t t = 0; t < DOUBLE_WINDOW_MS; t++) {
        wait(1);
        if (down()) {
            wait(SETTLE_MS);
            if (down()) {
                return Press::DOUBLE;
            }
        }
    }
    return Press::SHORT;
}

}   // namespace extras
