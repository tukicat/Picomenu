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

// A pretend Bluetooth (pico/bt.h) for picosim: two receivers are found by a search, connecting
// to one works at once (mono, 44.1 kHz), and the sound the firmware makes for it goes to the
// emulated Spectrum's EAR input (host_bt_ear), sample by sample as time passes, as a receiver
// wired to the EAR socket would play it. The SBC coding over the air is left out here: it is
// checked on its own by tools/btaudio.

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "bt.h"

namespace bt {

namespace {
State st = OFF;
Source source_ = nullptr;
void* ctx_ = nullptr;
const char* reason = "";
Found list[FOUND_MAX];
int list_n = 0;
bool have_saved = false;
char saved[32];
char peer[32];
int search_polls = 0;
int connect_polls = 0;
bool lost_ = false;
const uint32_t RATE = 44100;
// the sound: samples made so far, and the last one (what the EAR sees)
uint64_t made = 0;
int16_t last = 0;
int ear = 0;
int16_t chunk[128];
int chunk_at = 128;
}

void radio_clear() {}

const char* start(uint8_t*, size_t, Source source, void* ctx) {
    source_ = source;
    ctx_ = ctx;
    lost_ = false;
    reason = "";
    if (st == OFF) st = READY;
    printf("BT: on\n");
    return nullptr;
}

void poll() {
    if (st == CONNECTING && ++connect_polls > 500) {
        st = CONNECTED;
        printf("BT: connected to %s (paired before)\n", peer);
    }
    if (st == SEARCHING && ++search_polls > 3) {
        list_n = 2;
        memset(list, 0, sizeof(list));
        snprintf(list[0].name, sizeof(list[0].name), "Speccy BT receiver");
        list[0].addr[0] = 1;
        snprintf(list[1].name, sizeof(list[1].name), "Kitchen speaker");
        list[1].addr[0] = 2;
        st = READY;
    }
}

void stop() {
    st = OFF;
    printf("BT: off\n");
}

State state() { return st; }
const char* why() { return reason; }

void search() {
    if (st != READY) return;
    st = SEARCHING;
    search_polls = 0;
    list_n = 0;
}

int found_count() { return list_n; }
const Found& found(int i) { return list[i]; }

void connect(int i) {
    if (i < 0 || i >= list_n) return;
    snprintf(peer, sizeof(peer), "%s", list[i].name);
    snprintf(saved, sizeof(saved), "%s", list[i].name);
    have_saved = true;
    st = CONNECTED;
    printf("BT: connected to %s\n", peer);
}

// (connecting again to the one paired last takes a moment, as with a real receiver: a load can
// come while it connects)
bool connect_saved() {
    if (!have_saved) return false;
    snprintf(peer, sizeof(peer), "%s", saved);
    st = CONNECTING;
    connect_polls = 0;
    return true;
}

void disconnect() { if (st == CONNECTED) st = READY; }
const char* saved_name() { return have_saved ? saved : ""; }
void forget() { have_saved = false; }
const char* connected_name() { return peer; }
uint32_t rate() { return RATE; }
bool mono() { return true; }
uint32_t late_ms() { return 0; }
bool lost() { return lost_; }

// the crash recorder: nothing to record in the simulator
void note(uint8_t) {}
void crash_boot() {}
bool crashed() { return false; }
void crash_text(int, char* out, size_t) { out[0] = '\0'; }
void crash_clear() {}

}   // namespace bt

// The EAR input at time t (T-states at 3.5 MHz since the start): the sound is made up to then
// (while connected), and the level is the last sample's sign, with a little hysteresis as the
// Spectrum's input has
int host_bt_ear(uint64_t t) {
    using namespace bt;
    uint64_t want = t * RATE / 3500000;
    if (st != CONNECTED) {
        made = want;
        return ear;
    }
    while (made < want) {
        if (chunk_at >= 128) {
            if (source_ != nullptr) source_(chunk, 128, ctx_);
            else memset(chunk, 0, sizeof(chunk));
            chunk_at = 0;
        }
        last = chunk[chunk_at++];
        made++;
    }
    if (last > 2000) ear = 1;
    else if (last < -2000) ear = 0;
    return ear;
}

// The simulator's btdrop: the receiver goes away (switched off, out of range, taken by a phone)
void host_bt_drop() {
    using namespace bt;
    if (st != CONNECTED && st != CONNECTING) return;
    st = READY;
    reason = "The receiver went away";
    lost_ = true;
    printf("BT: the receiver went away\n");
}
