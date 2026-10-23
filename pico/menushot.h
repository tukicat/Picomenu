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


// Menu grabs: a double press of the button while a menu is up saves the menu's screen as a
// .png in /Scraper/Screen Grabs, named after the menu. The name is read off the screen itself
// (its title, in the menus' font), and the picture is written as a PNG: 256 x 192, 16 colours
// (the 8 Spectrum colours, normal and bright; flash is shown as it is), 4 bits a pixel, the
// image data stored without compression (about 25 KB, and no memory for a compressor).
// Nothing here touches the hardware, so it is tested on a PC.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

namespace menushot {

static const size_t SCR_SIZE = 6912;
static const size_t NAME_MAX = 40;

// A row of the screen as text: each cell that is a letter of the font (ink on paper, or the
// other way round) is that letter, any other is '\x01'. out has room for 33.
inline void row_text(const uint8_t* scr, int row, const uint8_t (*font)[8], char* out) {
    for (int col = 0; col < 32; col++) {
        uint8_t g[8];
        for (int l = 0; l < 8; l++) g[l] = scr[((row >> 3) << 11) | (l << 8) | ((row & 7) << 5) | col];
        char ch = '\x01';
        for (int c = 32; c < 128; c++) {
            bool same = true, inv = true;
            for (int l = 0; l < 8 && (same || inv); l++) {
                same = same && font[c - 32][l] == g[l];
                inv = inv && (uint8_t) ~font[c - 32][l] == g[l];
            }
            if (same || inv) { ch = (char) c; break; }
        }
        out[col] = ch;
    }
    out[32] = '\0';
}

// Trims spaces at both ends, in place; false if the text has a cell that is not a letter
inline bool tidy(char* t) {
    bool readable = strchr(t, '\x01') == nullptr;
    size_t n = strlen(t);
    while (n > 0 && t[n - 1] == ' ') t[--n] = '\0';
    size_t a = 0;
    while (t[a] == ' ') a++;
    memmove(t, t + a, n - a + 1);
    return readable;
}

// The menu's name, from the screen: the title row (row 0); with PICOMENU there, the menu's
// own title under it (Browse Games, Tools, Change Rom ...); a Help page's title gets "Help - "
// and loses its page number; the NMI menu (its big title is not in the font) is "NMI Menu";
// anything else (a picture on the whole screen) "Screen". out has room for NAME_MAX + 1.
inline void name_of(const uint8_t* scr, const uint8_t (*font)[8], char* out) {
    char r0[33], r1[33], r3[33];
    row_text(scr, 0, font, r0);
    row_text(scr, 1, font, r1);
    row_text(scr, 3, font, r3);
    bool ok0 = tidy(r0), ok1 = tidy(r1);
    tidy(r3);
    char name[64];
    if (ok0 && r0[0] != '\0' && strcmp(r0, "PICOMENU") != 0) {
        // "Picomenu.ini (2)          12/45": a Help page
        size_t n = strlen(r0);
        size_t i = n;
        while (i > 0 && (isdigit((unsigned char) r0[i - 1]) || r0[i - 1] == '/')) i--;
        bool page = i < n && i > 0 && r0[i - 1] == ' ' && strchr(r0 + i, '/') != nullptr;
        if (page) {
            r0[i] = '\0';
            tidy(r0);
            snprintf(name, sizeof(name), "Help - %s", r0);
        } else {
            snprintf(name, sizeof(name), "%s", r0);
        }
    } else if (ok0 && strcmp(r0, "PICOMENU") == 0 && ok1 && r1[0] != '\0') {
        snprintf(name, sizeof(name), "%s", r1);
    } else if (strncmp(r3, "B-Browse", 8) == 0) {        // its first item
        snprintf(name, sizeof(name), "NMI Menu");
    } else {
        snprintf(name, sizeof(name), "Screen");
    }
    // a file name: ": " becomes " - ", / becomes -, and \ : * ? " < > | go; no double spaces,
    // and no dot or space at the end
    size_t o = 0;
    for (size_t i = 0; name[i] != '\0' && o < NAME_MAX; i++) {
        char c = name[i];
        if (c == ':') {
            if (o < NAME_MAX) out[o++] = ' ';
            if (o < NAME_MAX) out[o++] = '-';
            continue;
        }
        if (c == '/') c = '-';
        if (strchr("\\*?\"<>|", c) != nullptr) continue;
        if (c == ' ' && o > 0 && out[o - 1] == ' ') continue;
        out[o++] = c;
    }
    while (o > 0 && (out[o - 1] == '.' || out[o - 1] == ' ')) o--;
    out[o] = '\0';
    if (o == 0) snprintf(out, NAME_MAX + 1, "Screen");
}

// ---- PNG ----

inline uint32_t crc32_add(uint32_t crc, const uint8_t* p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static const size_t ROW_BYTES = 1 + 128;            // the filter byte, then 256 pixels of 4 bits
static const size_t RAW_SIZE = 192 * ROW_BYTES;     // 24768

// The colours: 0-7 normal, 8-15 bright
inline void palette(uint8_t* rgb) {
    for (int i = 0; i < 16; i++) {
        uint8_t v = i >= 8 ? 0xFF : 0xD8;
        int c = i & 7;
        rgb[i * 3 + 0] = (c & 2) ? v : 0;       // red
        rgb[i * 3 + 1] = (c & 4) ? v : 0;       // green
        rgb[i * 3 + 2] = (c & 1) ? v : 0;       // blue
    }
}

// Writes the screen as a PNG to sink (sink.write(data, n) returns false if it failed)
template <class Sink>
inline bool write_png(Sink& sink, const uint8_t* scr) {
    auto be32 = [](uint8_t* p, uint32_t v) { p[0] = (uint8_t) (v >> 24); p[1] = (uint8_t) (v >> 16); p[2] = (uint8_t) (v >> 8); p[3] = (uint8_t) v; };
    // a whole chunk from a buffer
    auto chunk = [&](const char* type, const uint8_t* data, uint32_t n) {
        uint8_t h[8];
        be32(h, n);
        memcpy(h + 4, type, 4);
        uint32_t crc = crc32_add(0, h + 4, 4);
        crc = crc32_add(crc, data, n);
        uint8_t c[4];
        be32(c, crc);
        return sink.write(h, 8) && (n == 0 || sink.write(data, n)) && sink.write(c, 4);
    };
    static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (!sink.write(SIG, 8)) return false;
    uint8_t ihdr[13];
    be32(ihdr, 256);
    be32(ihdr + 4, 192);
    ihdr[8] = 4;            // bits a pixel
    ihdr[9] = 3;            // with a palette
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    if (!chunk("IHDR", ihdr, 13)) return false;
    uint8_t plte[48];
    palette(plte);
    if (!chunk("PLTE", plte, 48)) return false;
    // IDAT: zlib (no compression): 78 01, one stored block of RAW_SIZE bytes, then Adler-32
    const uint32_t idat_len = 2 + 5 + RAW_SIZE + 4;
    uint8_t h[8 + 7];
    be32(h, idat_len);
    memcpy(h + 4, "IDAT", 4);
    h[8] = 0x78;
    h[9] = 0x01;
    h[10] = 0x01;           // the last block, stored
    h[11] = (uint8_t) (RAW_SIZE & 0xFF);
    h[12] = (uint8_t) (RAW_SIZE >> 8);
    h[13] = (uint8_t) ~h[11];
    h[14] = (uint8_t) ~h[12];
    uint32_t crc = crc32_add(0, h + 4, 11);
    if (!sink.write(h, sizeof(h))) return false;
    uint32_t a = 1, b = 0;          // Adler-32
    uint8_t row[ROW_BYTES];
    for (int y = 0; y < 192; y++) {
        row[0] = 0;                 // no filter
        const uint8_t* pix = scr + (((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2));
        const uint8_t* attr = scr + 6144 + (y >> 3) * 32;
        for (int cx = 0; cx < 32; cx++) {
            uint8_t at = attr[cx];
            uint8_t ink = (uint8_t) ((at & 7) | ((at & 0x40) ? 8 : 0));
            uint8_t paper = (uint8_t) (((at >> 3) & 7) | ((at & 0x40) ? 8 : 0));
            uint8_t bits = pix[cx];
            for (int k = 0; k < 4; k++) {
                uint8_t hi = (bits & 0x80) ? ink : paper;
                uint8_t lo = (bits & 0x40) ? ink : paper;
                row[1 + cx * 4 + k] = (uint8_t) ((hi << 4) | lo);
                bits = (uint8_t) (bits << 2);
            }
        }
        for (size_t i = 0; i < ROW_BYTES; i++) {
            a = (a + row[i]) % 65521;
            b = (b + a) % 65521;
        }
        crc = crc32_add(crc, row, ROW_BYTES);
        if (!sink.write(row, ROW_BYTES)) return false;
    }
    uint8_t tail[8];
    be32(tail, (b << 16) | a);
    crc = crc32_add(crc, tail, 4);
    be32(tail + 4, crc);
    if (!sink.write(tail, 8)) return false;
    return chunk("IEND", nullptr, 0);
}

// Reads a PNG that write_png made (256 x 192, 16 colours, 4 bits a pixel, the image data
// stored: not compressed, no filter) back into a screen (6912 bytes). Each 8 x 8 cell gets its
// commonest colour as the paper and the next as the ink. scratch has room for RAW_SIZE + 512.
// False for any other PNG (V then leaves it out).
template <class File>
inline bool read_png(File& f, uint8_t* scr, uint8_t* scratch) {
    auto be32 = [](const uint8_t* p) { return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3]; };
    uint8_t h[16];
    if (f.read(h, 8) != 8 || h[0] != 0x89 || h[1] != 'P' || h[2] != 'N' || h[3] != 'G') return false;
    const uint32_t cap = (uint32_t) RAW_SIZE + 512;
    uint32_t have = 0;                      // the zlib stream (all the IDAT chunks) in scratch
    uint8_t pal[16];
    int colours = -1;
    bool header = false;
    for (;;) {
        if (f.read(h, 8) != 8) return false;
        uint32_t len = be32(h);
        if (memcmp(h + 4, "IHDR", 4) == 0) {
            uint8_t d[13];
            if (len != 13 || f.read(d, 13) != 13) return false;
            if (be32(d) != 256 || be32(d + 4) != 192 || d[8] != 4 || d[9] != 3 || d[12] != 0) return false;
            header = true;
        } else if (memcmp(h + 4, "PLTE", 4) == 0) {
            if (len % 3 != 0 || len > 48) return false;
            colours = (int) (len / 3);
            for (int i = 0; i < colours; i++) {
                uint8_t rgb[3];
                if (f.read(rgb, 3) != 3) return false;
                // the Spectrum colour: G R B bits (anything over half is on), bright if any is full
                uint8_t c = (uint8_t) (((rgb[1] > 0x80) ? 4 : 0) | ((rgb[0] > 0x80) ? 2 : 0) | ((rgb[2] > 0x80) ? 1 : 0));
                bool bright = rgb[0] > 0xE8 || rgb[1] > 0xE8 || rgb[2] > 0xE8;
                pal[i] = (uint8_t) (c | (bright ? 8 : 0));
            }
        } else if (memcmp(h + 4, "IDAT", 4) == 0) {
            if (have + len > cap || f.read(scratch + have, len) != (int) len) return false;
            have += len;
        } else if (memcmp(h + 4, "IEND", 4) == 0) {
            break;
        } else {
            for (uint32_t i = 0; i < len; i++) {           // another chunk: passed over
                if (f.read(h, 1) != 1) return false;
            }
        }
        if (f.read(h, 4) != 4) return false;               // its CRC
    }
    if (!header || colours <= 0 || have < 2) return false;
    // zlib: 2 bytes, then stored blocks only (BTYPE 00), copied down over themselves
    uint32_t in = 2, out = 0;
    bool last = false;
    while (!last) {
        if (in + 5 > have) return false;
        uint8_t b = scratch[in];
        if ((b & 6) != 0) return false;                     // compressed: not one of ours
        last = (b & 1) != 0;
        uint32_t n = scratch[in + 1] | (scratch[in + 2] << 8);
        in += 5;
        if (in + n > have || out + n > RAW_SIZE) return false;
        memmove(scratch + out, scratch + in, n);
        in += n;
        out += n;
    }
    if (out != RAW_SIZE) return false;
    for (int y = 0; y < 192; y++) {
        if (scratch[y * ROW_BYTES] != 0) return false;      // a filter: not one of ours
    }
    auto colour_at = [&](int x, int y) -> uint8_t {
        uint8_t v = scratch[y * ROW_BYTES + 1 + (x >> 1)];
        uint8_t i = (x & 1) ? (v & 15) : (v >> 4);
        return i < colours ? pal[i] : 0;
    };
    memset(scr, 0, SCR_SIZE);
    for (int cy = 0; cy < 24; cy++) {
        for (int cx = 0; cx < 32; cx++) {
            uint8_t count[16] = {};
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) count[colour_at(cx * 8 + x, cy * 8 + y)]++;
            int paper = 0, ink = -1;
            for (int c = 1; c < 16; c++) if (count[c] > count[paper]) paper = c;
            for (int c = 0; c < 16; c++) if (c != paper && count[c] > 0 && (ink < 0 || count[c] > count[ink])) ink = c;
            if (ink < 0) ink = paper;
            // black has no bright of its own: the other colour says
            bool bright = (paper & 7) != 0 ? (paper & 8) != 0 : (ink & 8) != 0;
            scr[6144 + cy * 32 + cx] = (uint8_t) ((bright ? 0x40 : 0) | ((paper & 7) << 3) | (ink & 7));
            for (int y = 0; y < 8; y++) {
                uint8_t bits = 0;
                for (int x = 0; x < 8; x++) bits = (uint8_t) ((bits << 1) | (colour_at(cx * 8 + x, cy * 8 + y) != paper ? 1 : 0));
                int py = cy * 8 + y;
                scr[((py & 0xC0) << 5) | ((py & 7) << 8) | ((py & 0x38) << 2) | cx] = bits;
            }
        }
    }
    return true;
}

}   // namespace menushot
