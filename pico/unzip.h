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

// The game in a .zip (as ZXDB's files are: GreenBeret.tzx.zip holds GreenBeret.tzx): the zip's
// central directory is read for its members, the first that is a game (zxdb::game_ext) is
// taken, and it is written out stored or inflated (miniz's tinfl, MIT licence), its CRC
// checked. File is anything with read, seek, size and write (FileHandle on the Pico; a PC's
// file in the tests). The memory is the caller's: Work (about 11 KB) and a 32 KB window.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "miniz_tinfl.h"
#include "zxdb.h"

namespace unzip {

static const size_t WINDOW = TINFL_LZ_DICT_SIZE;    // 32 KB

struct Member {
    char name[80];
    uint32_t local;                 // its local header
    uint32_t packed, size, crc;
    uint16_t method;                // 0 stored, 8 deflated
};

struct Work {
    tinfl_decompressor inflater;
    uint8_t in[1024];
};

inline uint32_t le16(const uint8_t* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8); }
inline uint32_t le32(const uint8_t* p) { return le16(p) | (le16(p + 2) << 16); }

inline uint32_t crc32(uint32_t crc, const uint8_t* p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

// The first member that is a game. False: not a zip, or no game in it.
template <class File>
bool find(File& f, Member& m, uint8_t* buf, size_t bufsize) {
    int size = f.size();
    if (size < 22 || bufsize < 1024) return false;
    // the end of the central directory: in the file's last bytes (a comment can follow it)
    uint32_t tail = (uint32_t) size < 1024 ? (uint32_t) size : 1024;
    if (!f.seek((uint32_t) size - tail) || f.read(buf, tail) != (int) tail) return false;
    int at = -1;
    for (int i = (int) tail - 22; i >= 0; i--) {
        if (le32(buf + i) == 0x06054B50) { at = i; break; }
    }
    if (at < 0) return false;
    uint32_t count = le16(buf + at + 10), cd = le32(buf + at + 16);
    // its entries, one at a time
    uint32_t pos = cd;
    for (uint32_t e = 0; e < count; e++) {
        uint8_t h[46];
        if (!f.seek(pos) || f.read(h, 46) != 46 || le32(h) != 0x02014B50) return false;
        uint32_t nlen = le16(h + 28), xlen = le16(h + 30), clen = le16(h + 32);
        size_t take = nlen < sizeof(m.name) - 1 ? nlen : sizeof(m.name) - 1;
        if (f.read(m.name, take) != (int) take) return false;
        m.name[take] = '\0';
        m.method = (uint16_t) le16(h + 10);
        m.crc = le32(h + 16);
        m.packed = le32(h + 20);
        m.size = le32(h + 24);
        m.local = le32(h + 42);
        const char* base = strrchr(m.name, '/');
        base = base != nullptr ? base + 1 : m.name;
        if (take == nlen && base[0] != '\0' && zxdb::game_ext(base)[0] != '\0' && strlen(base) == strlen(m.name) - (size_t) (base - m.name) &&
                (m.method == 0 || m.method == 8)) {
            if (base != m.name) memmove(m.name, base, strlen(base) + 1);     // (no folders in its name)
            return true;
        }
        pos += 46 + nlen + xlen + clen;
    }
    return false;
}

// The member written to out. progress (if not null) gets the bytes written so far.
template <class File>
bool extract(File& in, const Member& m, File& out, Work& w, uint8_t* window, void (*progress)(uint32_t, void*) = nullptr, void* ctx = nullptr) {
    uint8_t h[30];
    if (!in.seek(m.local) || in.read(h, 30) != 30 || le32(h) != 0x04034B50) return false;
    uint32_t data = m.local + 30 + le16(h + 26) + le16(h + 28);
    if (!in.seek(data)) return false;
    uint32_t crc = 0, written = 0, left = m.packed;
    if (m.method == 0) {
        while (left > 0) {
            uint32_t n = left < WINDOW ? left : (uint32_t) WINDOW;
            if (in.read(window, n) != (int) n || out.write(window, n) != (int) n) return false;
            crc = crc32(crc, window, n);
            written += n;
            left -= n;
            if (progress) progress(written, ctx);
        }
        return written == m.size && crc == m.crc;
    }
    tinfl_init(&w.inflater);
    size_t in_have = 0, in_at = 0, out_at = 0;
    while (true) {
        if (in_at == in_have && left > 0) {
            uint32_t n = left < sizeof(w.in) ? left : (uint32_t) sizeof(w.in);
            if (in.read(w.in, n) != (int) n) return false;
            in_have = n;
            in_at = 0;
            left -= n;
        }
        size_t in_n = in_have - in_at, out_n = WINDOW - out_at;
        tinfl_status st = tinfl_decompress(&w.inflater, w.in + in_at, &in_n, window, window + out_at, &out_n,
            left > 0 ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        in_at += in_n;
        if (out_n > 0) {
            if (out.write(window + out_at, out_n) != (int) out_n) return false;
            crc = crc32(crc, window + out_at, out_n);
            written += (uint32_t) out_n;
            if (progress) progress(written, ctx);
        }
        out_at = (out_at + out_n) & (WINDOW - 1);
        if (st == TINFL_STATUS_DONE) break;
        if (st < 0) return false;
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT && left == 0 && in_at == in_have) return false;
    }
    return written == m.size && crc == m.crc;
}

}   // namespace unzip
