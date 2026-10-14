// Screen grabs: the screen of a running game saved in the folder "Screen Grabs" in
// the root of the SD card, as <game>.scr (the 6912 bytes of the Spectrum screen, which V
// in Browse Games shows). A second grab of the same game is <game>1.scr, then <game>2.scr
// and so on.
// Nothing here touches the hardware, so it is tested on a PC.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

namespace screengrab {

static const char FOLDER[] = "/Screen Grabs";
// The scraper program (scripts/scrape_game_info) saves the loading screens it downloads in a
// "Screen Grabs" folder beside the Instructions file, so with the Instructions file in the
// folder Scraper they are here. V shows pictures from both folders (these first); the grabs
// the Picoface makes go in FOLDER.
static const char SCRAPER_FOLDER[] = "/Scraper/Screen Grabs";
static const size_t SCR_SIZE = 6912;
static const int MAX_SCAN = 40;         // the numbers looked at for one game (none, 1 ... 39)
static const int MAX_PICTURES = 8;      // the most V cycles through for one game
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

// "<folder>/<base>" then nothing for number 0, or the number, then the extension (with its dot)
inline void path_in(const char* folder, const char* base, int number, const char* ext, char* out, size_t outsize) {
    if (number == 0) {
        snprintf(out, outsize, "%s/%s%s", folder, base, ext);
    } else {
        snprintf(out, outsize, "%s/%s%d%s", folder, base, number, ext);
    }
}

// The same in the Picoface's own folder, "/Screen Grabs"
inline void path_for(const char* base, int number, const char* ext, char* out, size_t outsize) {
    path_in(FOLDER, base, number, ext, out, outsize);
}

// The text the Pico shows after a grab: "Saved as <name>", cut to fit a line of 31
inline void saved_message(const char* base, int number, char* out, size_t outsize) {
    char name[NAME_MAX + 8];
    if (number == 0) {
        snprintf(name, sizeof(name), "%s", base);
    } else {
        snprintf(name, sizeof(name), "%s%d", base, number);
    }
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
