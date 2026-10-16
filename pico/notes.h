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


// Your own notes on a game: a small text file for each game in the hidden folder
// "Game Notes" in the root of the SD card, as <game>.txt (the name without " Save Game",
// like the screen grabs). gameinfo.h shows them as the last section, My notes, and M on that
// screen edits them with the Editor here. The file is plain text, so a PC can read and
// change it too (the editor refuses a file that does not fit its screen, rather than cut it).
// Nothing here touches the hardware, so it is tested on a PC.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

namespace notes {

static const char FOLDER[] = "/Game Notes";
static const int COLS = 30;                     // letters in a line
static const int LINES = 18;                    // lines in the editor
static const size_t FILE_MAX = LINES * (COLS + 1) + 1;      // bytes of a file the editor can hold

// "/Game Notes/<base>.txt"
inline void path_for(const char* base, char* out, size_t outsize) {
    snprintf(out, outsize, "%s/%s.txt", FOLDER, base);
}

// The keys the NMI ROM sends (what its keyboard routine gives: a letter, digit or symbol as
// the letter itself; CAPS+0 is DELETE, CAPS+5 6 7 8 the arrows, ENTER, BREAK and CAPS+1 EDIT)
static const uint8_t KEY_EDIT = 0x01, KEY_LEFT = 0x05, KEY_DOWN = 0x06, KEY_UP = 0x07, KEY_RIGHT = 0x08;
static const uint8_t KEY_DELETE = 0x0A, KEY_ENTER = 0x13, KEY_BREAK = 0x18;

// The editor is a template so the game's details can be edited in a taller one (scrolling) than
// the notes: LINES lines of COLS letters. The notes' is Editor, 18 lines of 30.
template <int LINES, int COLS>
class EditorT {
public:
    enum Result { CONTINUE, SAVE, DISCARD };

    EditorT() { clear(); }

    void clear() {
        for (int i = 0; i < LINES; i++) {
            lines_[i][0] = '\0';
        }
        count_ = 1;
        row_ = col_ = top_ = 0;
        changed_ = false;
        prompt_ = false;
    }

    // Takes the text of a file (lines ending in \n or \r\n). False if it does not fit the
    // screen: nothing is loaded then and the editor must not be used for that file.
    bool load(const char* data, size_t n) {
        clear();
        int line = 0, len = 0;
        bool after_cr = false;
        for (size_t i = 0; i < n; i++) {
            char c = data[i];
            if (c == '\n' && after_cr) {
                after_cr = false;
                continue;
            }
            after_cr = c == '\r';
            if (c == '\r') {
                c = '\n';
            }
            if (c == '\n') {
                if (line + 1 >= LINES) {
                    // a last empty line is not a line
                    if (i + 1 < n) {
                        clear();
                        return false;
                    }
                    break;
                }
                lines_[line][len] = '\0';
                line++;
                len = 0;
                continue;
            }
            if (c == '\t') {
                c = ' ';
            }
            if ((unsigned char) c < 32 || (unsigned char) c > 126) {
                c = '?';
            }
            if (len >= COLS) {
                clear();
                return false;           // a longer line: the screen would cut it
            }
            lines_[line][len++] = c;
        }
        lines_[line][len] = '\0';
        count_ = line + 1;
        while (count_ > 1 && lines_[count_ - 1][0] == '\0') {
            count_--;                   // the end of the last line is not another line
        }
        // the cursor at the end of the text
        row_ = count_ - 1;
        col_ = (int) strlen(lines_[row_]);
        changed_ = false;
        scroll();
        return true;
    }

    // The text for the file: the lines each ending in \n, no empty lines at the end.
    // Returns the length (0 if there is nothing).
    size_t text(char* out, size_t outsize) const {
        int last = count_ - 1;
        while (last >= 0 && lines_[last][0] == '\0') {
            last--;
        }
        size_t n = 0;
        for (int i = 0; i <= last; i++) {
            size_t len = strlen(lines_[i]);
            if (n + len + 1 >= outsize) {
                break;
            }
            memcpy(out + n, lines_[i], len);
            n += len;
            out[n++] = '\n';
        }
        out[n] = '\0';
        return n;
    }

    Result key(uint8_t k) {
        if (prompt_) {
            prompt_ = false;
            if (k == 'y' || k == 'Y') {
                return SAVE;
            }
            if (k == 'n' || k == 'N') {
                return DISCARD;
            }
            return CONTINUE;            // any other key: go on editing
        }
        switch (k) {
            case KEY_BREAK:
            case KEY_EDIT:
                if (!changed_) {
                    return DISCARD;     // nothing to save
                }
                prompt_ = true;
                return CONTINUE;
            case KEY_ENTER:
                if (LINES == 1) {
                    // a one-line box: ENTER is the end of it
                    if (!changed_) {
                        return DISCARD;
                    }
                    prompt_ = true;
                    return CONTINUE;
                }
                split();
                break;
            case KEY_DELETE:
                backspace();
                break;
            case KEY_LEFT:
                if (col_ > 0) {
                    col_--;
                } else if (row_ > 0) {
                    row_--;
                    col_ = (int) strlen(lines_[row_]);
                }
                break;
            case KEY_RIGHT:
                if (col_ < (int) strlen(lines_[row_])) {
                    col_++;
                } else if (row_ + 1 < count_) {
                    row_++;
                    col_ = 0;
                }
                break;
            case KEY_UP:
                if (row_ > 0) {
                    row_--;
                    clamp_col();
                }
                break;
            case KEY_DOWN:
                if (row_ + 1 < count_) {
                    row_++;
                    clamp_col();
                }
                break;
            default:
                if (k >= 32 && k <= 126 && k != '~') {     // ~ is what the ROM's table has for no key
                    insert((char) k);
                }
                break;
        }
        scroll();
        return CONTINUE;
    }

    // At most this many bytes of text() (each line counts its letters and its end of line); 0 is no limit
    void set_limit(size_t bytes) { limit_ = bytes; }
    size_t limit() const { return limit_; }
    // The bytes text() would give now
    size_t used() const {
        int last = count_ - 1;
        while (last >= 0 && lines_[last][0] == '\0') {
            last--;
        }
        size_t n = 0;
        for (int i = 0; i <= last; i++) {
            n += strlen(lines_[i]) + 1;
        }
        return n;
    }
    // The first line on the screen, for an editor with more lines than the screen's 18
    int top() const { return top_; }
    static const int VIEW = 18;

    bool changed() const { return changed_; }
    bool prompting() const { return prompt_; }
    int line_count() const { return count_; }
    const char* line(int i) const { return lines_[i]; }
    int row() const { return row_; }
    int col() const { return col_; }

private:
    char lines_[LINES][COLS + 1];
    int count_;
    int row_, col_;
    int top_ = 0;
    size_t limit_ = 0;
    bool changed_, prompt_;

    void scroll() {
        if (row_ < top_) {
            top_ = row_;
        } else if (row_ >= top_ + VIEW) {
            top_ = row_ - VIEW + 1;
        }
    }

    bool full() const { return limit_ != 0 && used() + 2 > limit_; }

    void clamp_col() {
        int len = (int) strlen(lines_[row_]);
        if (col_ > len) {
            col_ = len;
        }
    }

    void insert(char c) {
        if (full()) {
            return;
        }
        char* l = lines_[row_];
        int len = (int) strlen(l);
        if (len >= COLS) {
            // a full line: typing at its end goes on at the start of a new line below
            if (col_ == len && count_ < LINES && row_ + 1 == count_) {
                row_ = count_++;
                col_ = 0;
                lines_[row_][0] = '\0';
                l = lines_[row_];
                len = 0;
            } else if (col_ == len && row_ + 1 < count_ && lines_[row_ + 1][0] == '\0') {
                row_++;
                col_ = 0;
                l = lines_[row_];
                len = 0;
            } else {
                return;
            }
        }
        memmove(l + col_ + 1, l + col_, (size_t) (len - col_ + 1));
        l[col_++] = c;
        changed_ = true;
    }

    void split() {
        if (count_ >= LINES || full()) {
            return;
        }
        for (int i = count_; i > row_ + 1; i--) {
            memcpy(lines_[i], lines_[i - 1], COLS + 1);
        }
        count_++;
        strcpy(lines_[row_ + 1], lines_[row_] + col_);
        lines_[row_][col_] = '\0';
        row_++;
        col_ = 0;
        changed_ = true;
    }

    void backspace() {
        if (col_ > 0) {
            char* l = lines_[row_];
            memmove(l + col_ - 1, l + col_, strlen(l + col_) + 1);
            col_--;
            changed_ = true;
        } else if (row_ > 0) {
            // join with the line above if it fits
            int above = (int) strlen(lines_[row_ - 1]);
            int here = (int) strlen(lines_[row_]);
            if (above + here > COLS) {
                return;
            }
            strcat(lines_[row_ - 1], lines_[row_]);
            for (int i = row_; i + 1 < count_; i++) {
                memcpy(lines_[i], lines_[i + 1], COLS + 1);
            }
            count_--;
            lines_[count_][0] = '\0';
            row_--;
            col_ = above;
            changed_ = true;
        }
    }
};

using Editor = EditorT<LINES, COLS>;

}   // namespace notes
