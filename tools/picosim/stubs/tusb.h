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
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool tusb_init(void);
void tud_task(void);
bool tud_cdc_connected(void);
uint32_t tud_cdc_available(void);
uint32_t tud_cdc_read(void* buf, uint32_t n);
uint32_t tud_cdc_write(const void* buf, uint32_t n);
uint32_t tud_cdc_write_flush(void);
bool tud_mounted(void);
#ifdef __cplusplus
}
#endif
