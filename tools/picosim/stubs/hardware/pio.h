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
#include "pico/stdlib.h"
typedef struct { int dummy; } pio_hw_t;
typedef pio_hw_t* PIO;
typedef struct { uint32_t c; } pio_sm_config;
typedef struct { const uint16_t* instructions; uint8_t length; int8_t origin; } pio_program_t;
extern pio_hw_t host_pio0;
#define pio0 (&host_pio0)
#define pio1 (&host_pio0)
#ifdef __cplusplus
extern "C" {
#endif
void pio_gpio_init(PIO pio, uint pin);
void pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pin, uint count, bool out);
uint pio_add_program(PIO pio, const pio_program_t* p);
void sm_config_set_in_pins(pio_sm_config* c, uint pin);
void sm_config_set_in_shift(pio_sm_config* c, bool right, bool autopush, uint threshold);
void sm_config_set_sideset_pins(pio_sm_config* c, uint pin);
void sm_config_set_out_pins(pio_sm_config* c, uint pin, uint count);
void pio_sm_init(PIO pio, uint sm, uint offset, const pio_sm_config* c);
void pio_sm_set_enabled(PIO pio, uint sm, bool enabled);
void pio_sm_claim(PIO pio, uint sm);
#ifdef __cplusplus
}
#endif
