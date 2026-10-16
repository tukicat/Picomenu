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

// Host stand-ins for the Pico SDK, enough for picoface.cxx to build on a PC (tools/picosim)
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned int uint;
void sleep_ms(uint32_t ms);
void sleep_us(uint64_t us);
uint64_t time_us_64(void);
uint32_t time_us_32(void);
uint32_t to_ms_since_boot(uint64_t t);
uint64_t get_absolute_time(void);
void gpio_init(uint gpio);
void gpio_set_dir(uint gpio, bool out);
void gpio_put(uint gpio, bool value);
bool gpio_get(uint gpio);
void gpio_pull_up(uint gpio);
void gpio_set_function(uint gpio, int fn);
bool stdio_init_all(void);
void tight_loop_contents(void);
uint32_t save_and_disable_interrupts(void);
void restore_interrupts(uint32_t);
void busy_wait_us(uint64_t);
#ifdef __cplusplus
}
#endif
#define __not_in_flash_func(f) f
#define __no_inline_not_in_flash_func(f) f
#define __time_critical_func(f) f
#define GPIO_FUNC_SIO 5
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#define XIP_BASE 0x10000000
#define XIP_NOCACHE_NOALLOC_BASE 0x13000000
#define FLASH_PAGE_SIZE 256
#define FLASH_SECTOR_SIZE 4096
