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


// Updating the Picoface's own firmware from a .uf2 file on the SD card (U on the menu).
//
// The update has three steps, and everything that does not touch the hardware is here so
// it can be tested on a PC:
//   1. Checker: reads the .uf2 block by block and decides whether it is a firmware for this
//      Picoface that is whole and fits in front of the internal file system.
//   2. The Pico copies the image (without the .uf2 wrapping) into a file on the internal
//      flash (littlefs), reads it back, and removes the file again. The blocks that held it
//      are not erased by that, so they are still there: map_file() finds where they are.
//   3. apply(): runs from RAM, with every interrupt off and the other core stopped (the code
//      in the flash is about to be replaced), and copies the image from those blocks over
//      the program, one 4096 byte sector at a time. Then it checks what is in the flash.
// The functions that run in step 3 are marked FW_RAMFUNC; they must not call anything that is
// in the flash (no memcpy, no division, no table of constants in the flash).

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifndef FW_RAMFUNC
#define FW_RAMFUNC
#endif
// no memcpy / memset calls made out of loops
#define FW_NOLIB __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))

namespace fwupdate {

static const uint32_t FLASH_BASE = 0x10000000;
#ifndef PICOMENU_FS_KB
#define PICOMENU_FS_KB 1800
#endif
#ifndef PICOMENU_RESERVE_KB
#define PICOMENU_RESERVE_KB 0
#endif
// the file system starts here: 2 MB - 1800 KB (the Pico W build: - 1400 KB, and the program
// ends 8 KB before it, where the Bluetooth pairing is kept)
static const uint32_t APP_MAX = 2 * 1024 * 1024 - PICOMENU_FS_KB * 1024 - PICOMENU_RESERVE_KB * 1024;
static const uint32_t APP_MIN = 0x10000;            // a Picoface image is far bigger than this
static const uint32_t SECTOR = 4096;
static const uint32_t PAGE = 256;
static const uint32_t FAMILY_RP2040 = 0xE48BFF56;
static const int MAX_BLOCKS = 192;                  // file system blocks the image can use: 191 x 4088 bytes (Map is in snap_load_buffer)
static const size_t VERSION_MAX = 31;
static const char MARKER[] = "tapeloader v";        // the firmware's version text starts like this (up to v65, and in the hidden id since)
static const char MARKER_REV[] = "Rev ";            // from Rev 1.0: "Rev 1.0", digits and dots

enum Error {
    OK = 0,
    BAD_SIZE = 1,           // not a whole number of 512 byte blocks
    BAD_BLOCK = 2,          // not a UF2 block, or not one for the program flash
    NOT_RP2040 = 3,         // for another chip
    BAD_ORDER = 4,          // blocks missing, repeated or not in order
    TOO_BIG = 5,            // would reach the file system
    TOO_SMALL = 6,          // too short to be a Picoface
    NOT_PICOFACE = 7,       // no version text of this firmware inside
    BAD_BOOT2 = 8,          // the first 256 bytes do not start the Pico
    WRONG_BOARD = 9,        // the Pico W build on a Pico, or the other way round
};

inline uint32_t u32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
}

// CRC-32 (the one zip uses). Start with 0xFFFFFFFF, and invert the result at the end.
FW_RAMFUNC FW_NOLIB static uint32_t crc32_update(uint32_t crc, const uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

// The check the Pico's boot ROM makes of the first 256 bytes: a CRC-32 (MSB first, no inversion)
// of the first 252 bytes, kept in the last four
inline bool boot2_ok(const uint8_t* first256) {
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < 252; i++) {
        crc ^= (uint32_t) first256[i] << 24;
        for (int k = 0; k < 8; k++) {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
    }
    return crc == u32(first256 + 252);
}

// Checks a .uf2 file, 512 bytes at a time in order
class Checker {
public:
    void begin(uint32_t file_size) {
        file_size_ = file_size;
        blocks_ = file_size / 512;
        seen_ = 0;
        crc_ = 0xFFFFFFFFu;
        tail_len_ = 0;
        version_[0] = '\0';
        version_ok_ = false;
        pico_w_ = false;
        error_ = (file_size == 0 || file_size % 512 != 0) ? BAD_SIZE : OK;
    }

    // one block; returns OK or the first error found (the same one again after that)
    Error feed(const uint8_t* b) {
        if (error_ != OK) {
            return error_;
        }
        if (u32(b) != 0x0A324655u || u32(b + 4) != 0x9E5D5157u || u32(b + 508) != 0x0AB16F30u) {
            return error_ = BAD_BLOCK;
        }
        uint32_t flags = u32(b + 8), addr = u32(b + 12), size = u32(b + 16), number = u32(b + 20), total = u32(b + 24);
        if ((flags & 0x1) != 0 || size != PAGE) {
            return error_ = BAD_BLOCK;              // marked "not main flash", or not a page
        }
        if ((flags & 0x2000) == 0 || u32(b + 28) != FAMILY_RP2040) {
            return error_ = NOT_RP2040;
        }
        if (number != seen_ || total != blocks_ || addr != FLASH_BASE + seen_ * PAGE) {
            return error_ = BAD_ORDER;
        }
        if ((seen_ + 1) * PAGE > APP_MAX) {
            return error_ = TOO_BIG;
        }
        const uint8_t* data = b + 32;
        if (seen_ == 0) {
            for (int i = 0; i < 256; i++) {
                first_[i] = data[i];
            }
        }
        crc_ = crc32_update(crc_, data, PAGE);
        find_version(data);
        seen_++;
        return OK;
    }

    // after the last block; pico_w: this firmware is the Pico W build (the new one must be too)
    Error finish(bool pico_w = false) {
        if (error_ != OK) {
            return error_;
        }
        if (seen_ != blocks_) {
            return error_ = BAD_ORDER;
        }
        if (size() < APP_MIN) {
            return error_ = TOO_SMALL;
        }
        if (!version_ok_) {
            return error_ = NOT_PICOFACE;
        }
        if (!boot2_ok(first_)) {
            return error_ = BAD_BOOT2;
        }
        if (pico_w_ != pico_w) {
            return error_ = WRONG_BOARD;
        }
        return OK;
    }

    uint32_t size() const { return seen_ * PAGE; }              // the program's bytes
    uint32_t crc() const { return ~crc_; }                       // of those bytes
    const char* version() const { return version_; }
    bool pico_w() const { return pico_w_; }                     // "Rev 2.4.0 W": the Pico W build
    Error error() const { return error_; }

private:
    // the version text, e.g. "tapeloader v48": the first place MARKER is found, up to the end of the text.
    // The last bytes of each block are looked at again with the next, in case the text is split.
    void find_version(const uint8_t* data) {
        if (version_ok_) {
            return;
        }
        uint8_t window[TAIL + 256];
        uint32_t n = 0;
        for (uint32_t i = 0; i < tail_len_; i++) {
            window[n++] = tail_[i];
        }
        for (uint32_t i = 0; i < PAGE; i++) {
            window[n++] = data[i];
        }
        // "Rev 1.0" (a digit, then digits and dots) or the older "tapeloader v65" (digits); the first of either
        for (uint32_t i = 0; i < n; i++) {
            const char* marker = nullptr;
            uint32_t mlen = 0;
            bool rev = false;
            if (window[i] == (uint8_t) MARKER_REV[0]) {
                marker = MARKER_REV; mlen = sizeof(MARKER_REV) - 1; rev = true;
            }
            else if (window[i] == (uint8_t) MARKER[0]) {
                marker = MARKER; mlen = sizeof(MARKER) - 1;
            }
            if (marker == nullptr || i + mlen > n) {
                continue;
            }
            uint32_t k = 0;
            while (k < mlen && window[i + k] == (uint8_t) marker[k]) {
                k++;
            }
            if (k != mlen) {
                continue;
            }
            uint32_t end = i + mlen;
            while (end < n && end - i <= VERSION_MAX && ((window[end] >= '0' && window[end] <= '9') || (rev && window[end] == '.'))) {
                end++;
            }
            if (end + 2 > n || end - i == mlen) {
                continue;                           // cut off by the end of the block (look again with the next), or no number
            }
            if (rev && !(window[i + mlen] >= '0' && window[i + mlen] <= '9' && window[end - 1] >= '0' && window[end - 1] <= '9')) {
                continue;                           // "Rev " must be followed by a number such as 1.0
            }
            for (uint32_t j = i; j < end; j++) {
                version_[j - i] = (char) window[j];
            }
            version_[end - i] = '\0';
            version_ok_ = true;
            pico_w_ = rev && window[end] == ' ' && window[end + 1] == 'W';     // "Rev 2.4.0 W (...
            return;
        }
        tail_len_ = n < TAIL ? n : TAIL;
        for (uint32_t i = 0; i < tail_len_; i++) {
            tail_[i] = window[n - tail_len_ + i];
        }
    }

    static const uint32_t TAIL = 40;
    uint32_t file_size_ = 0, blocks_ = 0, seen_ = 0, crc_ = 0, tail_len_ = 0;
    uint8_t first_[256];
    uint8_t tail_[TAIL];
    char version_[VERSION_MAX + 1];
    bool version_ok_ = false;
    bool pico_w_ = false;
    Error error_ = OK;
};

// ---------------------------------------------------------------------------
// Where a file kept in littlefs is. A file of this size is a list of blocks (a "CTZ skip
// list"): block 0 holds data only, block n (n > 0) starts with ctz(n)+1 pointers of four
// bytes (the first goes to block n-1) and then data. The file's head is its last block.

struct Map {
    int blocks = 0;
    uint32_t size = 0;                              // bytes of the file
    uint32_t start[MAX_BLOCKS];                     // where in the file the data of block i starts
    uint32_t where[MAX_BLOCKS];                     // offset in the file system's flash of that data
};

inline int ctz32(uint32_t n) {
    int c = 0;
    while ((n & 1) == 0) {
        n >>= 1;
        c++;
    }
    return c;
}

// read32(block, offset) gives a word from the file system. Returns false if it does not fit or is not a file this way.
template <class Read32>
bool map_file(Read32 read32, uint32_t head, uint32_t size, uint32_t block_size, Map& map) {
    // how many blocks: add up the data each holds until the size is reached
    uint32_t total = 0;
    int n = 0;
    while (total < size) {
        if (n >= MAX_BLOCKS) {
            return false;
        }
        uint32_t cap = n == 0 ? block_size : block_size - 4 * (ctz32(n) + 1);
        map.start[n] = total;
        total += cap;
        n++;
    }
    if (n == 0) {
        return false;
    }
    map.blocks = n;
    map.size = size;
    uint32_t block = head;
    for (int i = n - 1; i >= 0; i--) {
        map.where[i] = block * block_size + (i == 0 ? 0 : 4 * (ctz32(i) + 1));
        if (i > 0) {
            block = read32(block, 0);
            if (block == 0xFFFFFFFFu) {
                return false;
            }
        }
    }
    return true;
}

// Copies len bytes starting at offset in the file to dst. fs is the file system's flash
FW_RAMFUNC FW_NOLIB static void gather(const uint8_t* fs, const Map& map, uint32_t offset, uint8_t* dst, uint32_t len) {
    int i = 0;
    while (i + 1 < map.blocks && map.start[i + 1] <= offset) {
        i++;
    }
    while (len > 0 && i < map.blocks) {
        uint32_t end = i + 1 < map.blocks ? map.start[i + 1] : map.size;
        uint32_t within = offset - map.start[i];
        uint32_t take = end - offset;
        if (take > len) {
            take = len;
        }
        const uint8_t* src = fs + map.where[i] + within;
        for (uint32_t k = 0; k < take; k++) {
            dst[k] = src[k];
        }
        dst += take;
        offset += take;
        len -= take;
        i++;
    }
}

// CRC-32 of the file (as the flash holds it)
FW_RAMFUNC FW_NOLIB static uint32_t file_crc(const uint8_t* fs, const Map& map, uint8_t* scratch) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t off = 0; off < map.size; off += SECTOR) {
        uint32_t n = map.size - off < SECTOR ? map.size - off : SECTOR;
        gather(fs, map, off, scratch, n);
        crc = crc32_update(crc, scratch, n);
    }
    return ~crc;
}

// What apply() needs from the hardware (the SDK's flash_range_erase and flash_range_program
// on the Pico, which run from RAM)
struct Flash {
    void (*erase)(uint32_t offset, uint32_t count);
    void (*program)(uint32_t offset, const uint8_t* data, uint32_t count);
};

// Copies the file over the program in the flash and checks it. Interrupts must be off and the
// other core stopped. sector is 4096 bytes of RAM. app is the program as the flash shows it
// (XIP). Returns true if the flash holds the image (its CRC is expected_crc).
// progress, if not null, is set to how many of BAR_CELLS the first pass has done (the Spectrum
// reads it to draw a bar, so it is written without any division).
static const uint32_t BAR_CELLS = 28;
FW_RAMFUNC FW_NOLIB static bool apply(const Flash& flash, const uint8_t* fs, const Map& map, const uint8_t* app,
        uint32_t expected_crc, uint8_t* sector, int attempts, volatile uint8_t* progress = nullptr) {
    for (int attempt = 0; attempt < attempts; attempt++) {
        uint32_t sectors = (map.size + SECTOR - 1) / SECTOR;    // (a constant divisor, SECTOR is 4096: a shift)
        uint32_t acc = 0, cells = 0;
        for (uint32_t off = 0; off < map.size; off += SECTOR) {
            uint32_t n = map.size - off < SECTOR ? map.size - off : SECTOR;
            gather(fs, map, off, sector, n);
            for (uint32_t k = n; k < SECTOR; k++) {
                sector[k] = 0xFF;
            }
            flash.erase(off, SECTOR);
            flash.program(off, sector, SECTOR);
            if (progress != nullptr && attempt == 0) {
                acc += BAR_CELLS;
                while (acc >= sectors) {
                    acc -= sectors;
                    cells++;
                }
                *progress = (uint8_t) (cells < BAR_CELLS ? cells : BAR_CELLS);
            }
        }
        uint32_t crc = crc32_update(0xFFFFFFFFu, app, map.size);
        if (~crc == expected_crc) {
            return true;
        }
    }
    return false;
}

}   // namespace fwupdate
