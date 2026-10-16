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

MEMORY { FLASH (rx) : ORIGIN = 0x10000000, LENGTH = 2048K  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 256K }
SECTIONS {
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) . = ALIGN(4);
            __init_array_start = .; KEEP(*(.init_array*)) __init_array_end = .; } > FLASH
  .data : { __data_start = .; *(.data*) . = ALIGN(4); __data_end = .; } > RAM AT > FLASH
  __data_load = LOADADDR(.data);
  .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; } > RAM
  __stack_top = ORIGIN(RAM) + LENGTH(RAM);
  /DISCARD/ : { *(.ARM.exidx*) }
}
