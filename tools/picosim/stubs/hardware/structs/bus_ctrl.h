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
typedef struct { uint32_t priority; } bus_ctrl_hw_t;
extern bus_ctrl_hw_t host_bus_ctrl;
#define bus_ctrl_hw (&host_bus_ctrl)
#define BUSCTRL_BUS_PRIORITY_PROC1_BITS 0x10
