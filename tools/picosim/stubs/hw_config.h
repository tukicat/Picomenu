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
// host: no SD card driver; the card is a RAM disk under FatFs (tools/picosim)
#include <stddef.h>
#include "ff.h"
typedef struct { const char* pcName; FATFS fatfs; } sd_card_t;
#ifdef __cplusplus
extern "C" {
#endif
sd_card_t* sd_get_by_num(size_t num);
#ifdef __cplusplus
}
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
