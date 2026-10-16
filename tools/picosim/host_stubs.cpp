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

// The Pico SDK, USB and bus pieces picoface.cxx calls, done for a PC (tools/picosim)
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/structs/bus_ctrl.h"

pio_hw_t host_pio0;
bus_ctrl_hw_t host_bus_ctrl;
static uint64_t host_us = 0;
extern "C" { int host_conv_slice_frames = 0; }  // sim "slice N": the converter runs N frames a request            // the simulated clock: sleep moves it on

extern "C" {
uint64_t time_us_64(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t) t.tv_sec * 1000000u + t.tv_nsec / 1000 + host_us;
}
uint32_t time_us_32(void) { return (uint32_t) time_us_64(); }
uint64_t get_absolute_time(void) { return time_us_64(); }
uint32_t to_ms_since_boot(uint64_t t) { return (uint32_t) (t / 1000); }
void sleep_ms(uint32_t ms) { host_us += (uint64_t) ms * 1000; }
void sleep_us(uint64_t us) { host_us += us; }
void busy_wait_us(uint64_t us) { host_us += us; }
void gpio_init(uint) {}
void gpio_set_dir(uint, bool) {}
void gpio_put(uint, bool) {}
bool gpio_get(uint) { return true; }
void gpio_pull_up(uint) {}
void gpio_set_function(uint, int) {}
bool stdio_init_all(void) { return true; }
void tight_loop_contents(void) {}
uint32_t save_and_disable_interrupts(void) { return 0; }
void restore_interrupts(uint32_t) {}
void pio_gpio_init(PIO, uint) {}
void pio_sm_set_consecutive_pindirs(PIO, uint, uint, uint, bool) {}
uint pio_add_program(PIO, const pio_program_t*) { return 0; }
void sm_config_set_in_pins(pio_sm_config*, uint) {}
void sm_config_set_in_shift(pio_sm_config*, bool, bool, uint) {}
void sm_config_set_sideset_pins(pio_sm_config*, uint) {}
void sm_config_set_out_pins(pio_sm_config*, uint, uint) {}
void pio_sm_init(PIO, uint, uint, const pio_sm_config*) {}
void pio_sm_set_enabled(PIO, uint, bool) {}
void pio_sm_claim(PIO, uint) {}
bool tusb_init(void) { return true; }
void piohandler(void) {}
}
void multicore_launch_core1(void (*)(void)) {}
// USB: not there on a PC
void pollUsbCommandHandler() {}
bool usb_msc_busy() { return false; }
bool usb_msc_pc_holds_card() { return false; }
void usb_msc_task() {}
