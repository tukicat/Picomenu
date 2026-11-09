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

// HTTP/1.1 for the ZXDB downloads (net.h): a URL taken apart, the request, and the response
// read as it comes (the status, the headers it needs, the body as it is: Content-Length,
// chunked, or up to the end of the connection), with nothing about how the bytes travel, so it
// is tested on a PC (tools/nettest). One request on each connection (Connection: close).
//
// Every request says who is asking, as the ZXInfo API asks its users to (be a good API citizen:
// identify yourself with your own User-Agent): USER_AGENT.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

namespace netget {

static const char USER_AGENT[] = "PicoMenu/2.6.0 (Picoface for the ZX Spectrum)";
static const size_t HOST_MAX = 64, PATH_MAX_ = 480, URL_MAX = 560;

// https://host[:port]/path (http:// too, for tests). False if it is not one.
struct Url {
    char host[HOST_MAX];
    char path[PATH_MAX_];
    uint16_t port;
    bool tls;
    bool parse(const char* url) {
        const char* p = url;
        if (strncasecmp(p, "https://", 8) == 0) { tls = true; port = 443; p += 8; }
        else if (strncasecmp(p, "http://", 7) == 0) { tls = false; port = 80; p += 7; }
        else return false;
        size_t h = strcspn(p, "/:?");
        if (h == 0 || h >= HOST_MAX) return false;
        memcpy(host, p, h);
        host[h] = '\0';
        p += h;
        if (*p == ':') {
            unsigned v = 0;
            p++;
            while (*p >= '0' && *p <= '9') v = v * 10 + (unsigned) (*p++ - '0');
            if (v == 0 || v > 65535) return false;
            port = (uint16_t) v;
        }
        if (*p == '\0') p = "/";
        if (*p != '/' && *p != '?') return false;
        if (strlen(p) + (*p == '?' ? 1 : 0) >= PATH_MAX_) return false;
        snprintf(path, sizeof(path), "%s%s", *p == '?' ? "/" : "", p);
        return true;
    }
};

// The request (GET, one only, then the server closes): its length, 0 if out is too small
inline size_t request(const Url& u, char* out, size_t size) {
    int n = snprintf(out, size,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\nAccept-Encoding: identity\r\n"
        "Connection: close\r\n\r\n", u.path, u.host, USER_AGENT);
    return n > 0 && (size_t) n < size ? (size_t) n : 0;
}

// The response, a byte at a time. body() gets the body's bytes as they come (after the
// headers; chunked taken apart); it returns false to stop (the reader then fails).
class Reader {
public:
    enum State : uint8_t { STATUS, HEADERS, BODY, CHUNK_SIZE, CHUNK_DATA, CHUNK_END, TRAILER, DONE, FAILED };
    typedef bool (*Body)(const uint8_t* data, size_t n, void* ctx);

    void begin(Body body, void* ctx) {
        body_ = body;
        ctx_ = ctx;
        st_ = STATUS;
        status_ = 0;
        length_ = -1;
        got_ = 0;
        chunked_ = false;
        line_n_ = 0;
        location_[0] = '\0';
        why_ = "";
    }

    // the bytes received; false once it has failed
    bool feed(const uint8_t* p, size_t n) {
        while (n > 0 && st_ != DONE && st_ != FAILED) {
            if (st_ == BODY || st_ == CHUNK_DATA) {
                size_t take = n;
                if (st_ == BODY && length_ >= 0 && (int64_t) take > length_ - (int64_t) got_) take = (size_t) (length_ - (int64_t) got_);
                if (st_ == CHUNK_DATA && take > chunk_left_) take = (size_t) chunk_left_;
                if (take > 0 && is_body_wanted() && !body_(p, take, ctx_)) return fail("Stopped");
                got_ += take;
                p += take;
                n -= take;
                if (st_ == BODY && length_ >= 0 && (int64_t) got_ >= length_) st_ = DONE;
                if (st_ == CHUNK_DATA && (chunk_left_ -= take) == 0) st_ = CHUNK_END;
                continue;
            }
            // a line at a time (the status, the headers, a chunk's size)
            char c = (char) *p++;
            n--;
            if (c == '\r') continue;
            if (c != '\n') {
                if (line_n_ + 1 < sizeof(line_)) line_[line_n_++] = c;
                continue;
            }
            line_[line_n_] = '\0';
            size_t len = line_n_;
            line_n_ = 0;
            if (!on_line(line_, len)) return false;
        }
        return st_ != FAILED;
    }

    // the connection closed: the body ends there if it had no length
    bool closed() {
        if (st_ == BODY && length_ < 0) st_ = DONE;
        if (st_ != DONE && st_ != FAILED) fail(st_ == STATUS || st_ == HEADERS ? "No answer" : "Cut short");
        return st_ == DONE;
    }

    State state() const { return st_; }
    bool done() const { return st_ == DONE; }
    bool failed() const { return st_ == FAILED; }
    int status() const { return status_; }
    bool headers_done() const { return st_ >= BODY; }
    int64_t length() const { return length_; }         // -1 not known (chunked, or no length)
    uint64_t received() const { return got_; }
    // a redirection (301, 302, 303, 307, 308 with a Location): where to
    bool redirect() const { return (status_ == 301 || status_ == 302 || status_ == 303 || status_ == 307 || status_ == 308) && location_[0] != '\0'; }
    const char* location() const { return location_; }
    const char* why() const { return why_; }

private:
    bool is_body_wanted() const { return status_ == 200; }

    bool fail(const char* why) {
        why_ = why;
        st_ = FAILED;
        return false;
    }

    static bool has_word(const char* v, const char* w) {
        size_t k = strlen(w);
        for (; *v != '\0'; v++) {
            if (strncasecmp(v, w, k) == 0) return true;
        }
        return false;
    }

    static bool is_header(const char* l, size_t n, const char* name, const char** value) {
        size_t k = strlen(name);
        if (n <= k || strncasecmp(l, name, k) != 0 || l[k] != ':') return false;
        const char* v = l + k + 1;
        while (*v == ' ' || *v == '\t') v++;
        *value = v;
        return true;
    }

    bool on_line(const char* l, size_t n) {
        const char* v;
        switch (st_) {
        case STATUS:
            if (strncmp(l, "HTTP/1.", 7) != 0 || n < 12) return fail("Not an HTTP answer");
            status_ = (l[9] - '0') * 100 + (l[10] - '0') * 10 + (l[11] - '0');
            st_ = HEADERS;
            return true;
        case HEADERS:
            if (n == 0) {
                // the end of the headers: the body (none for a redirection: the connection is let go)
                if (redirect() || status_ == 204 || status_ == 304) { st_ = DONE; return true; }
                st_ = chunked_ ? CHUNK_SIZE : BODY;
                if (st_ == BODY && length_ == 0) st_ = DONE;
                return true;
            }
            if (is_header(l, n, "Content-Length", &v)) {
                int64_t x = 0;
                while (*v >= '0' && *v <= '9') x = x * 10 + (*v++ - '0');
                length_ = x;
            } else if (is_header(l, n, "Transfer-Encoding", &v)) {
                chunked_ = has_word(v, "chunked");
                if (chunked_) length_ = -1;
            } else if (is_header(l, n, "Location", &v)) {
                snprintf(location_, sizeof(location_), "%s", v);
            }
            return true;
        case CHUNK_SIZE: {
            uint64_t x = 0;
            int digits = 0;
            for (const char* q = l; *q != '\0' && *q != ';'; q++, digits++) {
                char c = *q;
                int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (d < 0 || digits > 12) return fail("Bad chunk");
                x = x * 16 + (uint64_t) d;
            }
            if (digits == 0) return fail("Bad chunk");
            if (x == 0) { st_ = TRAILER; return true; }
            chunk_left_ = x;
            st_ = CHUNK_DATA;
            return true;
        }
        case CHUNK_END:
            if (n != 0) return fail("Bad chunk");
            st_ = CHUNK_SIZE;
            return true;
        case TRAILER:
            if (n == 0) st_ = DONE;
            return true;
        default:
            return true;
        }
    }

    Body body_ = nullptr;
    void* ctx_ = nullptr;
    State st_ = STATUS;
    int status_ = 0;
    int64_t length_ = -1;
    uint64_t got_ = 0;
    uint64_t chunk_left_ = 0;
    bool chunked_ = false;
    char line_[URL_MAX];
    size_t line_n_ = 0;
    char location_[URL_MAX];
    const char* why_ = "";
};

// A Location (absolute, or a path on the same server) made into a whole URL in out
inline bool resolve(const Url& from, const char* location, char* out, size_t size) {
    if (strncasecmp(location, "https://", 8) == 0 || strncasecmp(location, "http://", 7) == 0) {
        return (size_t) snprintf(out, size, "%s", location) < size;
    }
    if (location[0] != '/') return false;
    return (size_t) snprintf(out, size, "%s://%s%s", from.tls ? "https" : "http", from.host, location) < size;
}

// Text for a URL: letters, digits and -_.~ as they are, the rest %XX (a search term, a path)
inline bool encode(const char* text, char* out, size_t size, bool keep_slash) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t k = 0;
    for (const unsigned char* p = (const unsigned char*) text; *p != '\0'; p++) {
        bool plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~' || (keep_slash && *p == '/');
        if (k + (plain ? 1 : 3) >= size) return false;
        if (plain) out[k++] = (char) *p;
        else { out[k++] = '%'; out[k++] = HEX[*p >> 4]; out[k++] = HEX[*p & 15]; }
    }
    out[k] = '\0';
    return true;
}

}   // namespace netget
