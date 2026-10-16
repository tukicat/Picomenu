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

// Speed test of the tape converter on a Cortex-M0+ (run in a simulator: see run.mjs).
// Converts the tape and ROM linked in (tape.inc, rom.inc) and stops with a BKPT; the
// simulator counts the clock cycles.
#include <stdint.h>
#include <string.h>
#include "tzxconv.h"

// BENCH128: rom.inc is a 32K 128.rom and the machine is a 128K
#ifdef BENCH128
static const uint8_t ROM[32768] = {
#else
static const uint8_t ROM[16384] = {
#endif
#include "rom.inc"
};
static const uint8_t TAPE[] = {
#include "tape.inc"
};

struct MemFile {
    uint32_t pos = 0;
    int read(void* p, uint32_t n) {
        if (pos >= sizeof(TAPE)) return 0;
        if (n > sizeof(TAPE) - pos) n = sizeof(TAPE) - pos;
        memcpy(p, TAPE + pos, n);
        pos += n;
        return (int) n;
    }
    bool seek(uint32_t p) { if (p > sizeof(TAPE)) return false; pos = p; return true; }
    uint32_t tell() { return pos; }
};

#ifdef BENCH128
static uint8_t ram[8 * 16384];
#else
static uint8_t ram[49152];
#endif
static MemFile tape;
static tzxconv::Machine<MemFile> m;

// read by the simulator
volatile uint32_t result[4] __attribute__((used));

extern "C" int main() {
#ifdef BENCH128
    uint8_t* banks[8];
    for (int i = 0; i < 8; i++) banks[i] = ram + i * 16384;
    m.begin128(ROM, ROM + 16384, banks, &tape, true, 2000);
#else
    m.begin(ROM, ram, &tape, true, 2000);
#endif
#ifdef BENCH_NOACCEL
    m.no_acceleration();
#endif
#ifdef BENCH_NOFLASH
    m.no_flash();
#endif
    tzxconv::Status s;
    while ((s = m.run(1)) == tzxconv::Status::RUNNING) {
        result[1] = m.stats().frames;
        __asm volatile ("bkpt #1");             // one frame done: the simulator notes the cycles
    }
    result[0] = (uint32_t) s;
    result[1] = m.stats().frames;
#ifdef BENCH128
    result[2] = ram[2 * 16384 + 0x1000] == 'G' && ram[2 * 16384 + 0x1001] == 'A';    // 0x9000 is bank 2
#else
    result[2] = ram[0x9000 - 0x4000] == 'G' && ram[0x9001 - 0x4000] == 'A';
#endif
    __asm volatile ("bkpt #2");
    for (;;) {}
}

// start up: .data copied, .bss cleared, then main
extern uint32_t __data_load, __data_start, __data_end, __bss_start, __bss_end, __stack_top;
extern "C" void reset() {
    uint32_t* s = &__data_load;
    for (uint32_t* d = &__data_start; d < &__data_end;) *d++ = *s++;
    for (uint32_t* d = &__bss_start; d < &__bss_end;) *d++ = 0;
    extern void (*__init_array_start[])(), (*__init_array_end[])();
    for (auto f = __init_array_start; f < __init_array_end; f++) (*f)();
    main();
}
__attribute__((section(".vectors"), used)) void* const vectors[2] = { &__stack_top, (void*) reset };
