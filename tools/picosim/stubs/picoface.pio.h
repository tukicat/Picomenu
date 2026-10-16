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

#pragma once
#include "hardware/pio.h"
static const pio_program_t fetchaddr_program = { 0, 0, -1 };
static const pio_program_t putdata_program = { 0, 0, -1 };
static inline pio_sm_config fetchaddr_program_get_default_config(uint offset) { (void) offset; pio_sm_config c = { 0 }; return c; }
static inline pio_sm_config putdata_program_get_default_config(uint offset) { (void) offset; pio_sm_config c = { 0 }; return c; }
