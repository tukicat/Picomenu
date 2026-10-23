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

// The tape converter (tzxconv.h) for the firmware, behind plain functions: it is built in
// its own file (tzxconv_pico.cxx) at -O2, as the emulation is the one thing that has to be
// fast, while picoface.cxx is built for size.

#ifndef INCLUDED_TZXCONV_PICO_H
#define INCLUDED_TZXCONV_PICO_H

#include <stdint.h>
#include <stddef.h>

class FileHandle;

namespace pconv {

// The machine is kept in memory the caller gives: at least STATE_SIZE bytes, 8-aligned
static const size_t STATE_SIZE = 1024;

// The same numbers as tzxconv::Status and tzxconv::Suspect
enum Status : uint8_t { RUNNING, LOADED, TAPE_ERROR, BAD_TAPE, TOO_LONG, STOPPED, BASIC_ERROR, RESET, SAVE };
enum Suspect : uint8_t { NONE, STILL_READING, IN_BASIC };

// A 48K: rom 16384 bytes, ram 49152 (0x4000 first). The tape file is open at its start.
void begin48(void* state, const uint8_t* rom, uint8_t* ram, FileHandle* tape, bool tzx);
// A 128K: the editor ROM and the 48K BASIC ROM, the eight banks
void begin128(void* state, const uint8_t* rom0, const uint8_t* rom1, uint8_t* const banks[8],
              FileHandle* tape, bool tzx);
// Runs up to `frames` frames (20 ms of Spectrum time each)
Status run(void* state, uint32_t frames);
Suspect suspect(void* state);
uint32_t frames(void* state);
// The registers as a .sna header (27 bytes, the PC not pushed) and the PC
void sna_header(void* state, uint8_t* h, uint16_t& pc);
// The screen being shown (the 128K's bank 5 or 7)
const uint8_t* screen(void* state);
// The loading screen: true once, after the run() in which the tape's loading screen was seen
// loaded (run() comes back early then). It is then the 6912 bytes at screen_data() (0x4000).
bool loading_screen(void* state);
const uint8_t* screen_data(void* state);
void no_screen_watch(void* state);         // stop looking for it
uint8_t port_7ffd(void* state);
const uint8_t* ay_registers(void* state);
uint8_t ay_selected(void* state);
// Where the tape has got to for a later load: the data blocks up to the last one loaded (so
// the next load takes the one after), as the tape trap counts them (tape_done in picoface.cxx)
uint32_t tape_position(void* state);
// True if the conversion stopped with the tape still playing to no one (the rest of it is for
// later: a multi-load game's next levels)
bool stopped_early(void* state);

}

#endif
