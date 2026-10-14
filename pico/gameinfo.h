// The controls and instructions of a game: the file "Instructions.txt" (or "Instructions") in
// the folder "Scraper" of the SD card (the root if there is none there), and the screen that shows them (V on a game, and I on the menu).
// Nothing here touches the hardware, so it is tested on a PC.
//
// The file is plain text with one entry for each game, like this:
//
//   Skool Daze
//   Controls
//   O or Cursor Left: Go left
//   P or Cursor Right: Go right
//   Instructions
//   Hit all the shields ...
//
// * An entry starts with the game's name on a line of its own, followed by the lines
//   Controls and Instructions. The name is the game's file name without .sna, .z80,
//   .tap and so on (capitals do not matter). A game saved with Quick save ("<game> Save
//   Game") finds the entry of <game>. The next game's name ends the entry.
// * The name can also be written [Name] (then the headings can be left out, and the text
//   is the instructions). Several [Name] lines in a row share one entry.
// * Controls and Instructions can have a colon, and text after it on the same line.
// * Right after the game's name (or its [Name] lines) there can be the lines
//     Software house: Ocean Software
//     Year: 1984
//   (also Publisher: and Year released:). They are shown above Controls, next to the
//   picture. Use the name line, then these, then Controls or Instructions. For a game
//   with only these use the [Name] form.
// * Your own notes are not in this file: they are kept per game in the hidden folder
//   "Game Notes" (see notes.h) and shown as the last section, My notes. So are details
//   typed in on the Spectrum (M, then H, Y, C or I; see Details below): they replace the
//   same parts of the entry here.
// * Each line of the file is a line on the screen (too long lines wrap at a space).
//   A blank line is kept (one at a time). Lines starting with ; are ignored. A line is
//   kept up to 700 letters.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

namespace gameinfo {

static const size_t CONTROLS_MAX = 512;         // letters kept of the controls
static const size_t TEXT_MAX = 2048;            // letters kept of the instructions
static const size_t GAME_MAX = 64;
static const size_t HOUSE_MAX = 40;             // letters kept of the software house
static const size_t YEAR_MAX = 8;               // and of the year
static const size_t NOTES_SHOW_MAX = 1024;      // letters kept of the notes to show
static const size_t TAPE_MAX = 640;             // letters kept of the tape summary
static const size_t VERDICT_MAX = 15;           // and of what the Picoface says about loading it
static const size_t LINE_MAX = 700;             // letters kept of a line of the file

// Screen layout (characters)
static const int THUMB_ROW = 1, THUMB_COL = 0, THUMB_ROWS = 12, THUMB_COLS = 16;
static const int CTRL_COL = 17, CTRL_WIDTH = 15, CTRL_HEAD_ROW = 1, CTRL_FIRST_ROW = 2, CTRL_ROWS = 11;
static const int TEXT_WIDTH = 32, TEXT_HEAD_ROW = 13, TEXT_FIRST_ROW = 14, TEXT_ROWS = 9;
static const int MORE_ROWS = 22;                // lines on the pages after the first (rows 1-22)

static const uint8_t ATTR_BANNER = 0x4F, ATTR_TEXT = 0x47, ATTR_HEADING = 0x46, ATTR_KEYS = 0x45;
static const uint8_t ATTR_DIM = 0x41;

inline const char* skip_blanks(const char* s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Wrapping text to a width. next() gives the lines one at a time.
class Wrap {
public:
    Wrap(const char* text, int width) : p_(text), width_(width) {}

    // out holds width + 1 letters. False at the end of the text.
    bool next(char* out) {
        if (*p_ == '\0') {
            return false;
        }
        int len = 0;
        while (p_[len] != '\0' && p_[len] != '\n' && len < width_) {
            len++;
        }
        int take = len;
        bool newline = p_[len] == '\n';
        if (!newline && p_[len] != '\0' && p_[len] != ' ') {
            // the line is full in the middle of a word: break at the last space if there is one
            int sp = len;
            while (sp > 0 && p_[sp] != ' ') {
                sp--;
            }
            // a space only after the indent is no place to break
            int first = 0;
            while (first < len && p_[first] == ' ') {
                first++;
            }
            if (sp > first) {
                take = sp;
            }
        }
        int n = take;
        while (n > 0 && p_[n - 1] == ' ') {
            n--;
        }
        memcpy(out, p_, (size_t) n);
        out[n] = '\0';
        p_ += take;
        if (*p_ == '\n') {
            p_++;
        } else {
            while (*p_ == ' ') {
                p_++;                   // the spaces where it broke
            }
            if (*p_ == '\n') {
                p_++;
            }
        }
        return true;
    }

    // What has not been given out yet
    const char* rest() const { return p_; }

    static int count(const char* text, int width) {
        Wrap w(text, width);
        char line[40];
        int n = 0;
        while (w.next(line)) {
            n++;
        }
        return n;
    }

private:
    const char* p_;
    int width_;
};

// ---------------------------------------------------------------------------
// Details typed in on the Spectrum (M, then H Y C or I on the game's information): the
// software house, the year, the controls and the instructions of a game. They are kept in
// a small file for the game in the Game Notes folder, as <game>.info.txt, in the same
// words as an entry of the Instructions file, without the game's name:
//
//   Software house: Microsphere
//   Year: 1985
//   Controls
//   Q/A up, down
//   Instructions
//   Hit the shields ...
//
// Only what was edited is in it. A part that is in it (even an empty one, which hides what
// the Instructions file says) replaces that part of the game's entry; the rest still comes
// from the Instructions file. A PC can read and change this file too.
inline bool details_key(const char* t, const char** value, bool* year) {
    static const struct { const char* key; bool year; } KEYS[] = {
        { "software house", false }, { "publisher", false }, { "year released", true }, { "year", true } };
    for (const auto& k : KEYS) {
        size_t n = strlen(k.key);
        if (strncasecmp(t, k.key, n) != 0) {
            continue;
        }
        const char* p = skip_blanks(t + n);
        if (*p != ':') {
            continue;
        }
        *value = skip_blanks(p + 1);
        *year = k.year;
        return true;
    }
    return false;
}

inline bool details_heading(const char* t, const char* word, const char** rest) {
    size_t n = strlen(word);
    if (strncasecmp(t, word, n) != 0) {
        return false;
    }
    const char* p = skip_blanks(t + n);
    if (*p == '\0' || *p == '\n' || *p == '\r') {      // the end of the line, also in text that goes on
        *rest = p;
        return true;
    }
    if (*p != ':') {
        return false;
    }
    *rest = skip_blanks(p + 1);
    return true;
}

// A line of a section that would be taken for a heading or for Year: and so on
inline bool details_special(const char* line) {
    const char* t = skip_blanks(line);
    const char* v;
    bool y;
    return *t == ';' || *t == '[' || details_key(t, &v, &y) || details_heading(t, "controls", &v) ||
           details_heading(t, "instructions", &v);
}

class Details {
public:
    char house[HOUSE_MAX + 1];
    char year[YEAR_MAX + 1];
    char controls[CONTROLS_MAX + 2];
    char text[TEXT_MAX + 2];
    size_t ncontrols = 0, ntext = 0;
    bool has_house = false, has_year = false, has_controls = false, has_text = false;

    Details() { clear(); }

    void clear() {
        house[0] = year[0] = controls[0] = text[0] = '\0';
        ncontrols = ntext = 0;
        has_house = has_year = has_controls = has_text = false;
    }

    bool any() const { return has_house || has_year || has_controls || has_text; }

    // The bytes of a file (lines ending in \n, \r\n or \r)
    void parse(const char* data, size_t n) {
        clear();
        enum { NONE, CONTROLS, TEXT } sec = NONE;
        char* line = line_;             // in the object, not on the Pico's small stack
        size_t len = 0;
        bool after_cr = false;
        for (size_t i = 0; i <= n; i++) {
            char c = i < n ? data[i] : '\n';
            if (i < n && c == '\n' && after_cr) {
                after_cr = false;
                continue;
            }
            after_cr = i < n && c == '\r';
            if (c != '\n' && c != '\r') {
                if (len < sizeof(line_) - 1) {
                    line[len++] = c == '\t' ? ' ' : c;
                }
                continue;
            }
            if (i == n && len == 0) {
                break;
            }
            line[len] = '\0';
            len = 0;
            while (strlen(line) > 0 && line[strlen(line) - 1] == ' ') {
                line[strlen(line) - 1] = '\0';
            }
            const char* t = skip_blanks(line);
            if (*t == ';') {
                continue;
            }
            const char* rest;
            bool is_year;
            if (details_key(t, &rest, &is_year)) {
                char* dest = is_year ? year : house;
                size_t cap = is_year ? YEAR_MAX : HOUSE_MAX;
                size_t k = 0;
                for (; rest[k] != '\0' && k < cap; k++) {
                    unsigned char ch = (unsigned char) rest[k];
                    dest[k] = (ch >= 32 && ch < 127) ? (char) ch : '?';
                }
                while (k > 0 && dest[k - 1] == ' ') {
                    k--;
                }
                dest[k] = '\0';
                (is_year ? has_year : has_house) = true;
                continue;
            }
            if (details_heading(t, "controls", &rest)) {
                sec = CONTROLS;
                has_controls = true;
                ncontrols = 0;
                controls[0] = '\0';
                if (*rest != '\0') {
                    append(controls, ncontrols, CONTROLS_MAX, rest);
                }
                continue;
            }
            if (details_heading(t, "instructions", &rest)) {
                sec = TEXT;
                has_text = true;
                ntext = 0;
                text[0] = '\0';
                if (*rest != '\0') {
                    append(text, ntext, TEXT_MAX, rest);
                }
                continue;
            }
            if (sec == NONE) {
                if (*t == '\0') {
                    continue;
                }
                sec = TEXT;                 // text with no heading is the instructions
                has_text = true;
            }
            if (line[0] == '*' && line[1] == ' ' && details_special(line + 2)) {
                memmove(line, line + 2, strlen(line + 2) + 1);      // the "* " serialize() put in front
            }
            if (sec == CONTROLS) {
                append(controls, ncontrols, CONTROLS_MAX, line);
            } else {
                append(text, ntext, TEXT_MAX, line);
            }
        }
        while (ncontrols >= 2 && controls[ncontrols - 1] == '\n' && controls[ncontrols - 2] == '\n') {
            controls[--ncontrols] = '\0';
        }
        while (ntext >= 2 && text[ntext - 1] == '\n' && text[ntext - 2] == '\n') {
            text[--ntext] = '\0';
        }
    }

    // The file's bytes: what is there, headings first. Returns the length (0: nothing to keep).
    size_t serialize(char* out, size_t cap) const {
        size_t n = 0;
        auto put = [&](const char* s, size_t k) {
            if (n + k < cap) {
                memcpy(out + n, s, k);
                n += k;
            }
        };
        auto line = [&](const char* a, const char* b) {
            put(a, strlen(a));
            put(b, strlen(b));
            put("\n", 1);
        };
        if (has_house) line("Software house: ", house);
        if (has_year) line("Year: ", year);
        auto body = [&](const char* heading, const char* s) {
            line(heading, "");
            while (*s != '\0') {
                const char* e = strchr(s, '\n');
                size_t k = e != nullptr ? (size_t) (e - s) : strlen(s);
                if (k > 0 && details_special(s)) {
                    put("* ", 2);       // not to be taken for a heading
                }
                put(s, k);
                put("\n", 1);
                s += k + (e != nullptr ? 1 : 0);
            }
        };
        if (has_controls) body("Controls", controls);
        if (has_text) body("Instructions", text);
        if (n < cap) out[n] = '\0';
        return n;
    }

    // Replaces a part with text of lines ending in \n (an empty text keeps the part, empty)
    void set_house(const char* s) { set_line(house, HOUSE_MAX, s); has_house = true; }
    void set_year(const char* s) { set_line(year, YEAR_MAX, s); has_year = true; }
    void set_controls(const char* s) { ncontrols = copy(controls, CONTROLS_MAX, s); has_controls = true; }
    void set_text(const char* s) { ntext = copy(text, TEXT_MAX, s); has_text = true; }

private:
    char line_[LINE_MAX + 1];           // parse()'s line

    static void set_line(char* dest, size_t cap, const char* s) {
        size_t k = 0;
        for (; s[k] != '\0' && s[k] != '\n' && k < cap; k++) {
            unsigned char ch = (unsigned char) s[k];
            dest[k] = (ch >= 32 && ch < 127) ? (char) ch : '?';
        }
        while (k > 0 && dest[k - 1] == ' ') {
            k--;
        }
        dest[k] = '\0';
    }

    static size_t copy(char* dest, size_t cap, const char* s) {
        size_t n = 0;
        while (*s != '\0' && n < cap) {
            unsigned char ch = (unsigned char) *s++;
            dest[n++] = ch == '\n' ? '\n' : ((ch >= 32 && ch < 127) ? (char) ch : '?');
        }
        dest[n] = '\0';
        return n;
    }

    // One line of a section, as Info does: no blank at the start, not two together, a line
    // that does not fit is left out
    static void append(char* buf, size_t& n, size_t cap, const char* s) {
        if (*s == '\0') {
            if (n > 0 && !(n >= 2 && buf[n - 1] == '\n' && buf[n - 2] == '\n') && n + 1 <= cap) {
                buf[n++] = '\n';
                buf[n] = '\0';
            }
            return;
        }
        size_t len = strlen(s);
        if (n + len + 1 > cap) {
            return;
        }
        for (size_t i = 0; i < len; i++) {
            unsigned char ch = (unsigned char) s[i];
            buf[n++] = (ch >= 32 && ch < 127) ? (char) ch : '?';
        }
        buf[n++] = '\n';
        buf[n] = '\0';
    }
};

// ---------------------------------------------------------------------------
// The entry of one game
class Info {
public:
    char controls[CONTROLS_MAX + 2];
    char text[TEXT_MAX + 2];
    char house[HOUSE_MAX + 1];          // "Software house:", "" if not given
    char year[YEAR_MAX + 1];            // "Year:"
    char notes[NOTES_SHOW_MAX + 2];     // the game's notes (set_notes), lines ending in \n
    char tape[TAPE_MAX + 2];            // the summary of a .tap or .tzx (set_tape), lines ending in \n
    char verdict[VERDICT_MAX + 1];      // can the Picoface load it: Yes, Maybe, Probably not, No
    size_t ncontrols = 0;
    size_t ntext = 0;
    size_t nnotes = 0;
    size_t ntape = 0;
    bool found = false;                 // the game has an entry

    // True if there is something to show from the Instructions file
    bool any() const { return found && (ncontrols > 0 || ntext > 0 || house[0] != '\0' || year[0] != '\0'); }

    // The details typed in on the Spectrum (Details): each part they have replaces the entry's
    void apply(const Details& d) {
        if (d.has_house) {
            memcpy(house, d.house, sizeof(house));
        }
        if (d.has_year) {
            memcpy(year, d.year, sizeof(year));
        }
        if (d.has_controls) {
            memcpy(controls, d.controls, d.ncontrols + 1);
            ncontrols = d.ncontrols;
        }
        if (d.has_text) {
            memcpy(text, d.text, d.ntext + 1);
            ntext = d.ntext;
        }
        if (d.any()) {
            found = true;
        }
    }

    // The notes of the game: text of lines ending in \n or \r\n. Letters the screen cannot
    // show become ?, a tab a blank, blank lines at the end are dropped.
    void set_notes(const char* data, size_t n) {
        nnotes = 0;
        notes[0] = '\0';
        bool after_cr = false;
        for (size_t i = 0; i < n && nnotes < NOTES_SHOW_MAX; i++) {
            char c = data[i];
            if (c == '\n' && after_cr) {
                after_cr = false;
                continue;
            }
            after_cr = c == '\r';
            if (c == '\r') {
                c = '\n';
            }
            if (c == '\t') {
                c = ' ';
            }
            if (c != '\n' && ((unsigned char) c < 32 || (unsigned char) c > 126)) {
                c = '?';
            }
            notes[nnotes++] = c;
        }
        while (nnotes > 0 && (notes[nnotes - 1] == '\n' || notes[nnotes - 1] == ' ')) {
            nnotes--;
        }
        notes[nnotes] = '\0';
        if (nnotes > 0) {
            notes[nnotes++] = '\n';
            notes[nnotes] = '\0';
        }
    }

    // The summary of a tape (lines ending in \n) and the verdict on loading it
    void set_tape(const char* text, const char* say) {
        ntape = strnlen(text, TAPE_MAX);
        memcpy(tape, text, ntape);
        tape[ntape] = '\0';
        size_t v = strnlen(say, VERDICT_MAX);
        memcpy(verdict, say, v);
        verdict[v] = '\0';
    }

    // Reads the file with f.read(buf, n) (the number of bytes got) and takes the entry
    // for game (a name without its extension).
    template <class File>
    bool load(File& f, const char* game) {
        reset(game);
        uint8_t buf[64];
        char line[LINE_MAX + 1];
        size_t len = 0;
        bool first = true;
        bool after_cr = false;
        bool done = false;
        for (;;) {
            int got = f.read(buf, sizeof(buf));
            if (got <= 0) {
                break;
            }
            for (int i = 0; i < got && !done; i++) {
                if (first) {
                    // a byte order mark from a Windows editor
                    if (bom_ < 3 && buf[i] == (bom_ == 0 ? 0xEF : bom_ == 1 ? 0xBB : 0xBF)) {
                        bom_++;
                        continue;
                    }
                    if (bom_ != 3) {
                        bom_ = 0;
                    }
                    first = false;
                }
                char c = (char) buf[i];
                if (c == '\n' && after_cr) {
                    after_cr = false;       // the end of a Windows line, already taken
                    continue;
                }
                after_cr = c == '\r';
                if (c == '\n' || c == '\r') {
                    line[len] = '\0';
                    done = take_line(line);
                    len = 0;
                }
                else if (len < sizeof(line) - 1) {
                    line[len++] = c == '\t' ? ' ' : c;
                }
            }
            if (done) {
                break;
            }
        }
        if (!done && len > 0) {
            line[len] = '\0';
            take_line(line);
        }
        finish();
        return any();
    }

private:
    enum Section { NONE, CONTROLS, TEXT };
    char game_[GAME_MAX + 1];
    size_t short_len_ = 0;              // the game's name without " Save Game"
    bool bracket_ = false;              // the last lines were [Name] lines and nothing since
    bool matches_ = false;
    Section section_ = NONE;
    bool have_ctrl_ = false, have_text_ = false;
    char pend_[LINE_MAX + 1];                    // the last line of text, which may be the next game's name
    bool has_pend_ = false;
    int blanks_ = 0;                    // blank lines after it (at most 1 matters)
    int bom_ = 0;
    char pm_house_[HOUSE_MAX + 1];      // lines after a name that may start the wanted entry
    char pm_year_[YEAR_MAX + 1];

    // "Software house: X", "Publisher: X", "Year: X", "Year released: X". Returns true if
    // the line was one (its text is kept for the entry the name above it starts).
    bool meta_line(const char* t) {
        static const struct { const char* key; bool year; } KEYS[] = {
            { "software house", false }, { "publisher", false }, { "year released", true }, { "year", true } };
        for (const auto& k : KEYS) {
            size_t n = strlen(k.key);
            if (strncasecmp(t, k.key, n) != 0) {
                continue;
            }
            const char* p = skip_blanks(t + n);
            if (*p != ':') {
                continue;
            }
            p = skip_blanks(p + 1);
            if (!(bracket_ || has_pend_)) {
                return false;           // not after a name: an ordinary line
            }
            char* dest = bracket_ ? (k.year ? year : house) : (k.year ? pm_year_ : pm_house_);
            size_t cap = k.year ? YEAR_MAX : HOUSE_MAX;
            if (bracket_ && !matches_) {
                return true;            // some other game's
            }
            size_t len = 0;
            for (; p[len] != '\0' && len < cap; len++) {
                unsigned char ch = (unsigned char) p[len];
                dest[len] = (ch >= 32 && ch < 127) ? (char) ch : '?';
            }
            while (len > 0 && dest[len - 1] == ' ') {
                len--;
            }
            dest[len] = '\0';
            return true;
        }
        return false;
    }

    void reset(const char* game) {
        set_name(game);
        controls[0] = text[0] = '\0';
        house[0] = year[0] = pm_house_[0] = pm_year_[0] = '\0';
        ncontrols = ntext = nnotes = 0;
        notes[0] = '\0';
        found = false;
        bracket_ = false;
        matches_ = false;
        section_ = NONE;
        have_ctrl_ = have_text_ = false;
        has_pend_ = false;
        blanks_ = 0;
        bom_ = 0;
    }

    void set_name(const char* game) {
        size_t n = strlen(game);
        if (n > GAME_MAX) {
            n = GAME_MAX;
        }
        memcpy(game_, game, n);
        game_[n] = '\0';
        while (n > 0 && game_[n - 1] == ' ') {
            game_[--n] = '\0';
        }
        static const char SUFFIX[] = " Save Game";
        static const size_t SUFFIX_LEN = sizeof(SUFFIX) - 1;
        short_len_ = n;
        size_t m = n;           // a numbered save, " Save Game 2", is the same game
        size_t digits = 0;
        while (digits < m && game_[m - 1 - digits] >= '0' && game_[m - 1 - digits] <= '9') {
            digits++;
        }
        if (digits > 0 && digits <= 3 && m > digits + 1 + SUFFIX_LEN && game_[m - digits - 1] == ' ' &&
                strncasecmp(game_ + m - digits - 1 - SUFFIX_LEN, SUFFIX, SUFFIX_LEN) == 0) {
            m -= digits + 1;
        }
        if (m > SUFFIX_LEN && strncasecmp(game_ + m - SUFFIX_LEN, SUFFIX, SUFFIX_LEN) == 0) {
            short_len_ = m - SUFFIX_LEN;
        }
    }

    bool name_matches(const char* name, size_t len) const {
        // name[0..len) without the blanks at both ends
        while (len > 0 && name[0] == ' ') {
            name++;
            len--;
        }
        while (len > 0 && name[len - 1] == ' ') {
            len--;
        }
        if (len == 0) {
            return false;
        }
        size_t full = strlen(game_);
        return (len == full && strncasecmp(name, game_, len) == 0) ||
               (len == short_len_ && strncasecmp(name, game_, len) == 0);
    }

    // A heading: the word alone on the line, or followed by a colon (and then perhaps
    // text). Returns true and the text after it.
    static bool heading(const char* s, const char* word, const char** rest) {
        size_t n = strlen(word);
        if (strncasecmp(s, word, n) != 0) {
            return false;
        }
        const char* p = skip_blanks(s + n);
        if (*p == '\0') {
            *rest = p;
            return true;
        }
        if (*p != ':') {
            return false;
        }
        *rest = skip_blanks(p + 1);
        return true;
    }

    void add_line(Section sec, const char* s) {
        char* buf = sec == CONTROLS ? controls : text;
        size_t& n = sec == CONTROLS ? ncontrols : ntext;
        size_t cap = sec == CONTROLS ? CONTROLS_MAX : TEXT_MAX;
        if (*s == '\0') {
            // a blank line: not at the start, and not two together
            if (n > 0 && !(n >= 2 && buf[n - 1] == '\n' && buf[n - 2] == '\n') && n + 1 <= cap) {
                buf[n++] = '\n';
                buf[n] = '\0';
            }
            return;
        }
        size_t len = strlen(s);
        if (n + len + 1 > cap) {
            return;                     // no room: what fits is kept
        }
        for (size_t i = 0; i < len; i++) {
            unsigned char ch = (unsigned char) s[i];
            buf[n++] = (ch >= 32 && ch < 127) ? (char) ch : '?';
        }
        buf[n++] = '\n';
        buf[n] = '\0';
    }

    // The held line (and the blank after it) become text of the entry
    void flush_pending() {
        if (has_pend_ && matches_) {
            add_line(section_ == CONTROLS ? CONTROLS : TEXT, pend_);
            if (blanks_ > 0) {
                add_line(section_ == CONTROLS ? CONTROLS : TEXT, "");
            }
        }
        has_pend_ = false;
        blanks_ = 0;
    }

    void start_entry(const char* name, size_t len) {
        matches_ = name_matches(name, len);
        if (matches_) {
            found = true;
            memcpy(house, pm_house_, sizeof(house));
            memcpy(year, pm_year_, sizeof(year));
        }
        section_ = NONE;
        have_ctrl_ = have_text_ = false;
    }

    // Takes a heading. Returns true if it starts the entry after the wanted one.
    bool take_heading(Section sec, const char* rest) {
        bool seen = sec == CONTROLS ? have_ctrl_ : have_text_;
        bool fresh = !have_ctrl_ && !have_text_ && !bracket_;
        if (fresh || seen || (have_ctrl_ && have_text_)) {
            // the line before is the name of a game, which starts an entry
            if (!fresh && matches_) {
                has_pend_ = false;          // the name of the next game, not part of this one
                return true;
            }
            if (has_pend_) {
                start_entry(pend_, strlen(pend_));
            } else {
                start_entry("", 0);
            }
            has_pend_ = false;
            blanks_ = 0;
        } else {
            flush_pending();
        }
        bracket_ = false;
        section_ = sec;
        if (sec == CONTROLS) {
            have_ctrl_ = true;
        } else {
            have_text_ = true;
        }
        if (matches_ && *rest != '\0') {
            add_line(sec, rest);
        }
        return false;
    }

    // true when the entry has been read
    bool take_line(char* line) {
        size_t len = strlen(line);
        while (len > 0 && line[len - 1] == ' ') {
            line[--len] = '\0';
        }
        const char* t = skip_blanks(line);
        if (*t == ';') {
            return false;
        }
        if (*t == '[') {
            const char* close = strchr(t, ']');
            if (close != nullptr) {
                if (!bracket_) {
                    flush_pending();
                    if (matches_ && (ncontrols > 0 || ntext > 0 || house[0] != '\0' || year[0] != '\0')) {
                        return true;    // the next entry
                    }
                    matches_ = false;
                    section_ = NONE;
                    have_ctrl_ = have_text_ = false;
                    bracket_ = true;
                }
                if (name_matches(t + 1, (size_t) (close - t - 1))) {
                    matches_ = true;
                    found = true;
                }
                return false;
            }
        }
        if (meta_line(t)) {
            return false;
        }
        const char* rest;
        if (heading(t, "controls", &rest)) {
            return take_heading(CONTROLS, rest);
        }
        if (heading(t, "instructions", &rest)) {
            return take_heading(TEXT, rest);
        }
        if (*t == '\0') {
            if (has_pend_) {
                blanks_ = 1;
            }
            return false;
        }
        // a line of text (or the name of the next game)
        flush_pending();
        bracket_ = false;
        size_t n = strlen(line);
        if (n >= sizeof(pend_)) {
            n = sizeof(pend_) - 1;
        }
        memcpy(pend_, line, n);
        pend_[n] = '\0';
        has_pend_ = true;
        pm_house_[0] = pm_year_[0] = '\0';
        return false;
    }

    // no blank lines at the end of a section
    void finish() {
        flush_pending();
        while (ncontrols >= 2 && controls[ncontrols - 1] == '\n' && controls[ncontrols - 2] == '\n') {
            controls[--ncontrols] = '\0';
        }
        while (ntext >= 2 && text[ntext - 1] == '\n' && text[ntext - 2] == '\n') {
            text[--ntext] = '\0';
        }
    }
};

// ---------------------------------------------------------------------------
// The picture: the screen (6144 bytes of pixels and 768 of colour) at half the size, which
// is THUMB_COLS x THUMB_ROWS characters at (row0, col0) of dst (a 6912 byte screen).
// Each new character takes the colours of the one of its four old ones that has the most
// detail; each pixel is ink where at least two of its four old ones are nearer to that ink
// than to its paper.

inline int pixel_address(int y, int xbyte) {
    return ((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2) | xbyte;
}

inline int colour_distance(int a, int b) {
    int d = a ^ b;                      // the bits are blue, red and green
    return (d & 1) + ((d >> 1) & 1) + ((d >> 2) & 1);
}

inline int ink_count(const uint8_t* src, int cellrow, int cellcol) {
    int n = 0;
    for (int line = 0; line < 8; line++) {
        n += __builtin_popcount(src[pixel_address(cellrow * 8 + line, cellcol)]);
    }
    return n;
}

inline void draw_thumbnail(const uint8_t* src, uint8_t* dst, int row0, int col0) {
    for (int orow = 0; orow < THUMB_ROWS; orow++) {
        for (int ocol = 0; ocol < THUMB_COLS; ocol++) {
            // which of the four old characters has the most detail (ink not 0 and not 64)
            int best = -1, bestscore = -1;
            for (int k = 0; k < 4; k++) {
                int r = orow * 2 + (k >> 1), c = ocol * 2 + (k & 1);
                int n = ink_count(src, r, c);
                int score = (n == 0 || n == 64) ? 0 : 1 + (n < 32 ? n : 64 - n);
                if (score > bestscore) {
                    bestscore = score;
                    best = k;
                }
            }
            int br = orow * 2 + (best >> 1), bc = ocol * 2 + (best & 1);
            uint8_t attr = src[6144 + br * 32 + bc];
            int ink = attr & 7, paper = (attr >> 3) & 7;
            for (int line = 0; line < 8; line++) {
                uint8_t out = 0;
                for (int px = 0; px < 8; px++) {
                    int on = 0;
                    for (int k = 0; k < 4; k++) {
                        int sy = orow * 16 + line * 2 + (k >> 1);
                        int sx = ocol * 16 + px * 2 + (k & 1);
                        int cr = sy >> 3, cc = sx >> 3;
                        uint8_t a = src[6144 + cr * 32 + cc];
                        bool isink = (src[pixel_address(sy, sx >> 3)] >> (7 - (sx & 7))) & 1;
                        int colour = isink ? (a & 7) : ((a >> 3) & 7);
                        if (colour_distance(colour, ink) < colour_distance(colour, paper) ||
                            (colour_distance(colour, ink) == colour_distance(colour, paper) && isink)) {
                            on++;
                        }
                    }
                    out = (uint8_t) ((out << 1) | (on >= 2 ? 1 : 0));
                }
                dst[pixel_address((row0 + orow) * 8 + line, col0 + ocol)] = out;
            }
            dst[6144 + (row0 + orow) * 32 + col0 + ocol] = attr & 0x7F;     // no flash
        }
    }
}

// ---------------------------------------------------------------------------
// The text of the screen

// The software house and the year take the first rows of the controls' column
inline int meta_rows(const Info& info) { return (info.house[0] != '\0' ? 1 : 0) + (info.year[0] != '\0' ? 1 : 0); }
inline int ctrl_rows(const Info& info) { return CTRL_ROWS - meta_rows(info); }

// What is left of a section after the lines of its place on the first page
inline const char* leftover(const char* text, int width, int rows) {
    Wrap w(text, width);
    char line[40];
    for (int i = 0; i < rows; i++) {
        if (!w.next(line)) {
            break;
        }
    }
    return w.rest();
}

// The number of pages
inline int page_count(const Info& info) {
    int more = 0;
    if (info.any()) {
        const char* c = leftover(info.controls, CTRL_WIDTH, ctrl_rows(info));
        if (*c != '\0') {
            more += 1 + Wrap::count(c, TEXT_WIDTH);
        }
        const char* t = leftover(info.text, TEXT_WIDTH, TEXT_ROWS);
        if (*t != '\0') {
            more += 1 + Wrap::count(t, TEXT_WIDTH);
        }
    }
    if (info.ntape > 0) {
        more += 1 + Wrap::count(info.tape, TEXT_WIDTH);
    }
    if (info.nnotes > 0) {
        more += 1 + Wrap::count(info.notes, TEXT_WIDTH);
    }
    return 1 + (more + MORE_ROWS - 1) / MORE_ROWS;
}

// Draws page of the text with put(row, col, text, length, attribute). The title row, the
// picture and the key row are the caller's.
template <class Put>
void draw_text(const Info& info, int page, Put put) {
    char line[40];
    auto emit = [&](int row, int col, const char* s, uint8_t attr) {
        int n = (int) strlen(s);
        if (n > 0) {
            put(row, col, s, n, attr);
        }
    };
    if (page == 0) {
        if (!info.any()) {
            emit(TEXT_FIRST_ROW, 0, "No information available", ATTR_TEXT);
            if (info.ntape > 0) {                           // the tape's summary is on the next page
                char say[48];
                snprintf(say, sizeof(say), "PicoMenu can load: %s", info.verdict);
                emit(TEXT_FIRST_ROW + 2, 0, say, ATTR_HEADING);
                emit(TEXT_FIRST_ROW + 3, 0, "N-Tape details", ATTR_DIM);
            }
            return;
        }
        int row = CTRL_HEAD_ROW;
        if (info.house[0] != '\0') {
            char shown[CTRL_WIDTH + 1];
            size_t n = strnlen(info.house, CTRL_WIDTH);
            memcpy(shown, info.house, n);
            shown[n] = '\0';
            emit(row++, CTRL_COL, shown, ATTR_HEADING);
        }
        if (info.year[0] != '\0') {
            char shown[CTRL_WIDTH + 1];
            size_t n = strnlen(info.year, YEAR_MAX);
            memcpy(shown, info.year, n);
            shown[n] = '\0';
            emit(row++, CTRL_COL, shown, ATTR_TEXT);
        }
        if (info.ncontrols > 0) {
            emit(row, CTRL_COL, "Controls", ATTR_HEADING);
            Wrap w(info.controls, CTRL_WIDTH);
            for (int i = 0; i < ctrl_rows(info) && w.next(line); i++) {
                emit(row + 1 + i, CTRL_COL, line, ATTR_TEXT);
            }
        }
        if (info.ntext > 0) {
            emit(TEXT_HEAD_ROW, 0, "Instructions", ATTR_HEADING);
            Wrap w(info.text, TEXT_WIDTH);
            for (int i = 0; i < TEXT_ROWS && w.next(line); i++) {
                emit(TEXT_FIRST_ROW + i, 0, line, ATTR_TEXT);
            }
        }
        return;
    }
    // the pages after the first: what did not fit, a line at a time from the top
    int skip = (page - 1) * MORE_ROWS;
    int row = 1;
    int index = 0;
    auto give = [&](const char* s, uint8_t attr) {
        if (index >= skip && row <= MORE_ROWS) {
            emit(row++, 0, s, attr);
        }
        index++;
    };
    if (info.any()) {
        const char* c = leftover(info.controls, CTRL_WIDTH, ctrl_rows(info));
        if (*c != '\0') {
            give("Controls (more)", ATTR_HEADING);
            Wrap w(c, TEXT_WIDTH);
            while (w.next(line)) {
                give(line, ATTR_TEXT);
            }
        }
        const char* t = leftover(info.text, TEXT_WIDTH, TEXT_ROWS);
        if (*t != '\0') {
            give("Instructions (more)", ATTR_HEADING);
            Wrap w(t, TEXT_WIDTH);
            while (w.next(line)) {
                give(line, ATTR_TEXT);
            }
        }
    }
    if (info.ntape > 0) {
        give("Tape", ATTR_HEADING);
        Wrap w(info.tape, TEXT_WIDTH);
        while (w.next(line)) {
            give(line, ATTR_TEXT);
        }
    }
    if (info.nnotes > 0) {
        give("My notes", ATTR_HEADING);
        Wrap w(info.notes, TEXT_WIDTH);
        while (w.next(line)) {
            give(line, ATTR_TEXT);
        }
    }
}

// True if the screen is all zeros (black on black), which is no picture
inline bool screen_is_blank(const uint8_t* screen) {
    for (int i = 0; i < 6912; i++) {
        if (screen[i] != 0) {
            return false;
        }
    }
    return true;
}

// The key row: what can be pressed (out holds 40 letters). In the menu (from_menu) the
// information is only looked at; from Browse Games ENTER loads the game.
// can_delete: a screen grab is showing, which D deletes. M edits the game's details and notes.
inline void key_help(bool from_menu, int pages, char* out, bool can_delete = false) {
    if (from_menu) {
        snprintf(out, 40, "%s", pages > 1 ? "N/P-Page  M-Edit  X-Exit" : "M-Edit  X-Exit");
        return;
    }
    if (can_delete) {
        snprintf(out, 40, "%s", pages > 1 ? "ENT-Load N/P D-Del M-Edit X-Bk" : "ENTER-Load D-Del M-Edit X-Back");
        return;
    }
    snprintf(out, 40, "%s", pages > 1 ? "ENTER-Load N/P M-Edit X-Back" : "ENTER-Load  M-Edit  X-Back");
}

}   // namespace gameinfo
