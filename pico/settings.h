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


// Picomenu.ini: the settings file, in the root of the SD card (or, if it has none, of the
// internal flash). Sections in [brackets], then "key = value" lines; # starts a comment, also
// after a value (" # ..."). Case does not matter. A key or value that is not known is skipped
// and the first such line is kept for Help > Diagnostics; the setting keeps its default.
// The [Screen grab order] section is a category on each line, as order.txt is.
// Nothing here touches the hardware, so it is tested on a PC.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

#include "screengrab.h"

namespace settings {

static const char FILE_NAME[] = "/Picomenu.ini";

enum Show : uint8_t { SHOW_OFF = 0, SHOW_ON = 1, SHOW_LAST = 2 };
enum Sort : uint8_t { SORT_NAME = 0, SORT_REVERSE = 1, SORT_TYPE = 2 };
enum Folders : uint8_t { FOLDERS_FIRST = 0, FOLDERS_LAST = 1, FOLDERS_MIXED = 2 };
enum Drive : uint8_t { DRIVE_FLASH = 0, DRIVE_SD = 1, DRIVE_LAST = 2 };
enum Quick : uint8_t { QUICK_ASK = 0, QUICK_OVERWRITE = 1, QUICK_NEW = 2 };
enum GrabFormat : uint8_t { GRAB_SCR = 0, GRAB_PNG = 1, GRAB_BOTH = 2 };
enum Section : uint8_t { SEC_NONE, SEC_BROWSE, SEC_SAVING, SEC_BUTTONS, SEC_CONVERTING, SEC_BIN, SEC_ORDER, SEC_DISPLAY, SEC_TAPES, SEC_WIFI, SEC_BLUETOOTH, SEC_UNKNOWN };

// long_press: holding the button this long resets the Spectrum. Below 2 seconds (1.0 was the
// old default) reads as 3: a hold of 1 second up to it is the save screen or the screen grab.
static const uint16_t LONG_PRESS_MIN = 2000, LONG_PRESS_MAX = 5000, LONG_PRESS_DEFAULT = 3000;
static const size_t BAD_TEXT = 24;      // letters of a line that was not understood kept

struct Settings {
    // [Display]
    bool light = false;                 // theme = light: the menus black on white
    // [Browse]
    uint8_t show_all = SHOW_OFF;
    uint8_t details = SHOW_OFF;
    uint8_t sort = SORT_NAME;
    uint8_t folders = FOLDERS_FIRST;
    uint8_t start_drive = DRIVE_SD;
    // [Saving]
    uint8_t save_type = 0;              // ACTION_SNA_SAVE_TYPE_AUTO, _SNA, _Z80, _SZX (0 ... 3)
    uint8_t quick_save = QUICK_ASK;
    bool slots_on_load = true;          // ask "carry on from a slot?" when a game with slots loads
    bool save_screen_slots = true;      // S opens the save slots (false: the Save snapshot screen)
    uint8_t grab_format = GRAB_SCR;     // screen grabs saved as .scr, .png or both
    uint8_t checkpoint_s = 0;           // autosave: one every 20, 40 or 60 seconds (0: off)
    // [Buttons]
    uint16_t long_press_ms = LONG_PRESS_DEFAULT;    // the reset (0: off)
    bool double_press = true;           // a double press does something (false: off)
    bool save_on_hold = true;           // save_press: hold (a 1 s hold saves, a double press grabs) or double
    // [Converting]
    bool loading_grab = true;
    // [Waste Bin]
    bool waste_bin = true;
    // [Tapes]
    bool level_loaders = true;          // patch a game's own copy of the ROM loader for the tape trap
    uint8_t lock_48k = 1;               // a 48K program on a 128K locked in 48K mode: 0 off, 1 with a tape, 2 always
    // [Bluetooth] (a Pico W: B - Bluetooth tape)
    uint8_t bt_volume = 0;              // 0 1/2 (loads best), 1 3/4, 2 full
    bool bt_auto_connect = true;        // B connects to the receiver paired last by itself
    bool bt_auto_play = true;           // LOAD "" (the ROM's loader) starts the tape
    bool bt_auto_off = true;            // off a few seconds after the tape ends (autosave back)
    // [Screen grab order]
    bool has_order = false;             // the file has the section (then order.txt is not read)
    screengrab::Order order;            // its lines, not yet finish()ed

    // What was read, for Diagnostics
    uint8_t source = 0;                 // 0 no file, 1 the SD card, 2 the internal flash
    uint16_t good = 0;                  // settings understood
    uint16_t bad = 0;                   // lines not understood
    uint16_t bad_line = 0;              // the first of them (1 is the first line)
    char bad_text[BAD_TEXT + 1] = "";
};

// Reads the file a line at a time. start_folder (if not null) gets the value of
// start_folder, in the form sd_cwd is ("" for the root, else "/Games", no / at the end);
// it is left alone if the file has none.
// [WiFi] (a Pico W): ssid, password and web_password are not kept in Settings (the RAM is
// tight); they are read again, into Wifi, when the file transfer starts.
struct Wifi {
    char ssid[33];
    char password[64];
    char web_password[33];
};

struct Parser {
    Settings& s;
    char* start_folder;
    size_t folder_size;
    Wifi* wifi = nullptr;               // set: [WiFi]'s values go here
    Section section = SEC_NONE;
    uint16_t number = 0;

    Parser(Settings& to, char* folder = nullptr, size_t size = 0) : s(to), start_folder(folder), folder_size(size) {}

    static bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

    static bool same(const char* t, size_t n, const char* word) {
        return strlen(word) == n && strncasecmp(t, word, n) == 0;
    }

    // which of the words (null at the end) the value is: -1 none
    static int pick(const char* v, size_t n, const char* const* words) {
        for (int i = 0; words[i] != nullptr; i++) {
            if (same(v, n, words[i])) return i;
        }
        return -1;
    }

    void problem(const char* text, size_t n) {
        if (s.bad++ == 0) {
            s.bad_line = number;
            size_t k = n < BAD_TEXT ? n : BAD_TEXT;
            memcpy(s.bad_text, text, k);
            s.bad_text[k] = '\0';
        }
    }

    // "1", "1.0", "0.5", "2.25" seconds, or off: ms (0 off), or -1
    static int seconds(const char* v, size_t n) {
        if (same(v, n, "off")) return 0;
        size_t i = 0;
        uint32_t whole = 0, frac = 0, scale = 1000;
        if (n == 0) return -1;
        while (i < n && v[i] >= '0' && v[i] <= '9' && whole < 100) whole = whole * 10 + (uint32_t) (v[i++] - '0');
        if (i == 0 && (n == 0 || v[0] != '.')) return -1;
        if (i < n && v[i] == '.') {
            i++;
            size_t digits = 0;
            while (i < n && v[i] >= '0' && v[i] <= '9') {
                if (digits < 3) {
                    scale /= 10;
                    frac += (uint32_t) (v[i] - '0') * scale;
                }
                digits++;
                i++;
            }
        }
        if (i < n) return -1;
        uint32_t ms = whole * 1000 + frac;
        if (ms > LONG_PRESS_MAX) return -1;
        return (int) ms;
    }

    bool folder(const char* v, size_t n) {
        if (n == 0 || v[0] != '/') return false;
        while (n > 0 && v[n - 1] == '/') n--;              // "/" is the root, "/Games/" is /Games
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char) v[i];
            if (c < 32 || strchr("\\:*?\"<>|", c) != nullptr) return false;
            if (c == '/' && i + 1 < n && v[i + 1] == '/') return false;
        }
        if (start_folder != nullptr) {
            if (n + 1 > folder_size) return false;
            memcpy(start_folder, v, n);
            start_folder[n] = '\0';
        }
        return true;
    }

    // A key and its value: true if both are known
    bool setting(const char* k, size_t kn, const char* v, size_t vn) {
        static const char* const SHOW[] = { "off", "on", "last", nullptr };
        static const char* const ONOFF[] = { "off", "on", nullptr };
        int i;
        switch (section) {
        case SEC_BROWSE:
            if (same(k, kn, "show_all")) {
                if ((i = pick(v, vn, SHOW)) < 0) return false;
                s.show_all = (uint8_t) i;
                return true;
            }
            if (same(k, kn, "details")) {
                if ((i = pick(v, vn, SHOW)) < 0) return false;
                s.details = (uint8_t) i;
                return true;
            }
            if (same(k, kn, "sort")) {
                static const char* const W[] = { "name", "reverse", "type", nullptr };
                if ((i = pick(v, vn, W)) < 0) return false;
                s.sort = (uint8_t) i;
                return true;
            }
            if (same(k, kn, "folders")) {
                static const char* const W[] = { "first", "last", "mixed", nullptr };
                if ((i = pick(v, vn, W)) < 0) return false;
                s.folders = (uint8_t) i;
                return true;
            }
            if (same(k, kn, "start_drive")) {
                static const char* const W[] = { "flash", "sd", "last", nullptr };
                if ((i = pick(v, vn, W)) < 0) return false;
                s.start_drive = (uint8_t) i;
                return true;
            }
            if (same(k, kn, "start_folder")) {
                return folder(v, vn);
            }
            return false;
        case SEC_SAVING:
            if (same(k, kn, "save_type")) {
                static const char* const W[] = { "auto", "sna", "z80", "szx", nullptr };
                static const char* const D[] = { "auto", ".sna", ".z80", ".szx", nullptr };
                if ((i = pick(v, vn, W)) < 0 && (i = pick(v, vn, D)) < 0) return false;
                s.save_type = (uint8_t) i;
                return true;
            }
            if (same(k, kn, "quick_save")) {
                static const char* const W[] = { "ask", "overwrite", "new", nullptr };
                if ((i = pick(v, vn, W)) < 0) return false;
                s.quick_save = (uint8_t) i;
                return true;
            }
            if (same(k, kn, "slots_on_load")) {
                static const char* const W[] = { "never", "ask", nullptr };
                if ((i = pick(v, vn, W)) < 0) return false;
                s.slots_on_load = i == 1;
                return true;
            }
            if (same(k, kn, "autosave") || same(k, kn, "checkpoints") || same(k, kn, "rewind")) {   // (earlier 2.3.0 tests' names)
                // (5 and 10, from the 2.3.0 tests, are 20 now)
                static const char* const W[] = { "off", "20", "40", "60", "5", "10", nullptr };
                static const uint8_t S[] = { 0, 20, 40, 60, 20, 20 };
                if ((i = pick(v, vn, W)) < 0) return false;
                s.checkpoint_s = S[i];
                return true;
            }
            if (same(k, kn, "grab_format")) {
                static const char* const W[] = { "scr", "png", "both", nullptr };
                if ((i = pick(v, vn, W)) < 0) return false;
                s.grab_format = (uint8_t) i;
                return true;
            }
            if (same(k, kn, "save_screen")) {
                static const char* const W[] = { "named", "slots", nullptr };
                if ((i = pick(v, vn, W)) < 0) return false;
                s.save_screen_slots = i == 1;
                return true;
            }
            return false;
        case SEC_BUTTONS:
            if (same(k, kn, "long_press")) {
                if ((i = seconds(v, vn)) < 0) return false;
                s.long_press_ms = i == 0 ? 0 : i < LONG_PRESS_MIN ? LONG_PRESS_DEFAULT : (uint16_t) i;
                return true;
            }
            if (same(k, kn, "double_press")) {
                static const char* const W[] = { "off", "on", "grab", nullptr };
                if ((i = pick(v, vn, W)) < 0) return false;
                s.double_press = i != 0;
                return true;
            }
            if (same(k, kn, "save_press")) {
                static const char* const W[] = { "double", "hold", nullptr };
                if ((i = pick(v, vn, W)) < 0) return false;
                s.save_on_hold = i == 1;
                return true;
            }
            return false;
        case SEC_CONVERTING:
            if (same(k, kn, "loading_grab")) {
                if ((i = pick(v, vn, ONOFF)) < 0) return false;
                s.loading_grab = i == 1;
                return true;
            }
            return false;
        case SEC_DISPLAY:
            if (same(k, kn, "theme")) {
                static const char* const W[] = { "dark", "light", nullptr };
                if ((i = pick(v, vn, W)) < 0) return false;
                s.light = i == 1;
                return true;
            }
            return false;
        case SEC_BIN:
            if (same(k, kn, "waste_bin")) {
                if ((i = pick(v, vn, ONOFF)) < 0) return false;
                s.waste_bin = i == 1;
                return true;
            }
            return false;
        case SEC_WIFI: {
            char* to = nullptr;
            size_t size = 0;
            if (same(k, kn, "ssid")) { to = wifi ? wifi->ssid : nullptr; size = sizeof(Wifi::ssid); }
            else if (same(k, kn, "password")) { to = wifi ? wifi->password : nullptr; size = sizeof(Wifi::password); }
            else if (same(k, kn, "web_password")) { to = wifi ? wifi->web_password : nullptr; size = sizeof(Wifi::web_password); }
            else return false;
            if (vn >= size) return false;
            if (to != nullptr) {
                memcpy(to, v, vn);
                to[vn] = '\0';
            }
            return true;
        }
        case SEC_BLUETOOTH:
            if (same(k, kn, "volume")) {
                static const char* const W[] = { "1/2", "3/4", "full", nullptr };
                if ((i = pick(v, vn, W)) < 0) return false;
                s.bt_volume = (uint8_t) i;
                return true;
            }
            if (same(k, kn, "auto_connect")) {
                if ((i = pick(v, vn, ONOFF)) < 0) return false;
                s.bt_auto_connect = i == 1;
                return true;
            }
            if (same(k, kn, "auto_play")) {
                if ((i = pick(v, vn, ONOFF)) < 0) return false;
                s.bt_auto_play = i == 1;
                return true;
            }
            if (same(k, kn, "auto_off")) {
                if ((i = pick(v, vn, ONOFF)) < 0) return false;
                s.bt_auto_off = i == 1;
                return true;
            }
            return false;
        case SEC_TAPES:
            if (same(k, kn, "level_loaders")) {
                static const char* const W[] = { "off", "patch", nullptr };
                if ((i = pick(v, vn, W)) < 0) return false;
                s.level_loaders = i == 1;
                return true;
            }
            if (same(k, kn, "lock_48k")) {
                static const char* const W[] = { "off", "tapes", "always", nullptr };
                if ((i = pick(v, vn, W)) < 0) return false;
                s.lock_48k = (uint8_t) i;
                return true;
            }
            return false;
        default:
            return false;
        }
    }

    void line(const char* text, size_t n) {
        number++;
        size_t a = 0, b = n;
        if (number == 1 && n >= 3 && (unsigned char) text[0] == 0xEF && (unsigned char) text[1] == 0xBB &&
                (unsigned char) text[2] == 0xBF) {
            a = 3;                      // a UTF-8 mark, as some PC editors write
        }
        while (a < b && is_blank(text[a])) a++;
        while (b > a && is_blank(text[b - 1])) b--;
        if (b == a || text[a] == '#' || text[a] == ';') return;
        const char* t = text + a;
        size_t len = b - a;
        if (t[0] == '[') {
            if (t[len - 1] != ']') {
                section = SEC_UNKNOWN;
                problem(t, len);
                return;
            }
            size_t x = 1, y = len - 1;
            while (x < y && is_blank(t[x])) x++;
            while (y > x && is_blank(t[y - 1])) y--;
            const char* nm = t + x;
            size_t nn = y - x;
            section = same(nm, nn, "browse") ? SEC_BROWSE : same(nm, nn, "saving") ? SEC_SAVING :
                same(nm, nn, "buttons") ? SEC_BUTTONS : same(nm, nn, "converting") ? SEC_CONVERTING :
                same(nm, nn, "waste bin") ? SEC_BIN : same(nm, nn, "screen grab order") ? SEC_ORDER :
                same(nm, nn, "display") ? SEC_DISPLAY : same(nm, nn, "tapes") ? SEC_TAPES :
                same(nm, nn, "wifi") ? SEC_WIFI : same(nm, nn, "bluetooth") ? SEC_BLUETOOTH : SEC_UNKNOWN;
            if (section == SEC_UNKNOWN) problem(t, len);
            if (section == SEC_ORDER) s.has_order = true;
            return;
        }
        if (section == SEC_ORDER) {
            if (s.order.line(t, len)) s.good++;
            else problem(t, len);
            return;
        }
        if (section == SEC_UNKNOWN) {
            return;                     // the section was reported
        }
        const char* eq = (const char*) memchr(t, '=', len);
        if (eq == nullptr) {
            problem(t, len);
            return;
        }
        size_t kn = (size_t) (eq - t);
        while (kn > 0 && is_blank(t[kn - 1])) kn--;
        const char* v = eq + 1;
        size_t vn = len - (size_t) (v - t);
        while (vn > 0 && is_blank(*v)) { v++; vn--; }
        for (size_t i = 1; i < vn; i++) {          // " # a comment" after the value
            if (v[i] == '#' && is_blank(v[i - 1])) {
                vn = i;
                break;
            }
        }
        while (vn > 0 && is_blank(v[vn - 1])) vn--;
        if (kn > 0 && setting(t, kn, v, vn)) s.good++;
        else problem(t, len);
    }

    // All of a text (lines end with \n, \r\n or \r)
    void text(const char* t, size_t n) {
        size_t i = 0;
        while (i < n) {
            size_t e = i;
            while (e < n && t[e] != '\n' && t[e] != '\r') e++;
            line(t + i, e - i);
            if (e + 1 < n && t[e] == '\r' && t[e + 1] == '\n') e++;
            i = e + 1;
        }
    }
};

// start_folder names a folder that is not on the card: Browse Games starts in the root
inline void missing_folder(Settings& s) {
    if (s.bad++ == 0) {
        s.bad_line = 0;
        snprintf(s.bad_text, sizeof(s.bad_text), "no start_folder");
    }
}

// What Diagnostics says about the file (out has room for 31)
inline void describe(const Settings& s, char* out, size_t size) {
    if (s.source == 0) {
        snprintf(out, size, "Not found: the defaults");
    } else if (s.bad == 0) {
        snprintf(out, size, "%s: %u settings read", s.source == 1 ? "SD card" : "Flash", (unsigned) s.good);
    } else {
        snprintf(out, size, "%s: %u not understood", s.source == 1 ? "SD card" : "Flash", (unsigned) s.bad);
    }
}
inline void describe_bad(const Settings& s, char* out, size_t size) {
    if (s.source == 0 || s.bad == 0) {
        out[0] = '\0';
    } else {
        if (s.bad_line == 0) snprintf(out, size, "%.30s", s.bad_text);
        else snprintf(out, size, "Line %u: %.20s", (unsigned) s.bad_line, s.bad_text);
    }
}

// The file Set up SD card writes: every setting at its default, lines of 30 letters at
// most so it can be edited on the Spectrum (E on it in Browse Games, with L-All on).
// DEFAULT_SETTINGS, then ORDER_HEADING, then DEFAULT_ORDER (or the lines of an order.txt
// already on the card, so the order it set is kept).
static const char DEFAULT_SETTINGS[] =
    "# PicoMenu settings\r\n"
    "# Change a value, or put # in\r\n"
    "# front to use the default.\r\n"
    "# O - Options has the choices.\r\n"
    "\r\n"
    "[Display]\r\n"
    "# dark or light\r\n"
    "theme = dark\r\n"
    "\r\n"
    "[Browse]\r\n"
    "# off, on or last\r\n"
    "show_all = off\r\n"
    "details = off\r\n"
    "# name, reverse or type\r\n"
    "sort = name\r\n"
    "# first, last or mixed\r\n"
    "folders = first\r\n"
    "# sd, flash or last\r\n"
    "start_drive = sd\r\n"
    "start_folder = /\r\n"
    "\r\n"
    "[Saving]\r\n"
    "# auto, sna, z80 or szx\r\n"
    "save_type = auto\r\n"
    "# S opens: slots or named\r\n"
    "save_screen = slots\r\n"
    "# ask or never\r\n"
    "slots_on_load = ask\r\n"
    "# grabs: scr, png or both\r\n"
    "grab_format = scr\r\n"
    "# autosave: off, 20, 40 or 60 seconds\r\n"
    "autosave = off\r\n"
    "\r\n"
    "[Buttons]\r\n"
    "# hold to reset: 2 to 5 seconds, or off\r\n"
    "long_press = 3.0\r\n"
    "# save screen on a 1 second hold or a double press (the other grabs)\r\n"
    "save_press = hold\r\n"
    "# on or off\r\n"
    "double_press = on\r\n"
    "\r\n"
    "[Converting]\r\n"
    "# on or off\r\n"
    "loading_grab = on\r\n"
    "\r\n"
    "[Waste Bin]\r\n"
    "# on or off\r\n"
    "waste_bin = on\r\n"
    "\r\n"
    "[Tapes]\r\n"
    "# multi-load levels: patch or off\r\n"
    "level_loaders = patch\r\n"
    "# 48K mode on a 128: tapes,\r\n"
    "# always or off\r\n"
    "lock_48k = tapes\r\n"
    "\r\n"
    "[WiFi]\r\n"
    "# a Pico W: F - File transfer\r\n"
    "# in Tools. The network's name\r\n"
    "# and password; web_password\r\n"
    "# (if set) for the web page\r\n"
    "ssid =\r\n"
    "password =\r\n"
    "web_password =\r\n"
    "\r\n"
    "[Bluetooth]\r\n"
    "# a Pico W: B - Bluetooth tape\r\n"
    "# volume: 1/2, 3/4 or full\r\n"
    "volume = 1/2\r\n"
    "# on or off. auto_connect: to\r\n"
    "# the receiver paired last;\r\n"
    "# auto_play: LOAD \"\" starts\r\n"
    "# the tape; auto_off: off when\r\n"
    "# the tape ends\r\n"
    "auto_connect = on\r\n"
    "auto_play = on\r\n"
    "auto_off = on\r\n"
    "\r\n";
static const char ORDER_HEADING[] =
    "[Screen grab order]\r\n";
static const char DEFAULT_ORDER[] =
    "# V shows a game's pictures\r\n"
    "# in this order. Put # in\r\n"
    "# front of a kind to hide it.\r\n"
    "From File\r\n"
    "Loading Screen\r\n"
    "Menu Screen\r\n"
    "Game Screen\r\n"
    "Ending Screen\r\n"
    "Standard Screen Number\r\n";

// A colour of the menus as the theme shows it. dark: as it is. light: a cell on black paper gets
// white paper (not bright, so all of it is the same white) and a dark ink that shows on it
// (white and green become black, yellow red, cyan blue); cells on coloured paper (the title
// bar, the key bars) are left as they are. Flash stays. The NMI ROM does the same (themeattr),
// so the menus it draws and the pico's screens match.
static const uint8_t LIGHT_INK[8] = { 7, 1, 2, 3, 0, 1, 2, 0 };
inline uint8_t theme_attr(uint8_t a, bool light) {
    if (!light || (a & 0x38) != 0) return a;
    return (uint8_t) ((a & 0x80) | 0x38 | LIGHT_INK[a & 7]);
}

// The order names are listed in, with the settings: folders (names ending '/') first, last or
// among the files; then A to Z (or Z to A), or by extension and then A to Z; not minding case.
// Names the same apart from case are then in ASCII order, and then by extension, so no two
// entries of a directory are equal. a and b are each the name and the extension as two strings.
inline int compare_names(const char* a, const char* b, uint8_t sort, uint8_t folders) {
    size_t la = strlen(a), lb = strlen(b);
    bool da = la > 0 && a[la - 1] == '/', db = lb > 0 && b[lb - 1] == '/';
    if (da != db && folders != FOLDERS_MIXED) {
        return (da == (folders == FOLDERS_FIRST)) ? -1 : 1;
    }
    const char* ea = a + la + 1;
    const char* eb = b + lb + 1;
    int c = 0;
    if (sort == SORT_TYPE) {
        // folders have no extension, so with folders mixed they come before the files
        c = strcasecmp(ea, eb);
    }
    if (c == 0) {
        if (folders != FOLDERS_MIXED) {
            c = strcasecmp(a, b);
        } else {
            // folders among files: by the name without the /, so "Exolon/" and "Exolon.z80"
            // are side by side (the folder first)
            size_t na = da ? la - 1 : la, nb = db ? lb - 1 : lb;
            size_t m = na < nb ? na : nb;
            c = strncasecmp(a, b, m);
            if (c == 0 && na != nb) c = na < nb ? -1 : 1;
            if (c == 0 && da != db) c = da ? -1 : 1;
        }
        if (c == 0) c = strcmp(a, b);
        if (c == 0) {
            c = strcasecmp(ea, eb);
            if (c == 0) c = strcmp(ea, eb);
        }
        if (sort == SORT_REVERSE) c = -c;
    }
    return c;
}

}   // namespace settings
