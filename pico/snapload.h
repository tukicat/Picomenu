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

// Reading .sna and .z80 snapshots for the NMI rom to restore.
//
// Three kinds of snapshot are handed to the NMI rom:
//
//   KIND_SNA48   a 48K .sna file, exactly as it is in the file (27 byte header,
//                49152 bytes of RAM, PC on the stack).
//   KIND_Z80V1   a 48K snapshot in the layout of an uncompressed version 1 .z80
//                file: 30 byte header then 49152 bytes of RAM.  Version 2 and 3
//                .z80 files for a 48K machine, and compressed version 1 files,
//                are decoded into this layout, so the NMI rom has only one 48K
//                .z80 case to deal with.
//   KIND_PAGED   a 128K snapshot, from a 128K .sna file or a .z80 file for a 128K
//                machine.  The buffer holds
//                    [0 .. 29]   a version 1 .z80 style header (PC filled in)
//                    [30]        the value last written to port 0x7FFD
//                    [31]        non zero if the sound chip registers are valid
//                    [32]        the sound chip register that was selected
//                    [33 .. 48]  the sound chip registers
//                    [64 ..]     the eight 16K RAM banks, in the order of
//                                PAGE_ORDER, which is the order the NMI rom asks
//                                for them in (page_order in nmirom.asm)
//
// Everything is written to be tested on a PC as well as built for the Pico: File
// is anything with "int read(void* buffer, size_t size)".

#ifndef INCLUDED_SNAPLOAD_H
#define INCLUDED_SNAPLOAD_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace snapload {

static const uint32_t PAGE_SIZE = 16384;
static const uint32_t IMAGE128_SIZE = 8 * PAGE_SIZE;
static const uint32_t HEADER_AREA = 64;                     // room for the header before the banks
static const uint32_t BUFFER128_SIZE = HEADER_AREA + IMAGE128_SIZE;
static const uint32_t NMI_HEADER128_SIZE = 49;              // how much of the header the NMI rom is sent

static const uint32_t SNA48_SIZE = 49179;
static const uint32_t SNA128_MIN_SIZE = 27 + 3 * PAGE_SIZE + 4 + 5 * PAGE_SIZE;    // 131103
static const uint32_t Z80_RAM48_SIZE = 3 * PAGE_SIZE;
static const uint32_t Z80V1_SIZE = 30 + Z80_RAM48_SIZE;        // an uncompressed version 1 file

// Where the extra fields of the header are (the first 30 bytes are the z80 header)
static const uint32_t X_7FFD = 30;
static const uint32_t X_AYFLAG = 31;
static const uint32_t X_AYSEL = 32;
static const uint32_t X_AYREGS = 33;

enum Kind {
    KIND_SNA48 = 0,         // these three are the type byte that the NMI rom is sent
    KIND_Z80V1 = 1,
    KIND_PAGED = 2,
    KIND_ERROR = -1
};

// The order that the banks are given to the NMI rom, bank 5 is address 0x4000, bank 2 is 0x8000
static constexpr uint8_t PAGE_ORDER[8] = { 5, 2, 0, 1, 3, 4, 6, 7 };

// Position of a bank in PAGE_ORDER
static inline uint32_t slot_of_bank(uint32_t bank) {
    static const uint8_t slot[8] = { 2, 3, 1, 4, 5, 0, 6, 7 };
    return slot[bank & 7];
}

static inline uint8_t* bank_memory(uint8_t* buf, uint32_t bank) {
    return buf + HEADER_AREA + slot_of_bank(bank) * PAGE_SIZE;
}

static inline bool is_sna128(uint32_t fileSize) {
    return fileSize >= SNA128_MIN_SIZE;
}


// Reads a file a byte at a time, from a small buffer, so decoding does not
// need the whole file in memory
template <class File>
class Reader {
public:
    Reader(File& file, uint8_t* iobuf, uint32_t iosize) :
        f(file), buf(iobuf), cap(iosize), pos(0), len(0), ended(false) {}

    int get() {
        if (pos >= len && !fill()) {
            return -1;
        }
        return buf[pos++];
    }

    bool read(uint8_t* dst, uint32_t n) {
        while (n > 0) {
            if (pos >= len && n >= cap && !ended) {
                // Nothing buffered and a lot wanted: read straight into the destination
                int got = f.read(dst, n);
                if (got <= 0) {
                    ended = true;
                    return false;
                }
                dst += got;
                n -= (uint32_t) got;
                continue;
            }
            if (pos >= len && !fill()) {
                return false;
            }
            uint32_t chunk = len - pos;
            if (chunk > n) chunk = n;
            memcpy(dst, buf + pos, chunk);
            pos += chunk;
            dst += chunk;
            n -= chunk;
        }
        return true;
    }

    bool skip(uint32_t n) {
        while (n > 0) {
            if (pos >= len && !fill()) {
                return false;
            }
            uint32_t chunk = len - pos;
            if (chunk > n) chunk = n;
            pos += chunk;
            n -= chunk;
        }
        return true;
    }

private:
    bool fill() {
        if (ended) {
            return false;
        }
        int n = f.read(buf, cap);
        if (n <= 0) {
            ended = true;
            return false;
        }
        len = (uint32_t) n;
        pos = 0;
        return true;
    }

    File& f;
    uint8_t* buf;
    uint32_t cap;
    uint32_t pos;
    uint32_t len;
    bool ended;
};


// Decodes a block of a version 2 or 3 .z80 file into dst, which is a page.
// clen is the length of the block in the file, 0xFFFF means it is not compressed.
// Compression: ED ED n b is n copies of b, and a single ED is followed by an
// ordinary byte.  Exactly clen bytes are always consumed so the next block header
// is where it should be.
template <class R>
static bool z80_decode_block(R& r, uint32_t clen, uint8_t* dst, uint32_t dstlen) {
    if (clen == 0xFFFF) {
        return r.read(dst, dstlen);
    }
    uint32_t used = 0;
    uint32_t out = 0;
    while (used < clen) {
        int b = r.get();
        if (b < 0) {
            return false;
        }
        used++;
        if (b == 0xED && used < clen) {
            int b2 = r.get();
            if (b2 < 0) {
                return false;
            }
            used++;
            if (b2 == 0xED) {
                int count = r.get();
                int value = r.get();
                if (value < 0) {
                    return false;
                }
                used += 2;
                while (count-- > 0 && out < dstlen) {
                    dst[out++] = (uint8_t) value;
                }
            }
            else {
                if (out < dstlen) dst[out++] = 0xED;
                if (out < dstlen) dst[out++] = (uint8_t) b2;
            }
        }
        else if (out < dstlen) {
            dst[out++] = (uint8_t) b;
        }
    }
    return true;
}


// Decodes the RAM of a compressed version 1 .z80 file.  It runs to the end of the
// file and is finished by the marker 00 ED ED 00.
template <class R>
static void z80_decode_v1(R& r, uint8_t* dst, uint32_t dstlen) {
    uint32_t out = 0;
    for (;;) {
        int b = r.get();
        if (b < 0) {
            return;
        }
        if (b == 0xED) {
            int b2 = r.get();
            if (b2 < 0) {
                if (out < dstlen) dst[out++] = 0xED;
                return;
            }
            if (b2 == 0xED) {
                int count = r.get();
                int value = r.get();
                if (count <= 0 || value < 0) {
                    return;                         // the end marker
                }
                while (count-- > 0 && out < dstlen) {
                    dst[out++] = (uint8_t) value;
                }
            }
            else {
                if (out < dstlen) dst[out++] = 0xED;
                if (out < dstlen) dst[out++] = (uint8_t) b2;
            }
        }
        else if (out < dstlen) {
            dst[out++] = (uint8_t) b;
        }
    }
}


// Loads a .z80 file into buf.  KIND_Z80V1 needs buf to hold 30 + 49152 bytes,
// KIND_PAGED needs BUFFER128_SIZE.  fileSize is the size of the file, which is
// how an uncompressed version 1 file whose PC is 0 is told from a later version.
template <class File>
Kind load_z80(File& f, int32_t fileSize, uint8_t* buf, uint8_t* iobuf, uint32_t iosize) {
    Reader<File> r(f, iobuf, iosize);
    uint8_t h[30];
    if (!r.read(h, sizeof(h))) {
        return KIND_ERROR;
    }
    if (h[12] == 255) {
        h[12] = 1;                              // old files use 255 for 1
    }

    if ((h[6] | (h[7] << 8)) != 0 || fileSize == (int32_t) Z80V1_SIZE) {
        // Version 1, always for a 48K machine
        uint8_t* ram = buf + 30;
        bool compressed = (h[12] & 0x20) != 0;
        memset(ram, 0, Z80_RAM48_SIZE);
        if (compressed) {
            z80_decode_v1(r, ram, Z80_RAM48_SIZE);
        }
        else {
            r.read(ram, Z80_RAM48_SIZE);
        }
        memcpy(buf, h, sizeof(h));
        buf[12] &= ~0x20;
        return KIND_Z80V1;
    }

    // Version 2 or 3: an additional header and then the memory in 16K pages
    uint8_t lenbytes[2];
    if (!r.read(lenbytes, 2)) {
        return KIND_ERROR;
    }
    uint32_t extlen = lenbytes[0] | (lenbytes[1] << 8);
    if (extlen < 23 || extlen > 64) {
        return KIND_ERROR;
    }
    uint8_t x[64];                              // x[n] is byte 32 + n of the file
    if (!r.read(x, extlen)) {
        return KIND_ERROR;
    }
    bool version2 = extlen == 23;
    uint8_t hardware = x[2];
    bool is48K = version2 ? hardware <= 2 : (hardware <= 3 || hardware == 14);

    if (is48K) {
        uint8_t* ram = buf + 30;
        memset(ram, 0, Z80_RAM48_SIZE);
        memcpy(buf, h, sizeof(h));
        buf[6] = x[0];
        buf[7] = x[1];
        buf[12] &= ~0x20;
        uint8_t bh[3];
        while (r.read(bh, sizeof(bh))) {
            uint32_t clen = bh[0] | (bh[1] << 8);
            int offset = bh[2] == 8 ? 0 : bh[2] == 4 ? PAGE_SIZE : bh[2] == 5 ? 2 * PAGE_SIZE : -1;
            if (offset >= 0) {
                if (!z80_decode_block(r, clen, ram + offset, PAGE_SIZE)) {
                    break;
                }
            }
            else if (!r.skip(clen == 0xFFFF ? PAGE_SIZE : clen)) {
                break;
            }
        }
        return KIND_Z80V1;
    }

    memset(buf, 0, BUFFER128_SIZE);
    memcpy(buf, h, sizeof(h));
    buf[6] = x[0];
    buf[7] = x[1];
    buf[12] &= ~0x20;
    buf[X_7FFD] = x[3];
    buf[X_AYFLAG] = 1;
    buf[X_AYSEL] = x[6];
    memcpy(buf + X_AYREGS, x + 7, 16);
    uint8_t bh[3];
    while (r.read(bh, sizeof(bh))) {
        uint32_t clen = bh[0] | (bh[1] << 8);
        if (bh[2] >= 3 && bh[2] <= 10) {        // page 3 is bank 0 ... page 10 is bank 7
            if (!z80_decode_block(r, clen, bank_memory(buf, bh[2] - 3u), PAGE_SIZE)) {
                break;
            }
        }
        else if (!r.skip(clen == 0xFFFF ? PAGE_SIZE : clen)) {
            break;
        }
    }
    return KIND_PAGED;
}


// Fills in the 30 byte version 1 .z80 style header at buf from the 27 byte
// header of a .sna file and the PC
static inline void sna_to_z80_header(const uint8_t* s, uint8_t pcLow, uint8_t pcHigh, uint8_t* buf) {
    buf[0] = s[22];                             // A
    buf[1] = s[21];                             // F
    buf[2] = s[13]; buf[3] = s[14];             // BC
    buf[4] = s[9];  buf[5] = s[10];             // HL
    buf[6] = pcLow; buf[7] = pcHigh;       // PC
    buf[8] = s[23]; buf[9] = s[24];             // SP
    buf[10] = s[0];                             // I
    buf[11] = s[20];                            // R
    buf[12] = (uint8_t) (((s[26] & 7) << 1) | ((s[20] >> 7) & 1));      // border
    buf[13] = s[11]; buf[14] = s[12];           // DE
    buf[15] = s[5];  buf[16] = s[6];            // BC'
    buf[17] = s[3];  buf[18] = s[4];            // DE'
    buf[19] = s[1];  buf[20] = s[2];            // HL'
    buf[21] = s[8];                             // A'
    buf[22] = s[7];                             // F'
    buf[23] = s[15]; buf[24] = s[16];           // IY
    buf[25] = s[17]; buf[26] = s[18];           // IX
    buf[27] = (s[19] >> 2) & 1;                 // interrupts enabled
    buf[28] = (s[19] >> 2) & 1;
    buf[29] = s[25] & 3;                        // interrupt mode
}


// Loads a 128K .sna file (see is_sna128) into buf, which must be BUFFER128_SIZE.
// The file is the 27 byte header, banks 5 and 2 and the bank that is paged in at
// 0xC000, then PC, the value of port 0x7FFD, a TR-DOS flag, and finally the other
// banks in order.  A paged bank of 5 or 2 is in the file twice.
template <class File>
Kind load_sna128(File& f, uint8_t* buf, uint8_t* iobuf, uint32_t iosize) {
    Reader<File> r(f, iobuf, iosize);
    uint8_t s[27];
    if (!r.read(s, sizeof(s))) {
        return KIND_ERROR;
    }
    memset(buf, 0, BUFFER128_SIZE);

    r.read(bank_memory(buf, 5), PAGE_SIZE);
    r.read(bank_memory(buf, 2), PAGE_SIZE);
    // The paged bank is not known until the bytes after it have been read, so it
    // goes into the place of bank 7 for now.
    uint8_t* paged = bank_memory(buf, 7);
    r.read(paged, PAGE_SIZE);
    uint8_t extra[4] = { 0, 0, 0, 0 };
    r.read(extra, sizeof(extra));
    uint32_t pagedBank = extra[2] & 7;
    if (pagedBank != 7) {
        memcpy(bank_memory(buf, pagedBank), paged, PAGE_SIZE);
    }
    for (uint32_t bank = 0; bank < 8; bank++) {
        if (bank != 5 && bank != 2 && bank != pagedBank) {
            r.read(bank_memory(buf, bank), PAGE_SIZE);
        }
    }

    sna_to_z80_header(s, extra[0], extra[1], buf);
    buf[X_7FFD] = extra[2];
    buf[X_AYFLAG] = 0;                          // a .sna file has no sound chip registers
    return KIND_PAGED;
}


// ---------------------------------------------------------------------------
// Saving a 128K snapshot as a version 3 .z80 file
//
// The NMI rom sends SAVE128_HEADER bytes (the 27 byte .sna header and then 23
// more) and the eight banks in PAGE_ORDER.  They are put in a buffer of the
// layout KIND_PAGED describes, which write_z80_128 turns into the file.

static const uint32_t SAVE128_HEADER = 50;
// the part of the SAVE128_HEADER after the .sna header
static const uint32_t SV_PC = 27;           // 2 bytes
static const uint32_t SV_BANKM = 29;        // the last value the ROM wrote to port 0x7FFD
static const uint32_t SV_S1 = 30;           // hash of the paged in bank, 2 bytes
static const uint32_t SV_S2 = 32;
static const uint32_t SV_AY = 34;           // 16 sound chip registers
// where the 128K ROM keeps its copy of the last value it wrote to port 0x7FFD
static const uint32_t BANK_M_ADDRESS = 0x5B5C;

// The NMI rom hashes the end of the bank that is paged in at 0xC000: from 0xDB00,
// which is after where bank 5 holds the screen that the menu overwrites
static const uint32_t HASH_START = 0xDB00 - 0xC000;
static const uint32_t HASH_LEN = PAGE_SIZE - HASH_START;

static inline void hash_page_end(const uint8_t* page, uint16_t& s1, uint16_t& s2) {
    uint16_t a = 0, b = 0;
    for (uint32_t i = HASH_START; i < PAGE_SIZE; i++) {
        a = (uint16_t) (a + page[i]);
        b = (uint16_t) (b + a);
    }
    s1 = a;
    s2 = b;
}

// What is known about the paging port from the last 128K snapshot that was loaded:
// its value, and the byte that the snapshot had at BANK_M. See choose_7ffd.
struct LoadedPaging {
    bool valid = false;
    uint8_t port = 0;
    uint8_t bankm = 0;
};

// The bank (0 to 7) whose end has the hash in info, trying hint first, or -1
static inline int find_hashed_bank(const uint8_t* buf, const uint8_t* info, uint32_t hint) {
    uint16_t want1 = (uint16_t) (info[SV_S1] | (info[SV_S1 + 1] << 8));
    uint16_t want2 = (uint16_t) (info[SV_S2] | (info[SV_S2 + 1] << 8));
    for (uint32_t i = 0; i < 9; i++) {
        uint32_t bank = i == 0 ? (hint & 7) : i - 1;
        uint16_t s1, s2;
        hash_page_end(bank_memory(const_cast<uint8_t*>(buf), bank), s1, s2);
        if (s1 == want1 && s2 == want2) {
            return (int) bank;
        }
    }
    return -1;
}

// The value for port 0x7FFD at the time of the snapshot.  The NMI rom can't read the
// port, so it sends BANK_M (what the 128K ROM last wrote) and a hash of what was
// paged in, which tells which bank it was.  The ROM and screen bits (4 and 3) can't
// be found out at all, and BANK_M only has them right if the 128K ROM did the last
// paging.  A game that pages by itself leaves BANK_M alone, or uses its address for
// something else, and the game's screen then goes blank or the wrong ROM is paged
// in.  So when a 128K snapshot was loaded by the pico, its port value (loaded) is
// used for those bits unless BANK_M has been written since the load (it is not the
// byte the snapshot had) and looks like a port value of the bank that is paged in.
// The lock bit is clear, paging worked.
static inline uint8_t choose_7ffd(const uint8_t* buf, const uint8_t* info, const LoadedPaging* loaded = nullptr) {
    uint8_t bankm = info[SV_BANKM];
    uint8_t value = bankm & 0x1F;
    if (loaded && loaded->valid) {
        bool untouched = bankm == loaded->bankm;
        bool looks_like_port = (bankm & 0xE0) == 0 && find_hashed_bank(buf, info, bankm & 7) == (bankm & 7);
        if (untouched || !looks_like_port) {
            value = loaded->port & 0x1F;
        }
    }
    int bank = find_hashed_bank(buf, info, value & 7);
    if (bank >= 0) {
        value = (uint8_t) ((value & 0x18) | bank);
    }
    return value;
}

// Once the NMI rom has sent the header and the banks. buf has the banks.  The
// header goes in the front of buf and the 7FFD value is returned
static inline uint8_t finish_save128(uint8_t* buf, const uint8_t* info, const LoadedPaging* loaded = nullptr) {
    uint16_t sp = (uint16_t) (info[23] | (info[24] << 8));
    sp = (uint16_t) (sp + 2);                   // the PC was on the stack
    uint8_t s[27];
    memcpy(s, info, sizeof(s));
    s[23] = (uint8_t) (sp & 0xFF);
    s[24] = (uint8_t) (sp >> 8);
    sna_to_z80_header(s, info[SV_PC], info[SV_PC + 1], buf);
    uint8_t port = choose_7ffd(buf, info, loaded);
    buf[X_7FFD] = port;
    buf[X_AYFLAG] = 1;
    buf[X_AYSEL] = 0;                           // the register selected can't be found out
    memcpy(buf + X_AYREGS, info + SV_AY, 16);
    return port;
}

// Run length encodes as a version 2 or 3 .z80 file does.  Emit is a function
// object taking a byte.
template <class Emit>
static void z80_encode_page(const uint8_t* d, uint32_t n, Emit& emit) {
    uint32_t i = 0;
    while (i < n) {
        uint8_t b = d[i];
        uint32_t j = i;
        while (j < n && d[j] == b && j - i < 255) {
            j++;
        }
        uint32_t run = j - i;
        if (run >= 5 || (b == 0xED && run >= 2)) {
            emit(0xED);
            emit(0xED);
            emit((uint8_t) run);
            emit(b);
            i = j;
        } else {
            emit(b);
            i++;
            if (b == 0xED && i < n) {       // a single ED is followed by an ordinary byte
                emit(d[i]);
                i++;
            }
        }
    }
}

namespace detail {
struct CountBytes {
    uint32_t n = 0;
    void operator()(uint8_t) { n++; }
};

// Collects bytes and passes them on to the sink in pieces
template <class Sink>
struct Buffered {
    Sink& sink;
    uint8_t* buf;
    uint32_t cap;
    uint32_t len = 0;
    bool ok = true;
    Buffered(Sink& s, uint8_t* b, uint32_t c) : sink(s), buf(b), cap(c) {}
    void operator()(uint8_t v) {
        buf[len++] = v;
        if (len == cap) {
            flush();
        }
    }
    void put(const uint8_t* d, uint32_t n) {
        while (n) {
            uint32_t k = n < cap - len ? n : cap - len;
            memcpy(buf + len, d, k);
            len += k;
            d += k;
            n -= k;
            if (len == cap) {
                flush();
            }
        }
    }
    void flush() {
        if (len && ok) {
            ok = sink.write(buf, len);
        }
        len = 0;
    }
};
}   // namespace detail

// Writes buf (as finish_save128 leaves it) to sink, which has
// "bool write(const uint8_t* data, uint32_t size)", as a version 3 .z80 file for a 128K
// machine.  A page that doesn't get smaller is stored as it is.  iobuf is a work area.
template <class Sink>
bool write_z80_128(Sink& sink, uint8_t* buf, uint8_t* iobuf, uint32_t iosize) {
    detail::Buffered<Sink> out(sink, iobuf, iosize);

    uint8_t h[30 + 2 + 54];
    memset(h, 0, sizeof(h));
    memcpy(h, buf, 30);
    h[6] = h[7] = 0;                            // the PC is in the extra header
    h[12] &= (uint8_t) ~0x20;                   // not compressed this way
    h[30] = 54;                                 // version 3
    h[31] = 0;
    h[32] = buf[6];
    h[33] = buf[7];
    h[34] = 4;                                  // 128K
    h[35] = buf[X_7FFD];
    h[37] = buf[X_AYFLAG] ? 4 : 0;              // sound chip in use
    h[38] = buf[X_AYSEL];
    memcpy(h + 39, buf + X_AYREGS, 16);
    out.put(h, sizeof(h));

    for (uint32_t bank = 0; bank < 8; bank++) {
        const uint8_t* page = bank_memory(buf, bank);
        detail::CountBytes counter;
        z80_encode_page(page, PAGE_SIZE, counter);
        uint8_t ph[3];
        ph[2] = (uint8_t) (bank + 3);
        if (counter.n >= PAGE_SIZE) {
            ph[0] = ph[1] = 0xFF;
            out.put(ph, 3);
            out.put(page, PAGE_SIZE);
        } else {
            ph[0] = (uint8_t) (counter.n & 0xFF);
            ph[1] = (uint8_t) (counter.n >> 8);
            out.put(ph, 3);
            z80_encode_page(page, PAGE_SIZE, out);
        }
    }
    out.flush();
    return out.ok;
}

// Writes a 48K snapshot, as the NMI rom sent it (a .sna: the 27 byte header and the
// 48K, with the PC on the stack), as a version 3 .z80 file for a 48K machine.
// buf is SNA48_SIZE bytes. iobuf is a work area.
// pc: -1 when the PC is on the stack (as the NMI rom sends it); otherwise the PC, and SP in
// the header is the real one (the tape converter knows the PC, so nothing is pushed).
template <class Sink>
bool write_z80_48(Sink& sink, const uint8_t* buf, uint8_t* iobuf, uint32_t iosize, int32_t pc = -1) {
    detail::Buffered<Sink> out(sink, iobuf, iosize);
    const uint8_t* ram = buf + 27;              // address 0x4000
    uint16_t sp = (uint16_t) (buf[23] | (buf[24] << 8));
    uint8_t pcLow = 0, pcHigh = 0;
    if (pc >= 0) {
        pcLow = (uint8_t) (pc & 0xFF);
        pcHigh = (uint8_t) ((pc >> 8) & 0xFF);
    }
    else if (sp >= 0x4000 && sp <= 0xFFFE) {    // the PC is on the stack: take it off
        pcLow = ram[sp - 0x4000];
        pcHigh = ram[sp - 0x4000 + 1];
        sp = (uint16_t) (sp + 2);
    }
    uint8_t s[27];
    memcpy(s, buf, sizeof(s));
    s[23] = (uint8_t) (sp & 0xFF);
    s[24] = (uint8_t) (sp >> 8);

    uint8_t h[30 + 2 + 54];
    memset(h, 0, sizeof(h));
    sna_to_z80_header(s, 0, 0, h);              // the PC is in the extra header
    h[30] = 54;                                 // version 3
    h[32] = pcLow;
    h[33] = pcHigh;
    h[34] = 0;                                  // 48K
    out.put(h, sizeof(h));

    // page 8 is 0x4000, page 4 is 0x8000 and page 5 is 0xC000
    static const uint8_t pageNumber[3] = { 8, 4, 5 };
    for (uint32_t i = 0; i < 3; i++) {
        const uint8_t* page = ram + i * PAGE_SIZE;
        detail::CountBytes counter;
        z80_encode_page(page, PAGE_SIZE, counter);
        uint8_t ph[3];
        ph[2] = pageNumber[i];
        if (counter.n >= PAGE_SIZE) {
            ph[0] = ph[1] = 0xFF;
            out.put(ph, 3);
            out.put(page, PAGE_SIZE);
        } else {
            ph[0] = (uint8_t) (counter.n & 0xFF);
            ph[1] = (uint8_t) (counter.n >> 8);
            out.put(ph, 3);
            z80_encode_page(page, PAGE_SIZE, out);
        }
    }
    out.flush();
    return out.ok;
}

// Writes a 128K snapshot (buf and info as for write_z80_128, after finish_save128,
// which returned port) as a 128K .sna file: the 27 byte header, banks 5, 2 and the
// one paged in at 0xC000, then the PC, the paging port value and a TR-DOS flag, and
// then the other banks in order. The sound chip registers have no place in it.
template <class Sink>
bool write_sna_128(Sink& sink, uint8_t* buf, const uint8_t* info, uint8_t port, uint8_t* iobuf, uint32_t iosize) {
    detail::Buffered<Sink> out(sink, iobuf, iosize);
    uint16_t sp = (uint16_t) (info[23] | (info[24] << 8));
    sp = (uint16_t) (sp + 2);                   // the PC was on the stack, and is not in a 128K file
    uint8_t s[27];
    memcpy(s, info, sizeof(s));
    s[23] = (uint8_t) (sp & 0xFF);
    s[24] = (uint8_t) (sp >> 8);
    out.put(s, sizeof(s));

    uint32_t paged = port & 7;
    out.put(bank_memory(buf, 5), PAGE_SIZE);
    out.put(bank_memory(buf, 2), PAGE_SIZE);
    out.put(bank_memory(buf, paged), PAGE_SIZE);
    uint8_t tail[4] = { info[SV_PC], info[SV_PC + 1], port, 0 };
    out.put(tail, sizeof(tail));
    for (uint32_t bank = 0; bank < 8; bank++) {
        if (bank != 5 && bank != 2 && bank != paged) {
            out.put(bank_memory(buf, bank), PAGE_SIZE);
        }
    }
    out.flush();
    return out.ok;
}

// ---------------------------------------------------------------------------
// .szx files (the Spectaculator / Fuse snapshot format)
//
// A header, "ZXST", version 1, minor version, machine, flags, then blocks of
// (4 letter id, 32 bit size, data).  Used here:
//   Z80R  the registers      SPCR  border and paging ports
//   AY    sound chip         RAMP  a 16K page, which may be zlib compressed
// Other blocks (keyboard, joystick, tape, disks, ...) are skipped.  The timing
// information is ignored.

// A small inflate for the zlib streams in RAMP blocks, after Mark Adler's puff.c
// (zlib licence).  It decodes straight into the page, which serves as the window,
// and keeps its state in static storage because the Pico's stack is small.
namespace detail {

struct Huff {
    int16_t count[16];
    int16_t symbol[288];
};

template <class R>
struct Inflater {
    R* in;
    uint32_t left;              // bytes of the compressed data not read yet
    uint8_t* out;
    uint32_t outlen;
    uint32_t outcnt;
    uint32_t bitbuf;
    int bitcnt;
    bool err;
    Huff lencode;
    Huff distcode;
    int16_t lengths[320];

    uint32_t bits(int need) {
        uint32_t val = bitbuf;
        while (bitcnt < need) {
            int c = left ? in->get() : -1;
            if (c < 0) {
                err = true;
                return 0;
            }
            left--;
            val |= (uint32_t) c << bitcnt;
            bitcnt += 8;
        }
        bitbuf = val >> need;
        bitcnt -= need;
        return val & ((1u << need) - 1);
    }

    static int construct(Huff& h, const int16_t* length, int n) {
        int16_t offs[16];
        for (int len = 0; len <= 15; len++) h.count[len] = 0;
        for (int symbol = 0; symbol < n; symbol++) h.count[length[symbol]]++;
        if (h.count[0] == n) return 0;                  // no codes: complete
        int left = 1;
        for (int len = 1; len <= 15; len++) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) return left;                  // over-subscribed
        }
        offs[1] = 0;
        for (int len = 1; len < 15; len++) offs[len + 1] = (int16_t) (offs[len] + h.count[len]);
        for (int symbol = 0; symbol < n; symbol++) {
            if (length[symbol] != 0) h.symbol[offs[length[symbol]]++] = (int16_t) symbol;
        }
        return left;                                    // 0 complete, > 0 incomplete
    }

    int decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; len++) {
            code |= (int) bits(1);
            if (err) return -1;
            int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    bool codes() {
        static const int16_t lens[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
            35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
        static const int16_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
            3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        static const int16_t dists[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
            257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        static const int16_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
            7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
        int symbol;
        do {
            symbol = decode(lencode);
            if (symbol < 0) return false;
            if (symbol < 256) {
                if (outcnt >= outlen) return false;
                out[outcnt++] = (uint8_t) symbol;
            } else if (symbol > 256) {
                symbol -= 257;
                if (symbol >= 29) return false;
                uint32_t len = (uint32_t) lens[symbol] + bits(lext[symbol]);
                symbol = decode(distcode);
                if (symbol < 0 || symbol >= 30) return false;
                uint32_t dist = (uint32_t) dists[symbol] + bits(dext[symbol]);
                if (err || dist > outcnt || outcnt + len > outlen) return false;
                while (len--) {
                    out[outcnt] = out[outcnt - dist];
                    outcnt++;
                }
            }
        } while (symbol != 256);
        return !err;
    }

    bool stored() {
        bitbuf = 0;
        bitcnt = 0;
        uint8_t h[4];
        for (int i = 0; i < 4; i++) {
            int c = left ? in->get() : -1;
            if (c < 0) return false;
            left--;
            h[i] = (uint8_t) c;
        }
        uint32_t len = h[0] | (h[1] << 8);
        if (len != (uint32_t) (~(h[2] | (h[3] << 8)) & 0xFFFF)) return false;
        if (len > left || outcnt + len > outlen) return false;
        if (!in->read(out + outcnt, len)) return false;
        left -= len;
        outcnt += len;
        return true;
    }

    bool fixed() {
        int symbol = 0;
        for (; symbol < 144; symbol++) lengths[symbol] = 8;
        for (; symbol < 256; symbol++) lengths[symbol] = 9;
        for (; symbol < 280; symbol++) lengths[symbol] = 7;
        for (; symbol < 288; symbol++) lengths[symbol] = 8;
        construct(lencode, lengths, 288);
        for (symbol = 0; symbol < 30; symbol++) lengths[symbol] = 5;
        construct(distcode, lengths, 30);
        return codes();
    }

    bool dynamic() {
        static const int16_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
        int nlen = (int) bits(5) + 257;
        int ndist = (int) bits(5) + 1;
        int ncode = (int) bits(4) + 4;
        if (err || nlen > 286 || ndist > 30) return false;
        int index = 0;
        for (; index < ncode; index++) lengths[order[index]] = (int16_t) bits(3);
        for (; index < 19; index++) lengths[order[index]] = 0;
        if (err || construct(lencode, lengths, 19) != 0) return false;
        index = 0;
        while (index < nlen + ndist) {
            int symbol = decode(lencode);
            if (symbol < 0) return false;
            if (symbol < 16) {
                lengths[index++] = (int16_t) symbol;
            } else {
                int len = 0;
                int rep;
                if (symbol == 16) {
                    if (index == 0) return false;
                    len = lengths[index - 1];
                    rep = 3 + (int) bits(2);
                } else if (symbol == 17) {
                    rep = 3 + (int) bits(3);
                } else {
                    rep = 11 + (int) bits(7);
                }
                if (err || index + rep > nlen + ndist) return false;
                while (rep--) lengths[index++] = (int16_t) len;
            }
        }
        if (lengths[256] == 0) return false;            // no end of block code
        int e = construct(lencode, lengths, nlen);
        if (e < 0 || (e > 0 && nlen - lencode.count[0] != 1)) return false;
        e = construct(distcode, lengths + nlen, ndist);
        if (e < 0 || (e > 0 && ndist - distcode.count[0] != 1)) return false;
        return codes();
    }

    // Decodes a zlib stream of at most avail bytes from r into dst, which holds dstlen
    // bytes. Returns the number of bytes it made, or -1. What is left of the stream
    // (the checksum, or all of it after an error) is in `left` for the caller to skip.
    int run(R& r, uint32_t avail, uint8_t* dst, uint32_t dstlen) {
        in = &r;
        left = avail;
        out = dst;
        outlen = dstlen;
        outcnt = 0;
        bitbuf = 0;
        bitcnt = 0;
        err = false;
        uint8_t zh[2];
        for (int i = 0; i < 2; i++) {
            int c = left ? in->get() : -1;
            if (c < 0) return -1;
            left--;
            zh[i] = (uint8_t) c;
        }
        if ((zh[0] & 0x0F) != 8 || ((zh[0] << 8) | zh[1]) % 31 != 0 || (zh[1] & 0x20)) return -1;
        uint32_t last;
        do {
            last = bits(1);
            uint32_t type = bits(2);
            if (err) return -1;
            bool ok = type == 0 ? stored() : type == 1 ? fixed() : type == 2 ? dynamic() : false;
            if (!ok) return -1;
        } while (!last);
        return (int) outcnt;
    }
};

}   // namespace detail

// zlib-decodes at most avail bytes from r into dst (dstlen bytes); returns how many
// bytes came out, or -1, and always consumes exactly avail bytes of r.
template <class R>
static int szx_inflate(R& r, uint32_t avail, uint8_t* dst, uint32_t dstlen) {
    static detail::Inflater<R> st;
    int n = st.run(r, avail, dst, dstlen);
    uint32_t rest = st.left;
    if (!r.skip(rest)) {
        return -1;
    }
    return n;
}

static const uint32_t SZX_Z80R_SIZE = 37;
static const uint32_t SZX_SPCR_SIZE = 8;
static const uint32_t SZX_AY_SIZE = 18;

static inline uint32_t szx_le32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

// Loads an .szx file into buf as a KIND_Z80V1 (16K, 48K) or KIND_PAGED (128K, +2) snapshot.
template <class File>
Kind load_szx(File& f, uint8_t* buf, uint8_t* iobuf, uint32_t iosize) {
    Reader<File> r(f, iobuf, iosize);
    uint8_t h[8];
    if (!r.read(h, sizeof(h)) || memcmp(h, "ZXST", 4) != 0 || h[4] != 1) {
        return KIND_ERROR;
    }
    uint8_t machine = h[6];
    bool is48 = machine == 0 || machine == 1 || machine == 8 || machine == 15;     // 16K, 48K, TC2048, NTSC 48K
    bool is128 = machine == 2 || machine == 3 || machine == 16;                    // 128K, +2, 128Ke (the +2A and +3 have other ROMs)
    if (!is48 && !is128) {
        return KIND_ERROR;
    }
    uint8_t* ram48 = buf + 30;
    if (is48) {
        memset(buf, 0, 30 + Z80_RAM48_SIZE);
    } else {
        memset(buf, 0, BUFFER128_SIZE);
    }
    uint8_t z[SZX_Z80R_SIZE];
    uint8_t sp[SZX_SPCR_SIZE];
    uint8_t ay[SZX_AY_SIZE];
    bool haveZ = false, haveSp = false, haveAy = false;
    memset(sp, 0, sizeof(sp));

    uint8_t bh[8];
    while (r.read(bh, sizeof(bh))) {
        uint32_t size = szx_le32(bh + 4);
        if (memcmp(bh, "Z80R", 4) == 0 && size >= SZX_Z80R_SIZE) {
            if (!r.read(z, sizeof(z)) || !r.skip(size - SZX_Z80R_SIZE)) return KIND_ERROR;
            haveZ = true;
        } else if (memcmp(bh, "SPCR", 4) == 0 && size >= SZX_SPCR_SIZE) {
            if (!r.read(sp, sizeof(sp)) || !r.skip(size - SZX_SPCR_SIZE)) return KIND_ERROR;
            haveSp = true;
        } else if (memcmp(bh, "AY\0\0", 4) == 0 && size >= SZX_AY_SIZE) {
            if (!r.read(ay, sizeof(ay)) || !r.skip(size - SZX_AY_SIZE)) return KIND_ERROR;
            haveAy = true;
        } else if (memcmp(bh, "RAMP", 4) == 0 && size >= 3) {
            uint8_t ph[3];
            if (!r.read(ph, sizeof(ph))) return KIND_ERROR;
            uint32_t flags = ph[0] | (ph[1] << 8);
            uint32_t page = ph[2];
            uint32_t datalen = size - 3;
            uint8_t* dst = nullptr;
            if (is48) {
                dst = page == 5 ? ram48 : page == 2 ? ram48 + PAGE_SIZE : page == 0 ? ram48 + 2 * PAGE_SIZE : nullptr;
            } else if (page < 8) {
                dst = bank_memory(buf, page);
            }
            if (dst == nullptr) {
                if (!r.skip(datalen)) return KIND_ERROR;
            } else if (flags & 1) {
                if (szx_inflate(r, datalen, dst, PAGE_SIZE) < 0) {
                    return KIND_ERROR;
                }
            } else {
                if (datalen < PAGE_SIZE || !r.read(dst, PAGE_SIZE) || !r.skip(datalen - PAGE_SIZE)) return KIND_ERROR;
            }
        } else if (!r.skip(size)) {
            return KIND_ERROR;
        }
    }
    if (!haveZ) {
        return KIND_ERROR;
    }

    // the version 1 .z80 style header
    buf[0] = z[1];  buf[1] = z[0];                      // A, F
    buf[2] = z[2];  buf[3] = z[3];                      // BC
    buf[4] = z[6];  buf[5] = z[7];                      // HL
    buf[6] = z[22]; buf[7] = z[23];                     // PC
    buf[8] = z[20]; buf[9] = z[21];                     // SP
    buf[10] = z[24];                                    // I
    buf[11] = (uint8_t) (z[25] & 0x7F);                 // R
    buf[12] = (uint8_t) (((sp[0] & 7) << 1) | (z[25] >> 7));
    buf[13] = z[4]; buf[14] = z[5];                     // DE
    buf[15] = z[10]; buf[16] = z[11];                   // BC'
    buf[17] = z[12]; buf[18] = z[13];                   // DE'
    buf[19] = z[14]; buf[20] = z[15];                   // HL'
    buf[21] = z[9]; buf[22] = z[8];                     // A', F'
    buf[23] = z[18]; buf[24] = z[19];                   // IY
    buf[25] = z[16]; buf[26] = z[17];                   // IX
    buf[27] = z[26] ? 1 : 0;                            // IFF1
    buf[28] = z[27] ? 1 : 0;                            // IFF2
    buf[29] = (uint8_t) (z[28] & 3);                    // interrupt mode
    (void) haveSp;
    if (is48) {
        return KIND_Z80V1;
    }
    buf[X_7FFD] = sp[1];
    if (haveAy) {
        buf[X_AYFLAG] = 1;
        buf[X_AYSEL] = ay[1];
        memcpy(buf + X_AYREGS, ay + 2, 16);
    }
    return KIND_PAGED;
}


// Writing .szx files. The RAM is stored as it is (uncompressed pages are allowed),
// which is why the file for a 128K game is about 131K.

namespace detail {
static inline void szx_put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24);
}

// the file header, the creator block and the registers and the paging ports. s is the 27 byte .sna
// header (with the stack pointer already corrected), pc the PC, port the value for 0x7FFD.
template <class Sink>
static void szx_start(Buffered<Sink>& out, uint8_t machine, const uint8_t* s, uint8_t pcLow, uint8_t pcHigh,
        uint8_t port) {
    uint8_t h[8] = { 'Z', 'X', 'S', 'T', 1, 5, machine, 0 };
    out.put(h, sizeof(h));

    uint8_t c[8 + 36];                                  // who made it
    memset(c, 0, sizeof(c));
    memcpy(c, "CRTR", 4);
    szx_put32(c + 4, 36);
    memcpy(c + 8, "Picoface", 8);
    out.put(c, sizeof(c));

    uint8_t z[8 + SZX_Z80R_SIZE];
    memset(z, 0, sizeof(z));
    memcpy(z, "Z80R", 4);
    szx_put32(z + 4, SZX_Z80R_SIZE);
    uint8_t* r = z + 8;
    r[0] = s[21]; r[1] = s[22];                         // F, A
    r[2] = s[13]; r[3] = s[14];                         // BC
    r[4] = s[11]; r[5] = s[12];                         // DE
    r[6] = s[9];  r[7] = s[10];                         // HL
    r[8] = s[7];  r[9] = s[8];                          // F', A'
    r[10] = s[5]; r[11] = s[6];                         // BC'
    r[12] = s[3]; r[13] = s[4];                         // DE'
    r[14] = s[1]; r[15] = s[2];                         // HL'
    r[16] = s[17]; r[17] = s[18];                       // IX
    r[18] = s[15]; r[19] = s[16];                       // IY
    r[20] = s[23]; r[21] = s[24];                       // SP
    r[22] = pcLow; r[23] = pcHigh;                      // PC
    r[24] = s[0];                                       // I
    r[25] = s[20];                                      // R
    r[26] = r[27] = (uint8_t) ((s[19] >> 2) & 1);       // IFF1, IFF2
    r[28] = (uint8_t) (s[25] & 3);                      // interrupt mode
    out.put(z, sizeof(z));

    uint8_t p[8 + SZX_SPCR_SIZE];
    memset(p, 0, sizeof(p));
    memcpy(p, "SPCR", 4);
    szx_put32(p + 4, SZX_SPCR_SIZE);
    p[8] = (uint8_t) (s[26] & 7);                       // border
    p[9] = port;
    out.put(p, sizeof(p));
}

template <class Sink>
static void szx_page(Buffered<Sink>& out, uint8_t pageNumber, const uint8_t* page) {
    uint8_t b[8 + 3];
    memcpy(b, "RAMP", 4);
    szx_put32(b + 4, 3 + PAGE_SIZE);
    b[8] = b[9] = 0;                                    // not compressed
    b[10] = pageNumber;
    out.put(b, sizeof(b));
    out.put(page, PAGE_SIZE);
}
}   // namespace detail

// A 48K snapshot as the NMI rom sent it (a .sna) as a 48K .szx file
template <class Sink>
bool write_szx_48(Sink& sink, const uint8_t* buf, uint8_t* iobuf, uint32_t iosize) {
    detail::Buffered<Sink> out(sink, iobuf, iosize);
    const uint8_t* ram = buf + 27;
    uint16_t sp = (uint16_t) (buf[23] | (buf[24] << 8));
    uint8_t pcLow = 0, pcHigh = 0;
    if (sp >= 0x4000 && sp <= 0xFFFE) {                 // the PC is on the stack: take it off
        pcLow = ram[sp - 0x4000];
        pcHigh = ram[sp - 0x4000 + 1];
        sp = (uint16_t) (sp + 2);
    }
    uint8_t s[27];
    memcpy(s, buf, sizeof(s));
    s[23] = (uint8_t) (sp & 0xFF);
    s[24] = (uint8_t) (sp >> 8);
    detail::szx_start(out, 1, s, pcLow, pcHigh, 0);
    detail::szx_page(out, 5, ram);
    detail::szx_page(out, 2, ram + PAGE_SIZE);
    detail::szx_page(out, 0, ram + 2 * PAGE_SIZE);
    out.flush();
    return out.ok;
}

// A 128K snapshot (buf and info as for write_z80_128, after finish_save128, which
// returned port) as a 128K .szx file with the sound chip registers
template <class Sink>
bool write_szx_128(Sink& sink, uint8_t* buf, const uint8_t* info, uint8_t port, uint8_t* iobuf, uint32_t iosize) {
    detail::Buffered<Sink> out(sink, iobuf, iosize);
    uint16_t sp = (uint16_t) (info[23] | (info[24] << 8));
    sp = (uint16_t) (sp + 2);                           // the PC was on the stack
    uint8_t s[27];
    memcpy(s, info, sizeof(s));
    s[23] = (uint8_t) (sp & 0xFF);
    s[24] = (uint8_t) (sp >> 8);
    detail::szx_start(out, 2, s, info[SV_PC], info[SV_PC + 1], port);

    uint8_t a[8 + SZX_AY_SIZE];
    memset(a, 0, sizeof(a));
    memcpy(a, "AY\0\0", 4);
    detail::szx_put32(a + 4, SZX_AY_SIZE);
    a[8] = 2;                                           // the 128K's own sound chip
    a[9] = buf[X_AYSEL];
    memcpy(a + 10, info + SV_AY, 16);
    out.put(a, sizeof(a));

    for (uint32_t bank = 0; bank < 8; bank++) {
        detail::szx_page(out, (uint8_t) bank, bank_memory(buf, bank));
    }
    out.flush();
    return out.ok;
}

}   // namespace snapload

#endif
