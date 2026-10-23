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


// Save slots: up to nine saves of each game, in a folder of its own on the SD card,
//   /Save Games/<game>/Slot 1.z80, /Save Games/<game>/Slot 2 - Zone 3.sna ...
// (the label after " - " is the user's). The slot used last is the name of an empty folder in
// the same folder, ".last3" (a dot name: never listed). Nothing here touches the hardware, so it
// is tested on a PC.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

namespace slots {

static const int COUNT = 9;
static const size_t LABEL_MAX = 16;         // letters of a label
static const char FOLDER[] = "/Save Games";
static const char LAST_FILE[] = ".last";       // then the digit: ".last3", a folder

// The slot a file's name (without the extension) is, 1 to 9, and its label (may be empty):
// 0 if it is not a slot's name. "Slot 3", "slot 3 - Zone 3".
inline int parse(const char* name, char* label, size_t label_size) {
    if (strncasecmp(name, "Slot ", 5) != 0 || name[5] < '1' || name[5] > '9') return 0;
    int n = name[5] - '0';
    const char* rest = name + 6;
    if (label != nullptr && label_size > 0) label[0] = '\0';
    if (*rest == '\0') return n;
    if (strncmp(rest, " - ", 3) != 0 || rest[3] == '\0') return 0;
    if (label != nullptr && label_size > 0) snprintf(label, label_size, "%s", rest + 3);
    return n;
}

// A slot's file name, without the extension
inline void name(int n, const char* label, char* out, size_t size) {
    if (label != nullptr && label[0] != '\0') snprintf(out, size, "Slot %d - %s", n, label);
    else snprintf(out, size, "Slot %d", n);
}

// Letters a label may have: those FAT allows in a name, and no dots (the extension)
inline bool label_char(int c) {
    return c >= 32 && c < 127 && strchr("\\/:*?\"<>|.", c) == nullptr;
}

// A label as typed: no blanks at the ends. Returns its length.
inline size_t tidy_label(char* label) {
    size_t n = strlen(label), a = 0;
    while (a < n && label[a] == ' ') a++;
    while (n > a && label[n - 1] == ' ') n--;
    memmove(label, label + a, n - a);
    label[n - a] = '\0';
    return n - a;
}

// Whether a folder (same form as sd_cwd) is a game's slot folder: /Save Games/<game>
inline bool is_slot_folder(const char* dir) {
    size_t fl = sizeof(FOLDER) - 1;
    return strncasecmp(dir, FOLDER, fl) == 0 && dir[fl] == '/' && dir[fl + 1] != '\0' &&
        strchr(dir + fl + 1, '/') == nullptr;
}

// The game a file in a slot folder is (the folder's name), or null
inline const char* folder_game(const char* dir) {
    return is_slot_folder(dir) ? dir + sizeof(FOLDER) : nullptr;
}

// The digit after ".last": 1 to 9, or 0
inline int parse_last(const char* text, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (text[i] >= '1' && text[i] <= '9') return text[i] - '0';
        if (text[i] != ' ' && text[i] != '\r' && text[i] != '\n') return 0;
    }
    return 0;
}

}   // namespace slots
