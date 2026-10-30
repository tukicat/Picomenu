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

// The WiFi of a Pico W (wifi.cxx, only in the Pico W build): joins the network Picomenu.ini
// names and serves the file transfer's page (webfiles.h) on port 80. It is on only while F -
// File transfer is open in Tools; all its memory (lwIP's, the connections') is lent to it
// then, from snap_load_buffer, which is free while the menu is up.

#ifndef PICOMENU_WIFI_H
#define PICOMENU_WIFI_H

#include <stdint.h>
#include <stddef.h>
#include "webfiles.h"

namespace wifi {

enum State : uint8_t { OFF, JOINING, UP, FAILED };

// Starts joining the network: ssid, password ("" for an open one), the web page's password
// ("": none). arena: the memory lent (size bytes, 8-aligned, at least ARENA_MIN). Null, or why not.
const char* start(const char* ssid, const char* password, const char* web_password,
    uint8_t* arena, size_t size, const web::Hooks* hooks);
void poll();            // from the main loop, as often as it goes round
void stop();            // the connections closed (an upload not finished is not kept), WiFi off
State state();
const char* address();  // when UP: "192.168.1.57"
const char* name();     // "picomenu": http://picomenu.local (mDNS), http://picomenu/ (NetBIOS)
const char* why();      // when FAILED
int busy();             // connections open

static const size_t ARENA_MIN = 96 * 1024;

// The Bluetooth (bt.cxx) starts the chip as the WiFi does, which starts lwIP the first time
// (it writes its heap's start and end): it is lent arena for that, and lwIP is started afresh
// when the WiFi is next used. False: too small.
bool lend_lwip(uint8_t* arena, size_t size);

}   // namespace wifi

#endif
