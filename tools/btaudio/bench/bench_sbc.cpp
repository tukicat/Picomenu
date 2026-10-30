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

// Speed of the Bluetooth tape's sound on a Cortex-M0+ (run in rp2040js, as
// tools/tzxconv/bench): the tape linked in (tape.inc) made into sound (tapeaudio.h) and coded
// with SBC (BTstack's encoder, built as the firmware builds it), a second of sound at a time
// (a BKPT after each, where the simulator notes the cycles). BENCH_STEREO: joint stereo
// (bitpool 53), else mono (bitpool 53, the most a receiver usually allows).
#include <stdint.h>
#include <string.h>
#include "tapeaudio.h"
extern "C" {
#include "btstack_sbc.h"
}

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
static MemFile tape;
static tapeaudio::Audio<MemFile> audio;
static btstack_sbc_encoder_state_t enc;
volatile uint32_t result[4] __attribute__((used));

extern "C" int main() {
    audio.begin(&tape, BENCH_TZX, 44100);
#ifdef BENCH_STEREO
    btstack_sbc_encoder_init(&enc, SBC_MODE_STANDARD, 16, 8, SBC_ALLOCATION_METHOD_LOUDNESS, 44100, 53, SBC_CHANNEL_MODE_JOINT_STEREO);
#else
    btstack_sbc_encoder_init(&enc, SBC_MODE_STANDARD, 16, 8, SBC_ALLOCATION_METHOD_LOUDNESS, 44100, 53, SBC_CHANNEL_MODE_MONO);
#endif
    uint32_t bytes = 0;
    for (int sec = 0; sec < 20; sec++) {
        for (int f = 0; f < 345; f++) {             // 128 samples a frame: 345 is a second
            int16_t pcm[256];
#ifdef BENCH_STEREO
            int16_t m[128];
            audio.fill(m, 128, 30000);
            for (int i = 0; i < 128; i++) pcm[2 * i] = pcm[2 * i + 1] = m[i];
#else
            audio.fill(pcm, 128, 30000);
#endif
            btstack_sbc_encoder_process_data(pcm);
            bytes += btstack_sbc_encoder_sbc_buffer_length();
        }
        result[1] = bytes;
        __asm volatile ("bkpt #1");
    }
    result[0] = 0;
    __asm volatile ("bkpt #2");
    for (;;) {}
}

extern "C" void btstack_assert_failed(const char*, uint16_t) { for (;;) {} }
extern "C" void hci_dump_log(int, const char*, ...) {}

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
