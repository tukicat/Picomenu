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

// ZXDB (the open database of Sinclair software, https://github.com/zxdb/ZXDB, ODbL), searched
// through the ZXInfo API (https://api.zxinfo.dk/v5/, the public API Spectrum Computing links
// to), with its "flat" answers (key=value lines), read a line at a time as they come:
//   a search: hits.0._id=0002259, hits.0.title=Green Beret, hits.0.originalYearOfRelease=1986,
//     hits.0.publishers.0.name=Imagine Software Ltd, hits.0.availability=Available,
//     hits.0.screens.1.type=Loading screen, hits.0.screens.1.scrUrl=/zxdb/sinclair/...scr
//   an entry: title=..., availability=..., releases.0.files.2.path=/pub/sinclair/games/g/
//     GreenBeret.tzx.zip, releases.0.files.2.type=Tape image, ...
// The files are on Spectrum Computing (/zxdb/sinclair/...) and archive.org's mirror of World of
// Spectrum (/pub/sinclair/...), as zxinfo.dk itself fetches them. Only titles whose availability
// is "Available" are offered for download (ZXDB: the others' copyright holders did not allow it,
// or nobody has them). Nothing here touches the network (net.h does), so it is tested on a PC.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include "netget.h"

namespace zxdb {

static const char API[] = "https://api.zxinfo.dk/v5";
static const int PAGE = 10;                 // titles in a page of a search
static const int FILES_MAX = 10;            // tape and snapshot files of an entry kept

struct Hit {
    char id[8];                             // "0002259"
    char title[40];
    char year[6];
    char house[40];                         // the first publisher
    char genre[32];
    char avail[24];                         // "Available" ...
    char scr[80];                           // the loading screen (.scr), or ""
    char instr[80];                         // the instructions (a .txt), or "" (a search for pokes: the .pok)
    uint8_t scr_seen;                       // (reading: the screen the next lines are about)
    bool scr_loading;
    uint8_t ad_seen;                        // (reading: the additional download the lines are about)
    bool ad_instr;
};

struct Search {
    Hit hits[PAGE];
    int n;
    uint32_t total;                         // titles found in all
};

struct File {
    char path[112];
    char kind[24];                          // "Tape image" ...
    uint8_t release, index;                 // (reading: which release's which file)
    bool ok;                                // a tape or a snapshot PicoMenu can use
};

struct Entry {
    Hit info;                               // title, year, software house, genre, availability, loading screen
    File files[FILES_MAX];
    int nfiles;
};

inline bool available(const char* avail) { return strcasecmp(avail, "Available") == 0; }

// The extension of a file PicoMenu loads (tap, tzx, z80, sna, szx), from a path or a name, a .zip
// of one too ("GreenBeret.tzx.zip"): "" if it is none of those
inline const char* game_ext(const char* name) {
    static const char* const EXT[] = { "tap", "tzx", "z80", "sna", "szx" };
    size_t n = strlen(name);
    if (n > 4 && strcasecmp(name + n - 4, ".zip") == 0) n -= 4;
    for (const char* e : EXT) {
        if (n > 4 && name[n - 4] == '.' && strncasecmp(name + n - 3, e, 3) == 0) return e;
    }
    return "";
}

// The search for a title (titles only), page 0, 1 ...
inline bool search_url(const char* term, int page, char* out, size_t size) {
    char enc[200];
    if (!netget::encode(term, enc, sizeof(enc), false)) return false;
    return (size_t) snprintf(out, size, "%s/search/titles/%s?mode=tiny&size=%d&offset=%d&output=flat&contenttype=SOFTWARE",
        API, enc, PAGE, page) < size;
}

inline bool entry_url(const char* id, char* out, size_t size) {
    return (size_t) snprintf(out, size, "%s/entries/%s?mode=compact&output=flat", API, id) < size;
}

// Where a ZXDB path is: Spectrum Computing (/zxdb/sinclair/), archive.org's World of Spectrum
// mirror (/pub/sinclair/), as zxinfo.dk's own pages fetch them. False for anything else.
inline bool file_url(const char* path, char* out, size_t size) {
    char enc[200];
    if (strncmp(path, "/zxdb/sinclair/", 15) == 0) {
        if (!netget::encode(path + 15, enc, sizeof(enc), true)) return false;
        return (size_t) snprintf(out, size, "https://spectrumcomputing.co.uk/zxdb/sinclair/%s", enc) < size;
    }
    if (strncmp(path, "/pub/sinclair/", 14) == 0) {
        if (!netget::encode(path + 14, enc, sizeof(enc), true)) return false;
        return (size_t) snprintf(out, size, "https://archive.org/download/World_of_Spectrum_June_2017_Mirror/"
            "World%%20of%%20Spectrum%%20June%%202017%%20Mirror.zip/World%%20of%%20Spectrum%%20June%%202017%%20Mirror/sinclair/%s", enc) < size;
    }
    return false;
}

// The name of a path's file (after its last /)
inline const char* base_of(const char* path) {
    const char* s = strrchr(path, '/');
    return s != nullptr ? s + 1 : path;
}

// A value as the screen can show it: letters outside plain ASCII become ?
inline void plain(char* out, size_t size, const char* v, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < size; i++) {
        unsigned char c = (unsigned char) v[i];
        if (c >= 0x80) {
            while (i + 1 < n && ((unsigned char) v[i + 1] & 0xC0) == 0x80) i++;    // (one ? for a UTF-8 letter)
            c = '?';
        } else if (c < 32) {
            c = ' ';
        }
        out[k++] = (char) c;
    }
    out[k] = '\0';
}

// The answers, a line at a time (key=value)
class Flat {
public:
    // a search's answer into s (pok: each title's .pok in place of its instructions), or an entry's into e
    void begin(Search* s, bool pok = false) { s_ = s; e_ = nullptr; memset(s, 0, sizeof(*s)); n_ = 0; pok_ = pok; }
    void begin(Entry* e) { e_ = e; s_ = nullptr; memset(e, 0, sizeof(*e)); n_ = 0; pok_ = false; }

    void feed(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; i++) {
            char c = (char) p[i];
            if (c == '\n') {
                line_[n_] = '\0';
                line(line_, n_);
                n_ = 0;
            } else if (c != '\r' && n_ + 1 < sizeof(line_)) {
                line_[n_++] = c;
            }
        }
    }
    void end() {
        if (n_ > 0) { line_[n_] = '\0'; line(line_, n_); n_ = 0; }
        // instructions only if the last additional download looked at was one, and a .txt
        if (s_ != nullptr) for (int i = 0; i < s_->n; i++) tidy(s_->hits[i], pok_);
        if (e_ != nullptr) tidy(e_->info, false);
    }
    static void tidy(Hit& h, bool pok = false) {
        // (a path cut short to fit is no use)
        if (!h.ad_instr || !ext(h.instr, pok ? ".pok" : ".txt") || strlen(h.instr) + 1 >= sizeof(h.instr)) h.instr[0] = '\0';
        if (!h.scr_loading || strlen(h.scr) + 1 >= sizeof(h.scr)) h.scr[0] = '\0';
    }

private:
    static bool num(const char*& k, unsigned& v) {
        if (*k < '0' || *k > '9') return false;
        v = 0;
        while (*k >= '0' && *k <= '9') v = v * 10 + (unsigned) (*k++ - '0');
        return true;
    }
    static bool eat(const char*& k, const char* word) {
        size_t n = strlen(word);
        if (strncmp(k, word, n) != 0) return false;
        k += n;
        return true;
    }

    static bool ext(const char* path, const char* e) {
        size_t n = strlen(path);
        return n > 4 && strcasecmp(path + n - 4, e) == 0;
    }

    // The fields of a title, the same in a search's hit and an entry (after "hits.N." or none)
    void field(Hit& h, const char* k, const char* v, size_t vn) {
        unsigned a, b;
        if (strcmp(k, "_id") == 0) plain(h.id, sizeof(h.id), v, vn);
        else if (strcmp(k, "title") == 0) plain(h.title, sizeof(h.title), v, vn);
        else if (strcmp(k, "originalYearOfRelease") == 0) plain(h.year, sizeof(h.year), v, vn);
        else if (strcmp(k, "availability") == 0) plain(h.avail, sizeof(h.avail), v, vn);
        else if (strcmp(k, "genre") == 0) plain(h.genre, sizeof(h.genre), v, vn);
        else if (eat(k, "publishers.") && num(k, a) && a == 0 && strcmp(k, ".name") == 0) plain(h.house, sizeof(h.house), v, vn);
        else if (eat(k, "screens.") && num(k, b)) {
            // the first screen of type "Loading screen" with a .scr (its lines come in any order)
            if (h.scr[0] != '\0' && h.scr_loading) return;
            if ((unsigned) h.scr_seen != b + 1) {
                h.scr_seen = (uint8_t) (b + 1);
                h.scr_loading = false;
                h.scr[0] = '\0';
            }
            if (strcmp(k, ".type") == 0) h.scr_loading = vn == 14 && strncmp(v, "Loading screen", 14) == 0;
            else if (strcmp(k, ".scrUrl") == 0) plain(h.scr, sizeof(h.scr), v, vn);
        } else if (eat(k, "additionalDownloads.") && num(k, b)) {
            // the first of type "Instructions" that is a .txt (path and type come in any order),
            // or for pokes of type "POK pokes file" that is a .pok
            const char* want = pok_ ? "POK pokes file" : "Instructions";
            if (h.instr[0] != '\0' && h.ad_instr && ext(h.instr, pok_ ? ".pok" : ".txt")) return;
            if ((unsigned) h.ad_seen != b + 1) {
                h.ad_seen = (uint8_t) (b + 1);
                h.ad_instr = false;
                h.instr[0] = '\0';
            }
            if (strcmp(k, ".type") == 0) h.ad_instr = vn == strlen(want) && strncmp(v, want, vn) == 0;
            else if (strcmp(k, ".path") == 0) plain(h.instr, sizeof(h.instr), v, vn);
        }
    }

    // an entry's files (releases.R.files.F.path / .type), the tapes and snapshots kept
    void file_field(const char* k, const char* v, size_t vn) {
        unsigned r, f;
        if (!eat(k, "releases.") || !num(k, r) || !eat(k, ".files.") || !num(k, f)) return;
        File* slot = nullptr;
        for (int i = 0; i < e_->nfiles; i++) {
            if (e_->files[i].release == r && e_->files[i].index == f) slot = &e_->files[i];
        }
        if (slot == nullptr) {
            // a new file: the last slot is reused if its file was not one to keep
            if (e_->nfiles == FILES_MAX && !e_->files[FILES_MAX - 1].ok) e_->nfiles--;
            if (e_->nfiles == FILES_MAX) return;
            slot = &e_->files[e_->nfiles++];
            memset(slot, 0, sizeof(*slot));
            slot->release = (uint8_t) r;
            slot->index = (uint8_t) f;
        }
        if (strcmp(k, ".path") == 0) {
            plain(slot->path, sizeof(slot->path), v, vn);
            slot->ok = game_ext(slot->path)[0] != '\0' && strlen(slot->path) + 1 < sizeof(slot->path);
        } else if (strcmp(k, ".type") == 0) {
            plain(slot->kind, sizeof(slot->kind), v, vn);
        }
    }

    void line(const char* l, size_t n) {
        const char* eq = (const char*) memchr(l, '=', n);
        if (eq == nullptr) return;              // (a value that ran onto another line)
        char key[80];
        size_t kn = (size_t) (eq - l);
        if (kn >= sizeof(key)) return;
        memcpy(key, l, kn);
        key[kn] = '\0';
        const char* v = eq + 1;
        size_t vn = n - kn - 1;
        const char* k = key;
        if (s_ != nullptr) {
            unsigned i;
            if (strcmp(k, "total.value") == 0) {
                unsigned t;
                const char* p = v;
                if (num(p, t)) s_->total = t;
            } else if (eat(k, "hits.") && num(k, i) && *k == '.' && i < (unsigned) PAGE) {
                if ((int) i >= s_->n) s_->n = (int) i + 1;
                field(s_->hits[i], k + 1, v, vn);
            }
        } else if (e_ != nullptr) {
            if (strncmp(k, "releases.", 9) == 0) file_field(k, v, vn);
            else field(e_->info, k, v, vn);
        }
    }

    Search* s_ = nullptr;
    Entry* e_ = nullptr;
    bool pok_ = false;
    char line_[300];
    size_t n_ = 0;
};

// The files kept (ok) moved to the front, in order; how many
inline int tidy_files(Entry& e) {
    int k = 0;
    for (int i = 0; i < e.nfiles; i++) {
        if (e.files[i].ok) e.files[k++] = e.files[i];
    }
    e.nfiles = k;
    return k;
}

// A game's name as a search: "Green_Beret_1986ImagineSpeedLock_2" -> "Green Beret" (_ as a
// space, up to a year, a bracket, or " - ")
inline void name_to_search(const char* name, char* out, size_t size) {
    size_t k = 0;
    for (const char* p = name; *p != '\0' && k + 1 < size; p++) {
        if (*p == '(' || *p == '[') break;
        if (p[0] >= '1' && p[0] <= '2' && p[1] >= '0' && p[1] <= '9' && p[2] >= '0' && p[2] <= '9' && p[3] >= '0' && p[3] <= '9' &&
                (p == name || p[-1] == '_' || p[-1] == ' ')) break;     // a year: 1982 ...
        if (p[0] == ' ' && p[1] == '-' && p[2] == ' ') break;
        out[k++] = *p == '_' ? ' ' : *p;
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = '\0';
}

}   // namespace zxdb
