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

// One HTTPS request at a time, while the WiFi is up (wifi.h): net.cxx in the Pico W build
// (lwIP's altcp_tls with mbedtls, mbedtls_config.h; the roots it trusts are netroots.h), a
// pretend one in the simulator (tools/picosim/net_host.cpp: files given by its script). The
// response is read by netget.h; redirects are followed (archive.org sends one), up to four.
// Only when PicoMenu's user asks for something (a search, a title, a file): nothing runs by
// itself.

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace net {

enum State : uint8_t { IDLE, BUSY, DONE, FAILED };
// how far a request has got: the server's name looked up, connecting, the TLS handshake, the
// request sent and waiting, the answer coming
enum Stage : uint8_t { DNS, TCP, TLS, WAIT, GET, STAGES };
typedef bool (*Body)(const uint8_t* data, size_t n, void* ctx);    // false stops it

// Starts getting url; the body (of a 200 answer) goes to body as it comes. Null, or why not.
const char* get(const char* url, Body body, void* ctx);
void poll();                // from the main loop, as often as it goes round (its time limit)
void stop();                // the request let go, and the TLS settings: before the WiFi goes off
State state();
const char* why();          // FAILED: why
int status();               // the HTTP status (200 ...)
int64_t length();           // the body's length, -1 if not known
uint64_t received();        // the body's bytes so far
bool verified();            // DONE: the server's certificate was checked against the roots
const char* host();         // the server being asked (for the screen)
const char* detail();       // FAILED: how long each stage took ("DNS 0.1 TCP 0.2 TLS 19.8 s5")
const char* io();           // FAILED after the request was sent: the connection's bytes since
Stage stage();              // BUSY: where it has got to
uint32_t elapsed_ms();      // BUSY: since it was asked for

// Memory from lwIP's heap (in snap_load_buffer while the WiFi is on), for a while (unzipping a
// download): null if there is not that much
void* alloc(size_t n);
void release(void* p);

}   // namespace net
