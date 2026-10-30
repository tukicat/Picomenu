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

// The file transfer's web side (F in Tools, a Pico W): one HTTP connection, which gets the
// request's bytes as they come and gives the reply's bytes as there is room for them. It knows
// nothing of WiFi (wifi.cxx carries the bytes; the simulator feeds it requests by hand). The SD
// card is read and written with FatFs; erasing and renaming go through PicoMenu (hooks), so
// the Waste Bin and the tape notes work as in Browse Games.
//
// The browser gets one page (PAGE) and then asks for these, each on a connection of its own
// that is closed when the reply has gone ("Connection: close"):
//   GET  /                        the page
//   GET  /api/list?p=/Games       the folder as JSON: {"path":"/Games","items":[{"n":"a.z80","d":0,"s":49179},...]}
//   GET  /api/get?p=/Games/a.z80  the file
//   PUT  /api/put?p=/Games/a.z80[&o=1]   the body is the file (o=1: over one already there)
//   POST /api/mkdir?p=/Games/New
//   POST /api/del?p=/Games/a.z80  (to the Waste Bin, as E in Browse Games)
//   POST /api/ren?p=/Games/a.z80&to=b.z80
// With web_password set in Picomenu.ini every request needs it (HTTP Basic: the browser asks).

#ifndef PICOMENU_WEBFILES_H
#define PICOMENU_WEBFILES_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "ff.h"

namespace web {

struct Hooks {
    bool (*card_ok)();                                  // the SD card is there, and the PC does not have it
    bool (*erase)(const char* path, bool is_dir);       // false: could not
    bool (*rename)(const char* from, const char* to, bool is_dir);
    void (*event)(char what, const char* path);         // 'S' sent, 'R' received, 'E' erased, 'N' renamed, 'M' made
};

static const size_t PATH_SIZE = 256;
static const size_t LINE_SIZE = 1024;                   // the request line (the path, %-encoded) and each header
static const size_t OUT_SIZE = 2048;

extern const char PAGE[];

// base64 (RFC 4648) of in into out; returns false if it is not base64 or out is too small
inline bool base64_decode(const char* in, size_t n, char* out, size_t size) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else return false;
        acc = (acc << 6) | (uint32_t) v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o + 1 >= size) return false;
            out[o++] = (char) ((acc >> bits) & 0xFF);
        }
    }
    out[o] = '\0';
    return true;
}

// %XX and + of a query value, into out (size). False if it does not fit or has a 0.
inline bool url_decode(const char* in, size_t n, char* out, size_t size) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (i + 2 >= n) return false;
            auto hex = [](char h) -> int {
                return h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
            };
            int a = hex(in[i + 1]), b = hex(in[i + 2]);
            if (a < 0 || b < 0) return false;
            c = (char) (a * 16 + b);
            i += 2;
        }
        if (c == '\0' || o + 1 >= size) return false;
        out[o++] = c;
    }
    out[o] = '\0';
    return true;
}

// The value of name in a query ("p=/Games&o=1"), decoded. False if it is not there (or bad).
inline bool query_value(const char* q, const char* name, char* out, size_t size) {
    size_t nl = strlen(name);
    const char* s = q;
    while (s != nullptr && *s != '\0') {
        const char* amp = strchr(s, '&');
        size_t len = amp ? (size_t) (amp - s) : strlen(s);
        if (len > nl && strncmp(s, name, nl) == 0 && s[nl] == '=') {
            return url_decode(s + nl + 1, len - nl - 1, out, size);
        }
        s = amp ? amp + 1 : nullptr;
    }
    return false;
}

// A path the browser may use: from the root, no "..", no backslashes or control characters.
// "/" stays "/"; a / at the end goes.
inline bool path_ok(char* p) {
    size_t n = strlen(p);
    if (n == 0 || p[0] != '/') return false;
    while (n > 1 && p[n - 1] == '/') p[--n] = '\0';
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) p[i];
        if (c < 32 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
        if (c == '/' && i + 1 < n && p[i + 1] == '/') return false;
        if (c == '/' && p[i + 1] == '.' && (p[i + 2] == '/' || p[i + 2] == '\0' ||
                (p[i + 2] == '.' && (p[i + 3] == '/' || p[i + 3] == '\0')))) {
            return false;
        }
    }
    return true;
}

// The last part of a path
inline const char* base_of(const char* p) {
    const char* s = strrchr(p, '/');
    return s ? s + 1 : p;
}

class Conn {
public:
    void begin(const Hooks* hooks, const char* web_password) {
        hooks_ = hooks;
        password_ = web_password;
        step_ = REQUEST;
        line_len_ = 0;
        content_length_ = 0;
        has_length_ = false;
        body_got_ = 0;
        auth_ok_ = password_ == nullptr || password_[0] == '\0';
        out_head_ = out_tail_ = 0;
        job_ = NONE;
        file_open_ = dir_open_ = false;
        done_ = false;
        method_[0] = path_[0] = query_[0] = '\0';
    }

    // The connection went before the reply was done: an upload not finished is not kept
    void abort() {
        if (job_ == RECEIVE && file_open_) {
            f_close(&file_);
            f_unlink(path_);
        } else if (file_open_) {
            f_close(&file_);
        }
        if (dir_open_) f_closedir(&dir_);
        file_open_ = dir_open_ = false;
        job_ = NONE;
        done_ = true;
    }

    // The request's bytes, as they come
    void feed(const uint8_t* d, size_t n) {
        size_t i = 0;
        while (i < n && !done_) {
            if (step_ == BODY) {
                size_t want = content_length_ - body_got_;
                size_t take = n - i < want ? n - i : want;
                body(d + i, take);
                i += take;
                body_got_ += take;
                if (body_got_ >= content_length_) body_end();
                continue;
            }
            if (step_ != REQUEST && step_ != HEADERS) return;       // the reply has begun: the rest is not looked at
            char c = (char) d[i++];
            if (c == '\n') {
                while (line_len_ > 0 && line_[line_len_ - 1] == '\r') line_len_--;
                line_[line_len_] = '\0';
                got_line();
                line_len_ = 0;
            } else if (line_len_ + 1 < LINE_SIZE) {
                line_[line_len_++] = c;
            } else {
                reply(414, "text/plain", "Too long", 8);
            }
        }
    }

    // What is ready to send: *p and how many bytes (all in one piece)
    size_t ready(const uint8_t** p) {
        pump();
        *p = out_ + out_head_;
        return out_tail_ - out_head_;
    }

    // n of them were taken
    void sent(size_t n) {
        out_head_ += n;
        if (out_head_ >= out_tail_) out_head_ = out_tail_ = 0;
    }

    // The reply has gone: the connection can be closed
    bool finished() {
        pump();
        return done_ && out_head_ == out_tail_;
    }

private:
    enum Step : uint8_t { REQUEST, HEADERS, BODY, REPLY };
    enum Job : uint8_t { NONE, SEND_PAGE, SEND_FILE, SEND_LIST, RECEIVE, DISCARD };

    void put(const char* t, size_t n) {
        if (out_tail_ + n > OUT_SIZE) {             // (keep what is waiting at the front)
            memmove(out_, out_ + out_head_, out_tail_ - out_head_);
            out_tail_ -= out_head_;
            out_head_ = 0;
        }
        if (out_tail_ + n > OUT_SIZE) n = OUT_SIZE - out_tail_;
        memcpy(out_ + out_tail_, t, n);
        out_tail_ += n;
    }
    void put(const char* t) { put(t, strlen(t)); }
    size_t room() const { return OUT_SIZE - (out_tail_ - out_head_); }

    // The status line and headers; length < 0: none (the end of the connection ends it)
    void head(int code, const char* type, long length, const char* extra = nullptr) {
        char h[200];
        const char* why = code == 200 ? "OK" : code == 401 ? "Unauthorized" : code == 404 ? "Not Found" :
            code == 409 ? "Conflict" : code == 503 ? "Service Unavailable" : code == 405 ? "Method Not Allowed" : "Bad Request";
        int n = snprintf(h, sizeof(h), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\nConnection: close\r\n",
            code, why, type);
        put(h, (size_t) n);
        if (length >= 0) {
            n = snprintf(h, sizeof(h), "Content-Length: %ld\r\n", length);
            put(h, (size_t) n);
        }
        if (extra != nullptr) put(extra);
        put("\r\n");
        step_ = REPLY;
    }

    void reply(int code, const char* type, const char* body, size_t n, const char* extra = nullptr) {
        if (file_open_) { f_close(&file_); file_open_ = false; }
        head(code, type, (long) n, extra);
        put(body, n);
        job_ = NONE;
        done_ = true;
    }
    void reply_text(int code, const char* text) { reply(code, "text/plain; charset=utf-8", text, strlen(text)); }

    void got_line() {
        if (step_ == REQUEST) {
            if (line_len_ == 0) return;             // (a blank line before it)
            // "GET /api/list?p=%2FGames HTTP/1.1"
            char* sp1 = strchr(line_, ' ');
            char* sp2 = sp1 ? strchr(sp1 + 1, ' ') : nullptr;
            if (sp1 == nullptr || sp2 == nullptr || (size_t) (sp1 - line_) >= sizeof(method_)) {
                reply_text(400, "Bad request");
                return;
            }
            *sp1 = '\0';
            *sp2 = '\0';
            snprintf(method_, sizeof(method_), "%s", line_);
            char* target = sp1 + 1;
            char* q = strchr(target, '?');
            if (q != nullptr) *q++ = '\0';
            if (strlen(target) >= sizeof(route_) || (q != nullptr && strlen(q) >= sizeof(query_))) {
                reply_text(414, "Too long");
                return;
            }
            snprintf(route_, sizeof(route_), "%s", target);
            snprintf(query_, sizeof(query_), "%s", q ? q : "");
            step_ = HEADERS;
            return;
        }
        // a header, or the blank line after them
        if (line_len_ == 0) {
            headers_done();
            return;
        }
        char* colon = strchr(line_, ':');
        if (colon == nullptr) return;
        *colon = '\0';
        const char* v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (strcasecmp_(line_, "content-length")) {
            content_length_ = (uint32_t) strtoul_(v);
            has_length_ = true;
        } else if (strcasecmp_(line_, "authorization") && !auth_ok_) {
            // "Basic dXNlcjpwYXNz": any user name, the password must be web_password
            if (strncmp(v, "Basic ", 6) == 0) {
                char decoded[100];
                if (base64_decode(v + 6, strlen(v + 6), decoded, sizeof(decoded))) {
                    const char* pw = strchr(decoded, ':');
                    auth_ok_ = pw != nullptr && strcmp(pw + 1, password_) == 0;
                }
            }
        }
    }

    static bool strcasecmp_(const char* a, const char* b) {
        while (*a && *b) {
            char x = *a >= 'A' && *a <= 'Z' ? (char) (*a + 32) : *a;
            char y = *b >= 'A' && *b <= 'Z' ? (char) (*b + 32) : *b;
            if (x != y) return false;
            a++;
            b++;
        }
        return *a == *b;
    }
    static unsigned long strtoul_(const char* v) {
        unsigned long n = 0;
        while (*v >= '0' && *v <= '9') n = n * 10 + (unsigned long) (*v++ - '0');
        return n;
    }

    bool is(const char* m, const char* r) const { return strcmp(method_, m) == 0 && strcmp(route_, r) == 0; }

    // The request is known: what to do
    void headers_done() {
        if (!auth_ok_) {
            static const char WHY[] = "The password is web_password in Picomenu.ini";
            reply(401, "text/plain", WHY, sizeof(WHY) - 1, "WWW-Authenticate: Basic realm=\"PicoMenu\"\r\n");
            return;
        }
        if (is("GET", "/") || is("GET", "/index.html")) {
            head(200, "text/html; charset=utf-8", (long) strlen(PAGE));
            page_at_ = 0;
            job_ = SEND_PAGE;
            return;
        }
        if (is("GET", "/favicon.ico")) {
            reply_text(404, "None");
            return;
        }
        if (strncmp(route_, "/api/", 5) != 0) {
            reply_text(404, "Not found");
            return;
        }
        if (!hooks_->card_ok()) {
            reply_text(503, "No SD card (or the PC has it as a USB drive)");
            return;
        }
        if (!query_value(query_, "p", path_, sizeof(path_)) || !path_ok(path_)) {
            reply_text(400, "Not a path PicoMenu can use");
            return;
        }
        if (is("GET", "/api/list")) start_list();
        else if (is("GET", "/api/get")) start_get();
        else if (is("PUT", "/api/put") || is("POST", "/api/put")) start_put();
        else if (is("POST", "/api/mkdir")) do_mkdir();
        else if (is("POST", "/api/del")) do_del();
        else if (is("POST", "/api/ren")) do_ren();
        else reply_text(405, "Not something it does");
    }

    void start_list() {
        if (f_opendir(&dir_, path_) != FR_OK) {
            reply_text(404, "No such folder");
            return;
        }
        dir_open_ = true;
        head(200, "application/json", -1);
        put("{\"path\":");
        json_string(path_);
        put(",\"items\":[");
        first_item_ = true;
        job_ = SEND_LIST;
    }

    void json_string(const char* s) {
        put("\"");
        for (; *s; s++) {
            unsigned char c = (unsigned char) *s;
            if (c == '"' || c == '\\') {
                char e[2] = { '\\', (char) c };
                put(e, 2);
            } else if (c < 32) {
                char e[8];
                snprintf(e, sizeof(e), "\\u%04x", c);
                put(e);
            } else {
                put(s, 1);                          // (FatFs gives UTF-8)
            }
        }
        put("\"");
    }

    void start_get() {
        FILINFO info;
        if (f_stat(path_, &info) != FR_OK || (info.fattrib & AM_DIR) != 0 || f_open(&file_, path_, FA_READ) != FR_OK) {
            reply_text(404, "No such file");
            return;
        }
        file_open_ = true;
        char extra[PATH_SIZE + 64];
        snprintf(extra, sizeof(extra), "Content-Disposition: attachment; filename=\"%s\"\r\n", base_of(path_));
        for (char* c = extra + 43; *c; c++) if (*c == '"' && c[1] != '\r') *c = '_';
        head(200, "application/octet-stream", (long) f_size(&file_), extra);
        job_ = SEND_FILE;
        if (hooks_->event) hooks_->event('S', path_);
    }

    void start_put() {
        if (!has_length_) {
            reply_text(400, "It needs a Content-Length");
            return;
        }
        char o[4] = "";
        bool over = query_value(query_, "o", o, sizeof(o)) && o[0] == '1';
        FILINFO info;
        if (f_stat(path_, &info) == FR_OK) {
            if ((info.fattrib & AM_DIR) != 0 || !over) {
                job_ = DISCARD;                     // the body is read and dropped, then 409
                code_after_ = 409;
                step_ = content_length_ > 0 ? BODY : REPLY;
                if (content_length_ == 0) body_end();
                return;
            }
        }
        if (f_open(&file_, path_, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
            job_ = DISCARD;
            code_after_ = 400;
            step_ = content_length_ > 0 ? BODY : REPLY;
            if (content_length_ == 0) body_end();
            return;
        }
        file_open_ = true;
        write_ok_ = true;
        job_ = RECEIVE;
        step_ = BODY;
        if (content_length_ == 0) body_end();
    }

    void body(const uint8_t* d, size_t n) {
        if (job_ != RECEIVE || !write_ok_) return;
        UINT w = 0;
        if (f_write(&file_, d, (UINT) n, &w) != FR_OK || w != n) write_ok_ = false;    // (a full card)
    }

    void body_end() {
        if (job_ == DISCARD) {
            reply_text(code_after_, code_after_ == 409 ? "There is one of that name already" : "Could not make the file");
            return;
        }
        bool ok = write_ok_ && f_close(&file_) == FR_OK;
        file_open_ = false;
        if (!ok) {
            f_close(&file_);
            f_unlink(path_);
            reply_text(400, "Could not write it all (is the card full?)");
            return;
        }
        if (hooks_->event) hooks_->event('R', path_);
        reply_text(200, "Saved");
    }

    void do_mkdir() {
        FRESULT r = f_mkdir(path_);
        if (r == FR_OK && hooks_->event) hooks_->event('M', path_);
        reply_text(r == FR_OK ? 200 : r == FR_EXIST ? 409 : 400, r == FR_OK ? "Made" : r == FR_EXIST ? "It is there already" : "Could not make it");
    }

    void do_del() {
        FILINFO info;
        if (strcmp(path_, "/") == 0 || f_stat(path_, &info) != FR_OK) {
            reply_text(404, "No such file or folder");
            return;
        }
        bool ok = hooks_->erase(path_, (info.fattrib & AM_DIR) != 0);
        if (ok && hooks_->event) hooks_->event('E', path_);
        reply_text(ok ? 200 : 400, ok ? "Erased" : "Could not erase it");
    }

    void do_ren() {
        char to_name[100];
        FILINFO info;
        if (!query_value(query_, "to", to_name, sizeof(to_name)) || to_name[0] == '\0' || strchr(to_name, '/') != nullptr) {
            reply_text(400, "Not a name");
            return;
        }
        if (strcmp(path_, "/") == 0 || f_stat(path_, &info) != FR_OK) {
            reply_text(404, "No such file or folder");
            return;
        }
        char to[PATH_SIZE];
        size_t dl = (size_t) (base_of(path_) - path_);
        if (dl + strlen(to_name) + 1 > sizeof(to)) {
            reply_text(400, "Too long a name");
            return;
        }
        memcpy(to, path_, dl);
        snprintf(to + dl, sizeof(to) - dl, "%s", to_name);
        if (!path_ok(to)) {
            reply_text(400, "Not a name");
            return;
        }
        FILINFO other;
        if (f_stat(to, &other) == FR_OK && strcasecmp_(base_of(path_), to_name) == false) {
            reply_text(409, "There is one of that name already");
            return;
        }
        bool ok = hooks_->rename(path_, to, (info.fattrib & AM_DIR) != 0);
        if (ok && hooks_->event) hooks_->event('N', to);
        reply_text(ok ? 200 : 400, ok ? "Renamed" : "Could not rename it");
    }

    // More of the reply, as there is room
    void pump() {
        if (job_ == SEND_PAGE) {
            size_t total = strlen(PAGE);
            while (page_at_ < total && room() > 0) {
                size_t n = total - page_at_;
                if (n > room()) n = room();
                put(PAGE + page_at_, n);
                page_at_ += n;
            }
            if (page_at_ >= total) {
                job_ = NONE;
                done_ = true;
            }
        } else if (job_ == SEND_FILE) {
            while (room() >= 512) {
                if (out_tail_ + 512 > OUT_SIZE) {
                    memmove(out_, out_ + out_head_, out_tail_ - out_head_);
                    out_tail_ -= out_head_;
                    out_head_ = 0;
                }
                UINT got = 0;
                size_t want = OUT_SIZE - out_tail_;
                if (f_read(&file_, out_ + out_tail_, (UINT) want, &got) != FR_OK) got = 0;
                out_tail_ += got;
                if (got < want) {
                    f_close(&file_);
                    file_open_ = false;
                    job_ = NONE;
                    done_ = true;
                    break;
                }
            }
        } else if (job_ == SEND_LIST) {
            while (room() >= 400) {
                FILINFO info;
                if (f_readdir(&dir_, &info) != FR_OK || info.fname[0] == '\0') {
                    f_closedir(&dir_);
                    dir_open_ = false;
                    put("]}");
                    job_ = NONE;
                    done_ = true;
                    break;
                }
                if (info.fattrib & AM_SYS) continue;
                put(first_item_ ? "{\"n\":" : ",{\"n\":");
                first_item_ = false;
                json_string(info.fname);
                char t[64];
                snprintf(t, sizeof(t), ",\"d\":%d,\"h\":%d,\"s\":%lu}", (info.fattrib & AM_DIR) ? 1 : 0,
                    (info.fattrib & AM_HID) ? 1 : 0, (unsigned long) info.fsize);
                put(t);
            }
        }
    }

    const Hooks* hooks_ = nullptr;
    const char* password_ = nullptr;
    Step step_ = REQUEST;
    Job job_ = NONE;
    bool done_ = false, auth_ok_ = false, has_length_ = false, file_open_ = false, dir_open_ = false;
    bool write_ok_ = false, first_item_ = false;
    int code_after_ = 0;
    uint32_t content_length_ = 0, body_got_ = 0;
    size_t line_len_ = 0, page_at_ = 0;
    size_t out_head_ = 0, out_tail_ = 0;
    char method_[8];
    char route_[32];
    char query_[LINE_SIZE];
    char path_[PATH_SIZE];
    char line_[LINE_SIZE];
    FIL file_;
    DIR dir_;
    uint8_t out_[OUT_SIZE];
};

}   // namespace web

#endif
