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

// A pretend network (pico/net.h) for picosim: the script says which file answers a URL
// ("net <url> <file>", a whole HTTP response: status line, headers, body; a URL ending in *
// answers any that starts so), and the answer is read through netget::Reader as the Pico W
// reads one, about 700 bytes each time the firmware's main loop goes round. A URL with no
// file is a name not found. Each request is counted (host_net_requests): nothing should be
// asked for that a key did not ask for.

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "net.h"
#include "netget.h"

namespace {
struct Answer { std::string url, file; };
std::vector<Answer> answers;
std::string data;
size_t at = 0;
netget::Reader reader;
netget::Url url_;
char next[netget::URL_MAX];
net::State st = net::IDLE;
const char* reason = "";
net::Body body_ = nullptr;
void* ctx_ = nullptr;
int redirects = 0;
int requests = 0;

bool body_cb(const uint8_t* d, size_t n, void*) { return body_ == nullptr || body_(d, n, ctx_); }

void fail(const char* why) { reason = why; st = net::FAILED; }

bool load(const char* url) {
    requests++;
    printf("NET %s\n", url);
    const Answer* best = nullptr;
    for (const Answer& a : answers) {
        bool star = !a.url.empty() && a.url.back() == '*';
        std::string u = star ? a.url.substr(0, a.url.size() - 1) : a.url;
        if (star ? strncmp(url, u.c_str(), u.size()) == 0 : u == url) {
            if (best == nullptr || a.url.size() > best->url.size()) best = &a;
        }
    }
    if (best == nullptr) return false;
    FILE* f = fopen(best->file.c_str(), "rb");
    if (f == nullptr) return false;
    data.clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    at = 0;
    reader.begin(body_cb, nullptr);
    return true;
}
}   // namespace

void host_net_answer(const char* url, const char* file) { answers.push_back({ url, file }); }
int host_net_requests() { return requests; }

namespace net {

const char* get(const char* url, Body body, void* ctx) {
    if (st == BUSY) return "Busy";
    if (!url_.parse(url)) return "Not a URL PicoMenu can get";
    body_ = body;
    ctx_ = ctx;
    redirects = 0;
    reason = "";
    st = BUSY;
    if (!load(url)) fail("The server's name was not found");
    return st == FAILED ? reason : nullptr;
}

void poll() {
    if (st != BUSY) return;
    size_t n = data.size() - at < 700 ? data.size() - at : 700;
    bool ok = n == 0 || reader.feed(reinterpret_cast<const uint8_t*>(data.data()) + at, n);
    at += n;
    if (!ok) { fail(reader.why()[0] != '\0' ? reader.why() : "The answer was not understood"); return; }
    if (!reader.done() && at < data.size()) return;
    if (!reader.done() && !reader.closed()) { fail(reader.why()); return; }
    if (reader.redirect()) {
        if (++redirects > 4) { fail("Too many redirections"); return; }
        if (!netget::resolve(url_, reader.location(), next, sizeof(next)) || !url_.parse(next)) { fail("A redirection PicoMenu can't follow"); return; }
        if (!load(next)) fail("The server's name was not found");
        return;
    }
    if (reader.status() != 200) { fail(reader.status() == 404 ? "Not found on the server" : "The server said no"); return; }
    st = DONE;
}

void stop() {
    if (st == BUSY) fail("Stopped");
    st = IDLE;
}

State state() { return st; }
const char* why() { return reason; }
int status() { return reader.status(); }
int64_t length() { return reader.length(); }
uint64_t received() { return reader.received(); }
bool verified() { return true; }
const char* host() { return url_.host; }
const char* io() { return ""; }
const char* detail() { return st == FAILED ? "DNS0.0 TCP0.0" : ""; }
Stage stage() { return at > 0 ? GET : WAIT; }
uint32_t elapsed_ms() { return 0; }
void* alloc(size_t n) { return malloc(n); }
void release(void* p) { free(p); }

}   // namespace net
