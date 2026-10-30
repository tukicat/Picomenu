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


// O-Options (in Tools): the settings of Picomenu.ini and the screen grab order, changed on the
// Spectrum. This is what is shown and changed, and how the file is written again: only the
// values that were changed, in their lines (a setting the file does not have goes at the end of
// its section, or a new section); comments, blank lines and anything else stay as they were.
// Nothing here touches the hardware, so it is tested on a PC.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

#include "settings.h"
#include "screengrab.h"

namespace options {

enum Id : uint8_t {
    THEME, SHOW_ALL, DETAILS, SORT, FOLDERS, START_DRIVE, START_FOLDER,
    SAVE_TYPE, SAVE_SCREEN, SLOTS_ON_LOAD, GRAB_FORMAT, CHECKPOINTS,
    LONG_PRESS, SAVE_PRESS, DOUBLE_PRESS, LOADING_GRAB, WASTE_BIN, LEVEL_LOADERS, LOCK_48K,
    // the WiFi and Bluetooth page (W): the WiFi's typed, the Bluetooth's chosen
    WIFI_SSID, WIFI_PASSWORD, WIFI_WEB_PASSWORD,
    BT_VOLUME, BT_AUTO_CONNECT, BT_AUTO_PLAY, BT_AUTO_OFF, COUNT
};
static const uint8_t MAIN_COUNT = WIFI_SSID;            // the items of the settings page
static const uint8_t WIFI_FIRST = WIFI_SSID, WIFI_COUNT = COUNT - WIFI_SSID;    // (the W page: all of them)
inline bool typed(uint8_t id) { return id >= WIFI_SSID && id <= WIFI_WEB_PASSWORD; }

struct Item {
    const char* key;            // as in the file
    uint8_t section;            // settings::Section
    const char* hint;           // a line on the screen, 30 letters at most
};

static const Item ITEMS[COUNT] = {
    { "theme", settings::SEC_DISPLAY, "[Display] dark or light menus" },
    { "show_all", settings::SEC_BROWSE, "[Browse] L-All when it opens" },
    { "details", settings::SEC_BROWSE, "[Browse] O-Info when it opens" },
    { "sort", settings::SEC_BROWSE, "[Browse] the order of names" },
    { "folders", settings::SEC_BROWSE, "[Browse] where folders go" },
    { "start_drive", settings::SEC_BROWSE, "[Browse] the drive it opens on" },
    { "start_folder", settings::SEC_BROWSE, "[Browse] / or last folder used" },
    { "save_type", settings::SEC_SAVING, "[Saving] the snapshot type" },
    { "save_screen", settings::SEC_SAVING, "[Saving] what S opens" },
    { "slots_on_load", settings::SEC_SAVING, "[Saving] ask for a slot" },
    { "grab_format", settings::SEC_SAVING, "[Saving] screen grab files" },
    { "autosave", settings::SEC_SAVING, "[Saving] autosave every (s)" },
    { "long_press", settings::SEC_BUTTONS, "[Buttons] hold to reset (s)" },
    { "save_press", settings::SEC_BUTTONS, "[Buttons] save: hold or double" },
    { "double_press", settings::SEC_BUTTONS, "[Buttons] a double press" },
    { "loading_grab", settings::SEC_CONVERTING, "[Converting] grab the screen" },
    { "waste_bin", settings::SEC_BIN, "[Waste Bin] erase to the bin" },
    { "level_loaders", settings::SEC_TAPES, "[Tapes] multi-load levels" },
    { "lock_48k", settings::SEC_TAPES, "[Tapes] 48K mode on a 128" },
    { "ssid", settings::SEC_WIFI, "[WiFi] the network's name" },
    { "password", settings::SEC_WIFI, "[WiFi] the network's password" },
    { "web_password", settings::SEC_WIFI, "[WiFi] for the web page" },
    { "volume", settings::SEC_BLUETOOTH, "[Bluetooth] the tape's volume" },
    { "auto_connect", settings::SEC_BLUETOOTH, "[Bluetooth] to the last paired" },
    { "auto_play", settings::SEC_BLUETOOTH, "[Bluetooth] LOAD \"\" plays it" },
    { "auto_off", settings::SEC_BLUETOOTH, "[Bluetooth] off at tape's end" },
};

static const char* const SECTION_NAME[] = { "", "Browse", "Saving", "Buttons", "Converting", "Waste Bin", "Screen grab order", "Display", "Tapes", "WiFi", "Bluetooth" };
static_assert(sizeof(SECTION_NAME) / sizeof(SECTION_NAME[0]) == settings::SEC_UNKNOWN, "a name for each section");

static const size_t FOLDER_MAX = 64;

// What the screen shows and changes
struct Edit {
    settings::Settings s;
    char start_folder[FOLDER_MAX] = "";             // "/" or "/Games"; "" the file has none
    uint8_t list[screengrab::CATEGORIES] = {};      // the screen grab order, first to last
    bool shown[screengrab::CATEGORIES] = {};        // by category
    settings::Wifi wifi = {};                       // [WiFi] (a Pico W's file transfer)

    // The order the pictures are in, from what the file (or order.txt) says
    void set_order(screengrab::Order order) {
        order.finish();
        for (int c = 0; c < screengrab::CATEGORIES; c++) {
            list[order.rank[c]] = (uint8_t) c;
            shown[c] = order.shown[c];
        }
    }
};

// The value as it is written in the file
inline void value_text(const Edit& e, uint8_t id, char* out, size_t size) {
    static const char* const SHOW[] = { "off", "on", "last" };
    static const char* const SORT_W[] = { "name", "reverse", "type" };
    static const char* const FOLD_W[] = { "first", "last", "mixed" };
    static const char* const DRIVE_W[] = { "flash", "sd", "last" };
    static const char* const TYPE_W[] = { "auto", "sna", "z80", "szx" };
    const settings::Settings& s = e.s;
    const char* w = "";
    switch (id) {
    case THEME: w = s.light ? "light" : "dark"; break;
    case SHOW_ALL: w = SHOW[s.show_all % 3]; break;
    case DETAILS: w = SHOW[s.details % 3]; break;
    case SORT: w = SORT_W[s.sort % 3]; break;
    case FOLDERS: w = FOLD_W[s.folders % 3]; break;
    case START_DRIVE: w = DRIVE_W[s.start_drive % 3]; break;
    case START_FOLDER: w = e.start_folder; break;
    case SAVE_TYPE: w = TYPE_W[s.save_type % 4]; break;
    case SAVE_SCREEN: w = s.save_screen_slots ? "slots" : "named"; break;
    case SLOTS_ON_LOAD: w = s.slots_on_load ? "ask" : "never"; break;
    case GRAB_FORMAT: w = s.grab_format == 1 ? "png" : s.grab_format == 2 ? "both" : "scr"; break;
    case CHECKPOINTS:
        if (s.checkpoint_s == 0) w = "off";
        else { snprintf(out, size, "%u", (unsigned) s.checkpoint_s); return; }
        break;
    case LONG_PRESS:
        if (s.long_press_ms == 0) {
            w = "off";
        } else {
            unsigned ms = s.long_press_ms;
            if (ms % 100 == 0) snprintf(out, size, "%u.%u", ms / 1000, (ms % 1000) / 100);
            else if (ms % 10 == 0) snprintf(out, size, "%u.%02u", ms / 1000, (ms % 1000) / 10);
            else snprintf(out, size, "%u.%03u", ms / 1000, ms % 1000);
            return;
        }
        break;
    case SAVE_PRESS: w = s.save_on_hold ? "hold" : "double"; break;
    case DOUBLE_PRESS: w = s.double_press ? "on" : "off"; break;
    case LOADING_GRAB: w = s.loading_grab ? "on" : "off"; break;
    case WASTE_BIN: w = s.waste_bin ? "on" : "off"; break;
    case LEVEL_LOADERS: w = s.level_loaders ? "patch" : "off"; break;
    case LOCK_48K: w = s.lock_48k == 0 ? "off" : s.lock_48k == 2 ? "always" : "tapes"; break;
    case WIFI_SSID: w = e.wifi.ssid; break;
    case WIFI_PASSWORD: w = e.wifi.password; break;
    case WIFI_WEB_PASSWORD: w = e.wifi.web_password; break;
    case BT_VOLUME: w = s.bt_volume == 1 ? "3/4" : s.bt_volume == 2 ? "full" : "1/2"; break;
    case BT_AUTO_CONNECT: w = s.bt_auto_connect ? "on" : "off"; break;
    case BT_AUTO_PLAY: w = s.bt_auto_play ? "on" : "off"; break;
    case BT_AUTO_OFF: w = s.bt_auto_off ? "on" : "off"; break;
    default: break;
    }
    snprintf(out, size, "%s", w);
}

// C: the next value. folder_now is the folder Browse Games was last in on the SD card ("" the
// root, else "/Games"): start_folder goes between it and the root.
inline void cycle(Edit& e, uint8_t id, const char* folder_now) {
    settings::Settings& s = e.s;
    switch (id) {
    case THEME: s.light = !s.light; break;
    case SHOW_ALL: s.show_all = (uint8_t) ((s.show_all + 1) % 3); break;
    case DETAILS: s.details = (uint8_t) ((s.details + 1) % 3); break;
    case SORT: s.sort = (uint8_t) ((s.sort + 1) % 3); break;
    case FOLDERS: s.folders = (uint8_t) ((s.folders + 1) % 3); break;
    case START_DRIVE: s.start_drive = (uint8_t) ((s.start_drive + 1) % 3); break;
    case START_FOLDER: {
        const char* now = folder_now[0] == '\0' ? "/" : folder_now;
        if (strcmp(e.start_folder, now) != 0 && strlen(now) < FOLDER_MAX) snprintf(e.start_folder, FOLDER_MAX, "%s", now);
        else snprintf(e.start_folder, FOLDER_MAX, "/");
        break;
    }
    case SAVE_TYPE: s.save_type = (uint8_t) ((s.save_type + 1) % 4); break;
    case SAVE_SCREEN: s.save_screen_slots = !s.save_screen_slots; break;
    case SLOTS_ON_LOAD: s.slots_on_load = !s.slots_on_load; break;
    case GRAB_FORMAT: s.grab_format = (uint8_t) ((s.grab_format + 1) % 3); break;
    case CHECKPOINTS: s.checkpoint_s = s.checkpoint_s == 0 ? 20 : s.checkpoint_s == 20 ? 40 : s.checkpoint_s == 40 ? 60 : 0; break;
    case LONG_PRESS:
        // 2.0, 2.5 ... 5.0 seconds, then off; a time between steps goes to the next step
        if (s.long_press_ms == 0) s.long_press_ms = settings::LONG_PRESS_MIN;
        else if (s.long_press_ms >= settings::LONG_PRESS_MAX) s.long_press_ms = 0;
        else s.long_press_ms = (uint16_t) ((s.long_press_ms / 500 + 1) * 500);
        break;
    case SAVE_PRESS: s.save_on_hold = !s.save_on_hold; break;
    case DOUBLE_PRESS: s.double_press = !s.double_press; break;
    case LOADING_GRAB: s.loading_grab = !s.loading_grab; break;
    case WASTE_BIN: s.waste_bin = !s.waste_bin; break;
    case LEVEL_LOADERS: s.level_loaders = !s.level_loaders; break;
    case LOCK_48K: s.lock_48k = (uint8_t) ((s.lock_48k + 1) % 3); break;
    case BT_VOLUME: s.bt_volume = (uint8_t) ((s.bt_volume + 1) % 3); break;
    case BT_AUTO_CONNECT: s.bt_auto_connect = !s.bt_auto_connect; break;
    case BT_AUTO_PLAY: s.bt_auto_play = !s.bt_auto_play; break;
    case BT_AUTO_OFF: s.bt_auto_off = !s.bt_auto_off; break;
    default: break;
    }
}

inline bool changed(const Edit& a, const Edit& b, uint8_t id) {
    char x[FOLDER_MAX], y[FOLDER_MAX];
    value_text(a, id, x, sizeof(x));
    value_text(b, id, y, sizeof(y));
    return strcmp(x, y) != 0;
}

// The categories shown, in order (the file only keeps those: a hidden one is a comment, and is
// put after the shown ones when it is read again)
inline int shown_order(const Edit& e, uint8_t* out) {
    int n = 0;
    for (int i = 0; i < screengrab::CATEGORIES; i++) {
        if (e.shown[e.list[i]]) out[n++] = e.list[i];
    }
    return n;
}

inline bool order_changed(const Edit& a, const Edit& b) {
    uint8_t x[screengrab::CATEGORIES], y[screengrab::CATEGORIES];
    int nx = shown_order(a, x), ny = shown_order(b, y);
    return nx != ny || memcmp(x, y, (size_t) nx) != 0;
}

inline bool any_changed(const Edit& a, const Edit& b) {
    for (uint8_t i = 0; i < COUNT; i++) {
        if (changed(a, b, i)) return true;
    }
    return order_changed(a, b);
}

// The order page: move the category at place i up (-1) or down (+1); false if it can't go
inline bool move(Edit& e, int i, int by) {
    int j = i + by;
    if (i < 0 || i >= screengrab::CATEGORIES || j < 0 || j >= screengrab::CATEGORIES) return false;
    uint8_t t = e.list[i];
    e.list[i] = e.list[j];
    e.list[j] = t;
    return true;
}

// Hides or shows the category at place i; false if it is the last one shown (with none shown,
// V would show them all)
inline bool toggle(Edit& e, int i) {
    uint8_t c = e.list[i];
    if (e.shown[c]) {
        int n = 0;
        for (int k = 0; k < screengrab::CATEGORIES; k++) n += e.shown[k] ? 1 : 0;
        if (n <= 1) return false;
    }
    e.shown[c] = !e.shown[c];
    return true;
}

// Writing the file again ------------------------------------------------------------------

struct Out {
    char* p;
    size_t cap, n = 0;
    bool full = false;
    const char* eol;
    Out(char* to, size_t size, bool crlf) : p(to), cap(size), eol(crlf ? "\r\n" : "\n") {}
    void put(const char* t, size_t len) {
        if (n + len > cap) { full = true; return; }
        memcpy(p + n, t, len);
        n += len;
    }
    void put(const char* t) { put(t, strlen(t)); }
    void line(const char* t, size_t len) { put(t, len); put(eol); }
    void line(const char* t) { line(t, strlen(t)); }
};

inline bool is_blank_char(char c) { return c == ' ' || c == '\t' || c == '\r'; }

inline uint8_t section_of(const char* nm, size_t nn) {
    for (uint8_t s = 1; s < sizeof(SECTION_NAME) / sizeof(SECTION_NAME[0]); s++) {
        if (strlen(SECTION_NAME[s]) == nn && strncasecmp(nm, SECTION_NAME[s], nn) == 0) return s;
    }
    return settings::SEC_UNKNOWN;
}

// A line of the order section that is a category, shown or not ("# Menu Screen")
inline bool is_order_line(const char* t, size_t n) {
    size_t a = 0;
    while (a < n && (t[a] == '#' || is_blank_char(t[a]))) a++;
    while (n > a && is_blank_char(t[n - 1])) n--;
    if (a == n) return false;
    screengrab::Order o;
    return o.line(t + a, n - a);
}

struct Rewriter {
    const Edit& before;
    const Edit& after;
    Out& out;
    bool done[COUNT] = {};
    bool order_done = false;
    uint8_t section = settings::SEC_NONE;
    bool seen[settings::SEC_UNKNOWN] = {};   // the sections the file has

    Rewriter(const Edit& b, const Edit& a, Out& o) : before(b), after(a), out(o) {}

    void setting_line(uint8_t id) {
        char v[FOLDER_MAX];
        value_text(after, id, v, sizeof(v));
        out.put(ITEMS[id].key);
        out.put(" = ");
        out.line(v);
        done[id] = true;
    }

    void order_lines() {
        for (int i = 0; i < screengrab::CATEGORIES; i++) {
            uint8_t c = after.list[i];
            if (!after.shown[c]) out.put("# ");
            out.line(screengrab::ORDER_NAME[c]);
        }
        order_done = true;
    }

    // The section that is ending: the settings changed that it did not have
    void finish_section() {
        if (section == settings::SEC_ORDER && !order_done && order_changed(before, after)) order_lines();
        for (uint8_t i = 0; i < COUNT; i++) {
            if (ITEMS[i].section == section && !done[i] && changed(before, after, i)) setting_line(i);
        }
    }

    void run(const char* in, size_t n) {
        size_t i = 0;
        size_t blank_from = 0, blanks = 0;     // blank lines held back, so additions go before them
        while (i < n) {
            size_t e = i;
            while (e < n && in[e] != '\n' && in[e] != '\r') e++;
            const char* t = in + i;
            size_t len = e - i;
            size_t next = e < n ? e + 1 : e;
            if (e + 1 < n && in[e] == '\r' && in[e + 1] == '\n') next = e + 2;
            line(in, t, len, blank_from, blanks, i);
            i = next;
        }
        finish_section();
        flush_blanks(in, n, blank_from, blanks);
        // sections the file did not have
        for (uint8_t s = 1; s < settings::SEC_UNKNOWN; s++) {
            if (seen[s] || s == settings::SEC_ORDER) continue;
            bool need = false;
            for (uint8_t k = 0; k < COUNT; k++) need = need || (ITEMS[k].section == s && changed(before, after, k));
            if (!need) continue;
            out.line("");
            out.put("[");
            out.put(SECTION_NAME[s]);
            out.line("]");
            section = s;
            finish_section();
        }
        if (!seen[settings::SEC_ORDER] && order_changed(before, after)) {
            out.line("");
            out.line("[Screen grab order]");
            const char* d = settings::DEFAULT_ORDER;    // its comment lines
            while (*d == '#') {
                const char* nl = strchr(d, '\n');
                size_t l = nl ? (size_t) (nl - d) : strlen(d);
                while (l > 0 && d[l - 1] == '\r') l--;
                out.line(d, l);
                d = nl ? nl + 1 : d + strlen(d);
            }
            order_lines();
        }
    }

    void flush_blanks(const char* in, size_t n, size_t& from, size_t& count) {
        size_t i = from;
        while (count > 0 && i < n) {
            size_t e = i;
            while (e < n && in[e] != '\n' && in[e] != '\r') e++;
            out.line(in + i, e - i);
            i = (e + 1 < n && in[e] == '\r' && in[e + 1] == '\n') ? e + 2 : e + 1;
            count--;
        }
        count = 0;
    }

    void line(const char* in, const char* t, size_t len, size_t& blank_from, size_t& blanks, size_t at) {
        size_t a = 0, b = len;
        if (at == 0 && len >= 3 && (unsigned char) t[0] == 0xEF && (unsigned char) t[1] == 0xBB && (unsigned char) t[2] == 0xBF) a = 3;
        while (a < b && is_blank_char(t[a])) a++;
        while (b > a && is_blank_char(t[b - 1])) b--;
        if (a == b) {                           // blank: held back
            if (blanks == 0) blank_from = at;
            blanks++;
            return;
        }
        const char* u = t + a;
        size_t un = b - a;
        if (u[0] == '[' && u[un - 1] == ']') {
            finish_section();
            flush_blanks(in, at, blank_from, blanks);
            size_t x = 1, y = un - 1;
            while (x < y && is_blank_char(u[x])) x++;
            while (y > x && is_blank_char(u[y - 1])) y--;
            section = section_of(u + x, y - x);
            if (section < settings::SEC_UNKNOWN) seen[section] = true;
            out.line(t, len);
            return;
        }
        flush_blanks(in, at, blank_from, blanks);
        if (section == settings::SEC_ORDER && order_changed(before, after)) {
            if (is_order_line(u, un)) {
                if (!order_done) order_lines();         // the new order where the old one began
                return;
            }
            out.line(t, len);
            return;
        }
        if (u[0] != '#' && u[0] != ';') {
            const char* eq = (const char*) memchr(u, '=', un);
            if (eq != nullptr) {
                size_t kn = (size_t) (eq - u);
                while (kn > 0 && is_blank_char(u[kn - 1])) kn--;
                for (uint8_t id = 0; id < COUNT; id++) {
                    if (ITEMS[id].section == section && strlen(ITEMS[id].key) == kn &&
                            strncasecmp(u, ITEMS[id].key, kn) == 0 && changed(before, after, id)) {
                        // the key as it was written, the new value, and a " # comment" after it
                        const char* v = eq + 1;
                        const char* end = u + un;
                        const char* note = nullptr;
                        for (const char* q = v + 1; q < end; q++) {
                            if (*q == '#' && is_blank_char(q[-1])) { note = q; break; }
                        }
                        char val[FOLDER_MAX];
                        value_text(after, id, val, sizeof(val));
                        out.put(t, (size_t) (eq + 1 - t));
                        out.put(" ");
                        out.put(val);
                        if (note != nullptr) {
                            out.put("  ");
                            out.put(note, (size_t) (end - note));
                        }
                        out.put(out.eol);
                        done[id] = true;
                        return;
                    }
                }
            }
        }
        out.line(t, len);
    }
};

// in: the file as it is (n = 0: there is none). Writes the new file to out; false if it
// does not fit.
inline bool rewrite(const char* in, size_t n, const Edit& before, const Edit& after, char* to, size_t cap, size_t* written) {
    bool crlf = n == 0 || memchr(in, '\r', n) != nullptr;
    Out out(to, cap, crlf);
    if (n == 0) {
        out.line("# PicoMenu settings");
    }
    Rewriter r(before, after, out);
    r.run(in, n);
    *written = out.n;
    return !out.full;
}

}   // namespace options
