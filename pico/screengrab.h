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


// Screen grabs: the screen of a running game saved as a .scr (the 6912 bytes of the Spectrum
// screen, which V in Browse Games shows), in /Scraper/Screen Grabs on the SD card.
// A grab has a category, in its name:
//   <game> Loading Screen.scr, <game> Loading Screen1.scr, <game> Loading Screen2.scr ...
//   the same with Menu Screen, Game Screen and Ending Screen
//   <game>.scr, <game>1.scr ... (Standard Screen Number: the older names, and a double press)
// and the screen in the game's file itself (a snapshot's, a tape's loading screen) is From File.
// V shows a game's pictures category by category, in the order of the [Screen grab order] section
// of Picomenu.ini (settings.h) or, with none, of the file order.txt (or order) in the same folder
// (a category on each line; # starts a comment), and by number in each. A category not named is
// not shown.
// Nothing here touches the hardware, so it is tested on a PC.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

namespace screengrab {

// Where grabs were saved before (Rev 1.10 and earlier): V still shows any left there, as
// Standard Screen Number after those in SCRAPER_FOLDER. Nothing is saved there now.
static const char OLD_FOLDER[] = "/Screen Grabs";
// The scraper program saves the loading screens it downloads in a "Screen Grabs" folder beside
// the Instructions file, so with the Instructions file in the folder Scraper they are here, and
// the Picoface saves its grabs here too.
static const char SCRAPER_FOLDER[] = "/Scraper/Screen Grabs";
static const char ORDER_FILE[] = "/Scraper/Screen Grabs/order.txt";     // as Set up SD card writes it
static const char ORDER_FILE_BARE[] = "/Scraper/Screen Grabs/order";      // read too, if there is no order.txt
static const size_t SCR_SIZE = 6912;
static const int MAX_SCAN = 100;        // the numbers a save looks at in a category (none, 1 ... 99)
static const int MAX_PICTURES = 15;     // the most V cycles through for one game (the file's own picture is one)

// The categories. STANDARD has no words in the name.
// FROM_FILE is not a .scr: the picture in the game's own file
enum Category : uint8_t { STANDARD = 0, LOADING = 1, MENU = 2, GAME = 3, ENDING = 4, FROM_FILE = 5, CATEGORIES = 6 };
static const int GRAB_CATEGORIES = 5;  // those a .scr can be (STANDARD to ENDING)
// What goes after the game's name, before the number
static const char* const SUFFIX[GRAB_CATEGORIES] = { "", " Loading Screen", " Menu Screen", " Game Screen", " Ending Screen" };
// As the order file names them
static const char* const ORDER_NAME[CATEGORIES] = { "Standard Screen Number", "Loading Screen", "Menu Screen", "Game Screen", "Ending Screen", "From File" };
// The order with no order file (as the example one): the file's own picture first, as before
static const Category DEFAULT_ORDER[CATEGORIES] = { FROM_FILE, LOADING, MENU, GAME, ENDING, STANDARD };
// The order file Set up SD card writes
static const char ORDER_EXAMPLE[] =       // lines of 30 letters at most, so it can be edited on the Spectrum
    "#Screen Grab Display Order\r\n"
    "#V shows a game's pictures in\r\n"
    "#this order. Put # in front of\r\n"
    "#a kind to hide it.\r\n"
    "#From File: the screen in the\r\n"
    "#game's own file.\r\n"
    "#Standard Screen Number:\r\n"
    "#<game>.scr, <game>1.scr ...\r\n"
    "\r\n"
    "From File\r\n"
    "Loading Screen\r\n"
    "Menu Screen\r\n"
    "Game Screen\r\n"
    "Ending Screen\r\n"
    "Standard Screen Number\r\n";
static const size_t NAME_MAX = 64;      // letters of the game's name used

// The name of the game for its grabs: without a " Save Game" on the end (so a quick-saved
// game shares the grabs of the game), at most NAME_MAX letters, no blanks at the end.
// Returns the length (0 if there is no name).
inline size_t base_name(const char* game, char* out) {
    size_t n = strlen(game);
    static const char SUFFIX[] = " Save Game";
    static const size_t SUFFIX_LEN = sizeof(SUFFIX) - 1;
    size_t digits = 0;          // a numbered save, " Save Game 2", is the same game
    while (digits < n && game[n - 1 - digits] >= '0' && game[n - 1 - digits] <= '9') {
        digits++;
    }
    if (digits > 0 && digits <= 3 && n > digits + 1 + SUFFIX_LEN && game[n - digits - 1] == ' ' &&
            strncasecmp(game + n - digits - 1 - SUFFIX_LEN, SUFFIX, SUFFIX_LEN) == 0) {
        n -= digits + 1;
    }
    if (n > SUFFIX_LEN && strncasecmp(game + n - SUFFIX_LEN, SUFFIX, SUFFIX_LEN) == 0) {
        n -= SUFFIX_LEN;
    }
    if (n > NAME_MAX) {
        n = NAME_MAX;
    }
    while (n > 0 && game[n - 1] == ' ') {
        n--;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) game[i];
        // letters FAT does not allow in a name
        out[i] = (c < 32 || strchr("\\/:*?\"<>|", c) != nullptr) ? '_' : (char) c;
    }
    out[n] = '\0';
    return n;
}

// Reads an order file: rank[c] is where category c comes (0 first). A category on each line
// (case does not matter; # starts a comment); one it does not name is not shown (shown[c]).
// With no file, or one that names none, all are shown, in the default order. Give it the
// lines with line(), then finish().
struct Order {
    uint8_t rank[CATEGORIES];
    bool named[CATEGORIES] = {};
    bool shown[CATEGORIES] = {};
    uint8_t next = 0;

    // False if the line is not blank, a comment or a category
    bool line(const char* text, size_t n) {
        size_t a = 0, b = n;
        while (a < b && (text[a] == ' ' || text[a] == '\t')) a++;
        while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r')) b--;
        if (b == a || text[a] == '#') return true;
        bool known = false;
        for (int c = 0; c < CATEGORIES; c++) {
            size_t len = strlen(ORDER_NAME[c]);
            bool same = (b - a == len && strncasecmp(text + a, ORDER_NAME[c], len) == 0) ||
                (c == STANDARD && b - a == 13 && strncasecmp(text + a, "Screen Number", 13) == 0);
            if (same && !named[c]) {
                named[c] = true;
                rank[c] = next++;
            }
            known = known || same;
        }
        return known;
    }

    // All of a text (lines end with \n, \r\n or \r)
    void text(const char* t, size_t n) {
        size_t i = 0;
        while (i < n) {
            size_t e = i;
            while (e < n && t[e] != '\n' && t[e] != '\r') e++;
            line(t + i, e - i);
            i = e + 1;
        }
    }

    void finish() {
        const bool all = next == 0;     // no file, or none named in it
        for (int k = 0; k < CATEGORIES; k++) {
            Category c = DEFAULT_ORDER[k];
            shown[c] = all || named[c];
            if (!named[c]) {
                named[c] = true;
                rank[c] = next++;
            }
        }
    }
};

// The category and number of a file name (no folder) that is a grab of base: true if it is one.
// "<base><suffix>[<number>].scr", with no 0 in front of a number (that would be another file
// with the same number). Case does not matter, as on the card.
// A .png of the same name is a grab too (grab_format in Picomenu.ini): *png says which it is.
inline bool parse_name(const char* name, const char* base, Category* cat, int* number, bool* png = nullptr) {
    size_t bl = strlen(base), nl = strlen(name);
    if (nl < bl + 4 || strncasecmp(name, base, bl) != 0) {
        return false;
    }
    bool is_png = strcasecmp(name + nl - 4, ".png") == 0;
    if (!is_png && strcasecmp(name + nl - 4, ".scr") != 0) {
        return false;
    }
    if (png != nullptr) *png = is_png;
    const char* rest = name + bl;
    size_t rl = nl - bl - 4;            // the part between the name and ".scr"
    for (int c = GRAB_CATEGORIES - 1; c >= 0; c--) {    // STANDARD (no words) last
        size_t sl = strlen(SUFFIX[c]);
        if (rl < sl || strncasecmp(rest, SUFFIX[c], sl) != 0) continue;
        const char* d = rest + sl;
        size_t dl = rl - sl;
        if (dl > 3 || (dl > 0 && d[0] == '0')) continue;
        int v = 0;
        bool digits = true;
        for (size_t k = 0; k < dl; k++) {
            if (d[k] < '0' || d[k] > '9') { digits = false; break; }
            v = v * 10 + (d[k] - '0');
        }
        if (!digits) continue;
        *cat = (Category) c;
        *number = v;
        return true;
    }
    return false;
}

// "<folder>/<base><suffix>" then nothing for number 0, or the number, then ".scr" (or ext)
inline void grab_path(const char* folder, const char* base, Category cat, int number, char* out, size_t outsize,
                      const char* ext = "scr") {
    if (number == 0) {
        snprintf(out, outsize, "%s/%s%s.%s", folder, base, SUFFIX[cat], ext);
    } else {
        snprintf(out, outsize, "%s/%s%s%d.%s", folder, base, SUFFIX[cat], number, ext);
    }
}

// The name of a grab without its folder and .scr ("Exolon Loading Screen1")
inline void grab_name(const char* base, Category cat, int number, char* out, size_t outsize) {
    if (number == 0) {
        snprintf(out, outsize, "%s%s", base, SUFFIX[cat]);
    } else {
        snprintf(out, outsize, "%s%s%d", base, SUFFIX[cat], number);
    }
}

// "<folder>/<base>" then nothing for number 0, or the number, then the extension (with its dot)
inline void path_in(const char* folder, const char* base, int number, const char* ext, char* out, size_t outsize) {
    if (number == 0) {
        snprintf(out, outsize, "%s/%s%s", folder, base, ext);
    } else {
        snprintf(out, outsize, "%s/%s%d%s", folder, base, number, ext);
    }
}

// The text the Pico shows after a grab: "Saved as <name>", cut to fit a line of 31
inline void saved_message(const char* base, Category cat, int number, char* out, size_t outsize) {
    char name[NAME_MAX + 24];
    grab_name(base, cat, number, name, sizeof(name));
    size_t room = 31 - 9;           // "Saved as " is 9
    size_t n = strlen(name);
    if (n > room) {
        // keep the end, which has the number
        snprintf(out, outsize, "Saved as ...%s", name + n - (room - 3));
    } else {
        snprintf(out, outsize, "Saved as %s", name);
    }
}

}   // namespace screengrab
