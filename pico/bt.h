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

// The Pico W's Bluetooth (bt.cxx, only in the Pico W build): sends sound to a Bluetooth
// speaker or receiver (A2DP, as a phone does), such as a Bluetooth receiver wired to a
// Spectrum's EAR socket. PicoMenu makes the sound (a tape, tapeaudio.h); this finds the
// receiver, pairs with it, remembers it, and keeps the sound going at the rate it plays.

#ifndef PICOMENU_BT_H
#define PICOMENU_BT_H

#include <stdint.h>
#include <stddef.h>

namespace bt {

enum State : uint8_t {
    OFF,
    STARTING,       // the chip being started
    READY,          // on, not connected
    SEARCHING,      // looking for speakers (about 10 s)
    CONNECTING,     // to one (and pairing with it the first time)
    CONNECTED,      // sound is going to it (silence until the tape plays)
    FAILED,         // why(): the chip did not start
};

// The sound, as it is needed: n samples (mono, 16 bits) at rate()
typedef void (*Source)(int16_t* out, uint32_t n, void* ctx);

// Starts the chip. arena: memory the WiFi's network code is given while it starts (it
// writes two words there, the start and 64 KB on: wifi.h's lwIP), lent only for this call.
// Null, or why not.
const char* start(uint8_t* arena, size_t size, Source source, void* ctx);
void poll();            // from the main loop, as often as it goes round
// Clears the Bluetooth's memory (BTstack's variables, in the end of snap_load_buffer in the Pico
// W build), as at the Pico's start: before the radio chip is started (start(), and the WiFi's)
void radio_clear();
void stop();            // disconnected and off
State state();
const char* why();      // FAILED, or the last connection that went wrong ("" if none)

// Searching: the speakers (and other sound devices) found so far, newest last
static const int FOUND_MAX = 8;
struct Found {
    uint8_t addr[6];
    char name[32];          // "" until it is known
};
void search();
int found_count();
const Found& found(int i);

void connect(int i);                    // one of found()
bool connect_saved();                   // the one paired last (false: none)
void disconnect();
const char* saved_name();               // the one paired last ("" if none)
void forget();                          // and its pairing
const char* connected_name();
uint32_t rate();                        // CONNECTED: 44100 or 48000
bool mono();                            // CONNECTED: the sound is sent as mono (else stereo)
uint32_t late_ms();                     // the longest the sound has had to catch up (the main loop held up)

// A connection was lost, or one could not be made: the stack is to be started afresh (stop(),
// start()) before it is used again, as BTstack is not always tidy after one (cleared by start())
bool lost();

// The crash recorder (kept in RAM over a restart): what the Bluetooth did last, as small codes
// (note(); picoface.cxx adds its own), and, if the Pico crashed (a fault: where) or hung (the
// watchdog, which is on while the Bluetooth is), what had happened. crash_boot() at the start.
void note(uint8_t code);
void crash_boot();
bool crashed();                         // the last restart was a crash or a hang
void crash_text(int line, char* out, size_t size);  // line 0: what and where, 1: the last events
void crash_clear();

}   // namespace bt

#endif
