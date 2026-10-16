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

// See tzxconv_pico.h
#include <new>
#include "file_wrapper.h"
#include "tzxconv.h"
#include "tzxconv_pico.h"

namespace pconv {

typedef tzxconv::Machine<FileHandle> M;
static_assert(sizeof(M) <= STATE_SIZE, "the converter's machine does not fit");
static_assert((int) RESET == (int) tzxconv::Status::RESET && (int) SAVE == (int) tzxconv::Status::SAVE,
              "the status numbers differ");

static M* m(void* s) { return static_cast<M*>(s); }

void begin48(void* s, const uint8_t* rom, uint8_t* ram, FileHandle* tape, bool tzx) {
    new (s) M();
    m(s)->begin(rom, ram, tape, tzx);
}

void begin128(void* s, const uint8_t* rom0, const uint8_t* rom1, uint8_t* const banks[8],
              FileHandle* tape, bool tzx) {
    new (s) M();
    m(s)->begin128(rom0, rom1, banks, tape, tzx);
}

Status run(void* s, uint32_t frames) { return (Status) m(s)->run(frames); }
Suspect suspect(void* s) { return (Suspect) m(s)->suspect(); }
uint32_t frames(void* s) { return m(s)->stats().frames; }
void sna_header(void* s, uint8_t* h, uint16_t& pc) { m(s)->sna_header(h, pc); }
const uint8_t* screen(void* s) { return m(s)->screen(); }
bool loading_screen(void* s) { return m(s)->loading_screen(); }
const uint8_t* screen_data(void* s) { return m(s)->screen_data(); }
void no_screen_watch(void* s) { m(s)->no_screen_watch(); }
uint8_t port_7ffd(void* s) { return m(s)->port_7ffd(); }
const uint8_t* ay_registers(void* s) { return m(s)->ay_registers(); }
uint8_t ay_selected(void* s) { return m(s)->ay_selected(); }

}
