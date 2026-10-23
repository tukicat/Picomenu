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


// C on the NMI menu: set up the SD card. It makes the folders the Picoface uses and, if there
// is no Instructions file yet, a small example one in the folder Scraper, and if there is no
// Picomenu.ini, one with every setting at its default (settings.h). Nothing that is
// already there is changed. The tables and the text are here, so they are tested on a PC;
// the work itself (picoface.cxx, sd_setup_run) uses the file system.

#pragma once

namespace sdsetup {

struct Folder {
    const char* path;           // in the root of the card
    const char* what;           // shown on the screen (15 letters at most)
    bool hidden;                // made hidden (the Waste Bin)
    const char* label = nullptr;    // shown instead of the path (without its /), if given
};

static const Folder FOLDERS[] = {
    { "/Scraper",      "Instructions.txt", false },
    { "/Scraper/Screen Grabs", " screen grabs", false, " Screen Grabs" },     // under Scraper/ on the screen
    { "/Game Notes",   "your game notes",  false },
    { "/Save Games",   "quick saves",      false },
    { "/Favourites",   "favourite games",  false },
    { "/Poke List",    ".pok cheat files", false },
    { "/Tape Files",   "tape positions",   false },
    { "/Recordings",   "recorded tapes",   false },
    { "/Waste Bin",    "erased files",     true },      // hidden: Browse Games shows it with L-All
    { "/ROMS",         "ROM images",       true },      // hidden too: 48.rom, 128.rom, testrom.rom, DiagROM.rom, others for C-Change ROM
};
static const int FOLDER_COUNT = (int) (sizeof(FOLDERS) / sizeof(FOLDERS[0]));

// Where the Instructions are read from, first to last (gameinfo, info_read_file)
static const char* const INSTRUCTIONS_PATHS[] = {
    "/Scraper/Instructions.txt", "/Scraper/Instructions", "/Instructions.txt", "/Instructions"
};
static const int INSTRUCTIONS_PATH_COUNT = 4;
static const char EXAMPLE_PATH[] = "/Scraper/Instructions.txt";

// The example. Lines starting with ; are ignored by the Picoface.
static const char EXAMPLE[] =
    "; Game information for the PicoMenu (V in Browse Games, I on the menu).\r\n"
    "; Lines that start with ; are not shown. For each game write its name\r\n"
    "; (the file's name without .sna, .z80, .tap and so on), then the software\r\n"
    "; house and year, then Controls and Instructions, like the example below.\r\n"
    "; Delete the example and put your own games here, or run the scraper program\r\n"
    "; on this file to fill in the software house, year and pictures.\r\n"
    "\r\n"
    "Example Game\r\n"
    "Software house: Example Soft\r\n"
    "Year: 1985\r\n"
    "Controls\r\n"
    "Q up  A down\r\n"
    "O left  P right\r\n"
    "SPACE fire\r\n"
    "Instructions\r\n"
    "Collect all the keys, then reach the exit before the time runs out.\r\n";

// What happened to each item
enum Result : unsigned char { THERE = 0, MADE = 1, FAILED = 2 };

}   // namespace sdsetup
