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

// Converting a tape (.tzx or .tap) into a 48K snapshot by loading it on an emulated
// Spectrum: the tape's own loader runs, so a turbo or protected loader works the way it
// does on a real machine.
//
//   Player<File>     plays the tape as levels and durations (T-states at 3.5 MHz), the
//                    way a tape deck would: every TZX block that makes a signal, with
//                    loops, jumps and calls.
//   Machine<File>    a 48K Spectrum (ROM, 48K, the ULA port, 50 Hz interrupts) around the
//                    Z80 emulator in z80/. It types LOAD "" and plays the tape, and says
//                    when the game is running or the load went wrong.
//
// No memory is allocated: the caller gives the 16K ROM and the 48K RAM (on the Pico, both
// are in snap_load_buffer, where the RAM is already in its .sna place). File is anything
// with int read(void*, uint32_t), bool seek(uint32_t) and uint32_t tell(); it is the
// Pico's FileHandle there and a FILE* wrapper on a PC, where this is tested.
//
// Not done here (yet): memory contention, the floating bus, and CSW blocks compressed with
// Z-RLE (CSW with plain RLE works).

#ifndef INCLUDED_TZXCONV_H
#define INCLUDED_TZXCONV_H

#include <stdint.h>
#include <string.h>

#ifndef Z80_STATIC
#define Z80_STATIC
#endif
#include "Z80.h"

#ifndef TZXCONV_NOINLINE
#define TZXCONV_NOINLINE __attribute__((noinline))
#endif

namespace tzxconv {

static const uint32_t MS = 3500;                    // T-states in a millisecond

// ---------------------------------------------------------------- the tape

enum class TapeEnd : uint8_t { PLAYING, ENDED, BAD };

template <class File> class Machine;

template <class File>
class Player {
    template <class> friend class Machine;     // (edge_wait's test build looks ahead and goes back)
public:
    // tzx false: a .tap file (only standard speed blocks)
    bool begin(File* f, bool tzx) {
        f_ = f;
        tzx_ = tzx;
        memset(&s_, 0, sizeof(s_));
        level_ = 0;
        end_ = TapeEnd::PLAYING;
        stops_ = 0;
        stops48_ = 0;
        closing_ = 0;
        if (tzx) {
            uint8_t h[10];
            if (!f_->seek(0) || !get(h, 10) || memcmp(h, "ZXTape!\x1A", 8) != 0 || h[8] != 1) {
                end_ = TapeEnd::BAD;
                return false;
            }
        }
        else if (!f_->seek(0)) {
            end_ = TapeEnd::BAD;
            return false;
        }
        block_ = 0;
        data_started_ = 0;
        in_data_ = false;
        read_in_ = loaded_ = 0;
        return true;
    }

    // The next piece of the signal: the level (0 or 1) held for dur T-states. False at
    // the end of the tape (end() then says whether it was a good end).
    bool next(uint32_t& dur, uint8_t& level) {
        while (true) {
            if (step(dur)) {
                level = level_;
                return true;
            }
            if (end_ != TapeEnd::PLAYING || !next_block()) {
                if (end_ == TapeEnd::PLAYING) {
                    // The end of the tape: the last pulse still needs its closing edge (a
                    // last data block with no pause), then the level stays low a moment.
                    if (closing_ < 2) {
                        level_ = closing_ == 0 ? level_ ^ 1 : 0;
                        closing_++;
                        dur = MS;
                        level = level_;
                        return true;
                    }
                    end_ = TapeEnd::ENDED;
                }
                return false;
            }
        }
    }

    TapeEnd end() const { return end_; }
    // 0: silence (a pause, or between blocks), 1: a tone or pulses, 2: data bits
    uint8_t playing_now() const {
        switch (s_.phase) {
        case DATA: case DIRECT: case CSW: case GEN_DATA: return 2;
        case TONE: case SYNC1: case SYNC2: case PULSES: case GEN_PILOT: return 1;
        default: return 0;
        }
    }
    uint32_t stops() const { return stops_; }       // "stop the tape" blocks played through
    uint32_t stops48() const { return stops48_; }   // of them, "stop the tape if a 48K" (0x2A)
    // Where the tape is, in data blocks (standard and turbo speed and pure data: those the
    // Picoface's tape trap hands over, counted as it counts them): the next one a load would
    // take. A data block that has begun but not ended is taken again from its start.
    uint32_t data_position() const {
        return data_started_ - (in_data_ && !quiet() && data_started_ > 0 ? 1 : 0);
    }
    // What has been loaded: the data blocks up to the last one a loader took in (counted as
    // data_position counts them). A block counts when the tape port was read while its data
    // played, in a frame in which the Spectrum was loading (Machine::frame_checks: bytes
    // stored one after another, or the ROM's loader), or when it was flash loaded. The blocks
    // after it played to no one (a game waiting for a key while the tape runs on), so a load
    // made later (a multi-load game's next level) wants the first of them.
    uint32_t loaded_position() const { return loaded_; }
    void note_read() {
        if (in_data_ && playing_now() == 2) read_in_ = data_started_;
    }
    void note_loading() {
        if (read_in_ > loaded_) loaded_ = read_in_;
    }
    // In a silence (a pause, or between blocks) with another block that makes a signal after
    // it (not only the end of the tape): the tape can be held here (Machine::hold_tape)
    bool silent() const { return quiet(); }
    bool silence_before_signal() {
        if (!quiet() || end_ != TapeEnd::PLAYING) return false;
        Saved sv;
        save(sv);
        bool found = false;
        for (int i = 0; i < 8 && end_ == TapeEnd::PLAYING; i++) {
            if (!next_block()) break;
            if (!quiet()) {
                found = true;
                break;
            }
        }
        restore(sv);
        return found;
    }
    uint32_t blocks() const { return block_; }      // blocks started

    // ---- flash loading (the ROM's LD-BYTES done at once: see Machine::flash_load)
    // The standard speed block the ROM's loader would take in if it began waiting for a
    // signal now: the block playing, if it is still early enough in its pilot for the ROM to
    // find it, or after a pause or silence the next block that makes a signal (not past a
    // "stop the tape" or the end). It must have the flag byte 'flag' and at least 'min_len'
    // bytes. True: the file is then at its data (flag byte first), and len is its length.
    // False: nothing has changed.
    bool flash_ready(uint8_t flag, uint32_t min_len, uint32_t& len) {
        Saved sv;
        save(sv);
        if (quiet()) {
            uint32_t stops = stops_;
            for (int i = 0; quiet(); i++) {
                if (i >= 8 || end_ != TapeEnd::PLAYING || !next_block() || stops_ != stops) {
                    restore(sv);
                    return false;
                }
            }
        }
        uint8_t got;
        // the ROM waits about a second after the first edge it sees, then needs 256 pulses of
        // pilot: 1.2 s of it must be left
        bool ok = s_.phase == TONE && (uint64_t) s_.tone_left * s_.pilot >= 4200000 &&
                  in(s_.pilot, 1900, 2450) && in(s_.sync1, 400, 900) && in(s_.sync2, 450, 1000) &&
                  in(s_.zero, 700, 1000) && in(s_.one, 1450, 2000) && s_.used_last == 8 &&
                  s_.data_left >= min_len && get(&got, 1) && got == flag && f_->seek(f_->tell() - 1);
        if (!ok) {
            restore(sv);
            return false;
        }
        len = s_.data_left;
        return true;
    }
    // After flash_ready: the level the signal will be at after the pilot and syncs (the data
    // has an even number of edges, so also at its end)
    uint8_t flash_level() const {
        return (uint8_t) (level_ ^ ((s_.tone_left + (s_.sync1 ? 1 : 0) + (s_.sync2 ? 1 : 0)) & 1));
    }
    // After flash_ready: the next bytes of the data
    bool flash_read(uint8_t* p, uint32_t n) {
        if (n > s_.data_left || !get(p, n)) return bad();
        s_.data_left -= n;
        return true;
    }
    // After flash_ready: the rest of the block is skipped, and its pause starts
    void flash_done() {
        read_in_ = loaded_ = data_started_;
        level_ = flash_level();
        if (s_.data_left > 0 && !skip(s_.data_left)) {
            bad();
            return;
        }
        s_.data_left = 0;
        start_pause(s_.pause_ms);
    }

private:
    struct Saved;
    static bool in(uint32_t v, uint32_t lo, uint32_t hi) { return v >= lo && v <= hi; }
    bool quiet() const { return s_.phase == IDLE || s_.phase == PAUSE1 || s_.phase == PAUSE2; }

    enum Phase : uint8_t { IDLE, TONE, SYNC1, SYNC2, PULSES, DATA, DIRECT, CSW, GEN_PILOT, GEN_DATA, PAUSE1, PAUSE2 };

    struct State {
        Phase phase;
        // signal of the block being played
        uint16_t pilot, sync1, sync2, zero, one;
        uint32_t tone_left;             // pulses of the pilot or pure tone left
        uint32_t data_left;             // data bytes left (DATA, DIRECT, CSW)
        uint8_t used_last;              // bits used in the last byte
        uint8_t byte, bits_left, half;  // DATA: byte being sent, its bits left, 2nd pulse of a bit
        uint32_t pause_ms;
        uint32_t pulses_left;           // PULSES: the pulse lengths are read from the file
        // direct recording / CSW
        uint32_t sample_t;              // DIRECT: T per sample
        uint32_t csw_num, csw_den, csw_frac;    // CSW: T = samples * num / den
        // generalised data (0x19)
        uint32_t g_totp, g_totd, g_pos; // symbols of the pilot (as runs) / data, done so far
        uint32_t g_pdef, g_prle, g_ddef, g_dstream, g_end;  // file offsets
        uint8_t g_npp, g_npd, g_nb;
        uint16_t g_asp, g_asd;
        uint16_t g_sym, g_rep;          // pilot: symbol, repeats left
        uint8_t g_pulse, g_np;          // pulse of the symbol being played, pulses in its definition
        uint32_t g_symoff;              // file offset of the symbol's pulses
        uint32_t g_bitpos;              // data stream: bit position
        // control
        uint32_t loop_start, loop_left; // a loop (0x24) being played: offset after it, times left
        uint32_t call_at, call_n, call_i;       // a call sequence (0x26) being played
        uint32_t loop_block, call_block, call_after;    // block numbers, for jumps after them
        bool in_call;
        uint32_t idle;                  // control blocks in a row, so a bad file can't spin for ever
    };

    // T-states of a pause, and the 1 ms before it at the old level
    bool start_pause(uint32_t ms) {
        if (ms == 0) {
            s_.phase = IDLE;
            return false;
        }
        s_.pause_ms = ms;
        s_.phase = PAUSE1;
        return true;
    }

    // One piece of the current block, or false when the block is done
    bool step(uint32_t& dur) {
        switch (s_.phase) {
        case IDLE:
            return false;
        case TONE:
            if (s_.tone_left == 0) {
                s_.phase = SYNC1;
                return step(dur);
            }
            s_.tone_left--;
            level_ ^= 1;
            dur = s_.pilot;
            return true;
        case SYNC1:
            s_.phase = SYNC2;
            if (s_.sync1 == 0) return step(dur);
            level_ ^= 1;
            dur = s_.sync1;
            return true;
        case SYNC2:
            s_.phase = DATA;
            s_.bits_left = 0;
            s_.half = 0;
            if (s_.sync2 == 0) return step(dur);
            level_ ^= 1;
            dur = s_.sync2;
            return true;
        case DATA: {
            if (s_.half == 0 && s_.bits_left == 0) {
                if (s_.data_left == 0) return start_pause(s_.pause_ms) && step(dur);
                uint8_t b;
                if (!get(&b, 1)) return bad();
                s_.byte = b;
                s_.data_left--;
                s_.bits_left = s_.data_left == 0 ? (s_.used_last ? s_.used_last : 8) : 8;
            }
            bool bit = (s_.byte & 0x80) != 0;
            dur = bit ? s_.one : s_.zero;
            level_ ^= 1;
            if (s_.half) {
                s_.half = 0;
                s_.byte <<= 1;
                s_.bits_left--;
            }
            else {
                s_.half = 1;
            }
            return true;
        }
        case PULSES: {
            if (s_.pulses_left == 0) {
                s_.phase = IDLE;
                return false;
            }
            uint8_t p[2];
            if (!get(p, 2)) return bad();
            s_.pulses_left--;
            level_ ^= 1;
            dur = p[0] | (p[1] << 8);
            return true;
        }
        case DIRECT: {
            // runs of the same level are given as one piece
            if (s_.data_left == 0 && s_.bits_left == 0) return start_pause(s_.pause_ms) && step(dur);
            uint32_t n = 0;
            uint8_t want = 2;
            while (true) {
                if (s_.bits_left == 0) {
                    if (s_.data_left == 0) break;
                    uint8_t b;
                    if (!get(&b, 1)) return bad();
                    s_.byte = b;
                    s_.data_left--;
                    s_.bits_left = s_.data_left == 0 ? (s_.used_last ? s_.used_last : 8) : 8;
                }
                uint8_t bit = (s_.byte >> 7) & 1;
                if (want == 2) want = bit;
                else if (bit != want) break;
                s_.byte <<= 1;
                s_.bits_left--;
                n++;
            }
            level_ = want;
            dur = n * s_.sample_t;
            return true;
        }
        case CSW: {
            if (s_.data_left == 0) return start_pause(s_.pause_ms) && step(dur);
            uint8_t b;
            if (!get(&b, 1)) return bad();
            s_.data_left--;
            uint32_t samples = b;
            if (b == 0) {
                uint8_t q[4];
                if (s_.data_left < 4 || !get(q, 4)) return bad();
                s_.data_left -= 4;
                samples = q[0] | (q[1] << 8) | (q[2] << 16) | ((uint32_t) q[3] << 24);
            }
            uint64_t t = (uint64_t) samples * s_.csw_num + s_.csw_frac;
            dur = (uint32_t) (t / s_.csw_den);
            s_.csw_frac = (uint32_t) (t % s_.csw_den);
            level_ ^= 1;
            return true;
        }
        case GEN_PILOT:
        case GEN_DATA:
            return gen_step(dur);
        case PAUSE1:
            // the last pulse ends with an edge, which is held for 1 ms, then the level is low
            s_.phase = PAUSE2;
            level_ ^= 1;
            dur = s_.pause_ms > 1 ? MS : s_.pause_ms * MS;
            if (s_.pause_ms <= 1) s_.phase = IDLE;
            return true;
        case PAUSE2:
            s_.phase = IDLE;
            level_ = 0;
            dur = (s_.pause_ms - 1) * MS;
            return true;
        }
        return false;
    }

    // ---- generalised data (0x19)
    bool gen_symbol_start(uint32_t defs, uint8_t np, uint16_t sym) {
        uint32_t off = defs + (uint32_t) sym * (1 + 2u * np);
        uint8_t flags;
        if (!f_->seek(off) || !get(&flags, 1)) return false;
        switch (flags & 3) {
        case 0: level_ ^= 1; break;     // an edge
        case 1: break;                  // the same level
        case 2: level_ = 0; break;
        case 3: level_ = 1; break;
        }
        s_.g_symoff = off + 1;
        s_.g_pulse = 0;
        s_.g_np = np;
        return true;
    }

    // the next pulse of the symbol being played; false when the symbol is done
    bool gen_symbol_pulse(uint32_t& dur) {
        if (s_.g_pulse >= s_.g_np) return false;
        uint8_t p[2];
        if (!f_->seek(s_.g_symoff + 2u * s_.g_pulse) || !get(p, 2)) return bad();
        uint16_t len = p[0] | (p[1] << 8);
        if (len == 0) {                 // the definition ends early
            s_.g_pulse = s_.g_np;
            return false;
        }
        if (s_.g_pulse > 0) level_ ^= 1;
        s_.g_pulse++;
        dur = len;
        return true;
    }

    bool gen_step(uint32_t& dur) {
        while (true) {
            if (s_.g_np != 0 && gen_symbol_pulse(dur)) return true;
            if (end_ != TapeEnd::PLAYING) return false;
            s_.g_np = 0;
            if (s_.phase == GEN_PILOT) {
                if (s_.g_rep == 0) {
                    if (s_.g_pos >= s_.g_totp) {
                        s_.phase = GEN_DATA;
                        s_.g_pos = 0;
                        continue;
                    }
                    uint8_t e[3];
                    if (!f_->seek(s_.g_prle + 3 * s_.g_pos) || !get(e, 3)) return bad();
                    s_.g_pos++;
                    s_.g_sym = e[0];
                    s_.g_rep = e[1] | (e[2] << 8);
                    if (s_.g_rep == 0) continue;
                    if (s_.g_sym >= s_.g_asp) return bad();
                }
                s_.g_rep--;
                if (!gen_symbol_start(s_.g_pdef, s_.g_npp, s_.g_sym)) return bad();
            }
            else {
                if (s_.g_pos >= s_.g_totd) {
                    if (!f_->seek(s_.g_end)) return bad();
                    return start_pause(s_.pause_ms) && step(dur);
                }
                // NB bits, most significant first
                uint16_t sym = 0;
                for (uint8_t i = 0; i < s_.g_nb; i++) {
                    uint32_t bit = s_.g_bitpos++;
                    uint8_t b;
                    if (!f_->seek(s_.g_dstream + bit / 8) || !get(&b, 1)) return bad();
                    sym = (uint16_t) ((sym << 1) | ((b >> (7 - bit % 8)) & 1));
                }
                s_.g_pos++;
                if (sym >= s_.g_asd) return bad();
                if (!gen_symbol_start(s_.g_ddef, s_.g_npd, sym)) return bad();
            }
        }
    }

    // ---- blocks

    bool bad() {
        end_ = TapeEnd::BAD;
        s_.phase = IDLE;
        return false;
    }

    bool get(void* p, uint32_t n) {
        return f_->read(p, n) == (int) n;
    }

    static uint32_t le(const uint8_t* p, int n) {
        uint32_t v = 0;
        for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
        return v;
    }

    bool skip(uint32_t n) {
        return f_->seek(f_->tell() + n);
    }

    // Goes to block number 'target' (the first block is 0), by walking the file from the start
    bool goto_block(int32_t target) {
        if (target < 0 || !f_->seek(10)) return false;
        for (int32_t i = 0; i < target; i++) {
            uint8_t id;
            if (!get(&id, 1) || !skip_body(id)) return false;
        }
        block_ = (uint32_t) target;
        return true;
    }

    // Skips the body of a block whose id has been read
    bool skip_body(uint8_t id) {
        uint8_t h[20];
        switch (id) {
        case 0x10: return get(h, 4) && skip(le(h + 2, 2));
        case 0x11: return get(h, 18) && skip(le(h + 15, 3));
        case 0x12: return skip(4);
        case 0x13: return get(h, 1) && skip(2u * h[0]);
        case 0x14: return get(h, 10) && skip(le(h + 7, 3));
        case 0x15: return get(h, 8) && skip(le(h + 5, 3));
        case 0x18: case 0x19: case 0x2B: case 0x16: case 0x17:
            return get(h, 4) && skip(le(h, 4));
        case 0x20: case 0x23: case 0x24: return skip(2);
        case 0x21: case 0x30: return get(h, 1) && skip(h[0]);
        case 0x22: case 0x25: case 0x27: return true;
        case 0x26: return get(h, 2) && skip(2u * le(h, 2));
        case 0x28: case 0x32: return get(h, 2) && skip(le(h, 2));
        case 0x2A: return skip(4);
        case 0x31: return get(h, 2) && skip(h[1]);
        case 0x33: return get(h, 1) && skip(3u * h[0]);
        case 0x34: return skip(8);
        case 0x35: return get(h, 20) && skip(le(h + 16, 4));
        case 0x40: return get(h, 4) && skip(le(h + 1, 3));
        case 0x5A: return skip(9);
        }
        return false;                   // not known: the length can't be found
    }

    // .tap: every block is a standard speed block with a second's pause
    bool next_tap_block() {
        uint8_t h[2];
        if (f_->read(h, 2) != 2) return false;
        uint32_t len = le(h, 2);
        if (len == 0) return next_tap_block();
        uint8_t flag;
        if (!get(&flag, 1) || !f_->seek(f_->tell() - 1)) return bad();
        standard(len, 1000, flag);
        block_++;
        data_started_++;
        in_data_ = true;
        return true;
    }

    void standard(uint32_t len, uint32_t pause, uint8_t flag) {
        s_.pilot = 2168; s_.sync1 = 667; s_.sync2 = 735; s_.zero = 855; s_.one = 1710;
        s_.tone_left = flag < 128 ? 8063 : 3223;
        s_.data_left = len;
        s_.used_last = 8;
        s_.pause_ms = pause;
        s_.phase = TONE;
    }

    // At the end of a block in a call sequence, or after a return, the next one called
    bool next_call() {
        if (s_.call_i >= s_.call_n) {
            s_.in_call = false;
            return f_->seek(s_.call_at);    // after the call sequence block
        }
        uint8_t o[2];
        uint32_t list = s_.call_at - 2u * s_.call_n;
        if (!f_->seek(list + 2u * s_.call_i) || !get(o, 2)) return false;
        int32_t rel = (int16_t) le(o, 2);
        s_.call_i++;
        return goto_block((int32_t) s_.call_block + rel);
    }

    // Reads blocks until one that makes a signal has been started
    bool next_block() {
        if (!tzx_) return next_tap_block();
        in_data_ = false;
        while (true) {
            if (++s_.idle > 4000) return bad();             // a control block loop that never ends
            uint8_t id;
            if (f_->read(&id, 1) != 1) return false;        // the end of the file
            uint32_t at = block_++;
            uint8_t h[20];
            switch (id) {
            case 0x10:
                if (!get(h, 4)) return bad();
                {
                    uint8_t flag = 0xFF;
                    uint32_t len = le(h + 2, 2);
                    if (len > 0 && (!get(&flag, 1) || !f_->seek(f_->tell() - 1))) return bad();
                    standard(len, le(h, 2), flag);
                }
                data_started_++;
                in_data_ = true;
                return started();
            case 0x11:
                if (!get(h, 18)) return bad();
                s_.pilot = le(h, 2); s_.sync1 = le(h + 2, 2); s_.sync2 = le(h + 4, 2);
                s_.zero = le(h + 6, 2); s_.one = le(h + 8, 2); s_.tone_left = le(h + 10, 2);
                s_.used_last = h[12]; s_.pause_ms = le(h + 13, 2); s_.data_left = le(h + 15, 3);
                s_.phase = TONE;
                data_started_++;
                in_data_ = true;
                return started();
            case 0x12:
                if (!get(h, 4)) return bad();
                s_.pilot = le(h, 2); s_.tone_left = le(h + 2, 2);
                s_.sync1 = s_.sync2 = 0; s_.data_left = 0; s_.pause_ms = 0;
                s_.phase = TONE;            // then SYNC1, SYNC2 and DATA, which give nothing
                in_data_ = false;
                return started();
            case 0x13:
                if (!get(h, 1)) return bad();
                s_.pulses_left = h[0];
                s_.phase = PULSES;
                in_data_ = false;
                return started();
            case 0x14:
                if (!get(h, 10)) return bad();
                s_.zero = le(h, 2); s_.one = le(h + 2, 2); s_.used_last = h[4];
                s_.pause_ms = le(h + 5, 2); s_.data_left = le(h + 7, 3);
                s_.sync1 = s_.sync2 = 0; s_.tone_left = 0;
                s_.phase = TONE;
                data_started_++;
                in_data_ = true;
                return started();
            case 0x15:
                if (!get(h, 8)) return bad();
                s_.sample_t = le(h, 2); s_.pause_ms = le(h + 2, 2); s_.used_last = h[4];
                s_.data_left = le(h + 5, 3); s_.bits_left = 0;
                s_.phase = DIRECT;
                in_data_ = false;
                return started();
            case 0x18: {
                if (!get(h, 14)) return bad();
                uint32_t len = le(h, 4);
                uint32_t rate = le(h + 6, 3);
                uint32_t after = f_->tell() + len - 10;
                if (len < 10 || rate == 0) return bad();
                if (h[9] != 1) {            // Z-RLE: not done yet, skipped
                    if (!f_->seek(after)) return bad();
                    unsupported_++;
                    continue;
                }
                s_.pause_ms = le(h + 4, 2);
                s_.csw_num = 3500000; s_.csw_den = rate; s_.csw_frac = 0;
                s_.data_left = len - 10;
                s_.phase = CSW;
                return started();
            }
            case 0x19: {
                if (!get(h, 18)) return bad();
                uint32_t len = le(h, 4);
                uint32_t start = f_->tell();        // after the 18 bytes
                s_.g_end = start - 14 + len;
                s_.pause_ms = le(h + 4, 2);
                s_.g_totp = le(h + 6, 4); s_.g_npp = h[10]; s_.g_asp = h[11] ? h[11] : 256;
                s_.g_totd = le(h + 12, 4); s_.g_npd = h[16]; s_.g_asd = h[17] ? h[17] : 256;
                uint32_t off = start;
                if (s_.g_totp > 0) {
                    s_.g_pdef = off;
                    off += (uint32_t) s_.g_asp * (1 + 2u * s_.g_npp);
                    s_.g_prle = off;
                    off += 3 * s_.g_totp;
                }
                if (s_.g_totd > 0) {
                    s_.g_ddef = off;
                    off += (uint32_t) s_.g_asd * (1 + 2u * s_.g_npd);
                    s_.g_dstream = off;
                }
                s_.g_nb = 0;
                while ((1u << s_.g_nb) < s_.g_asd) s_.g_nb++;
                if (s_.g_nb == 0) s_.g_nb = 1;
                s_.g_pos = 0; s_.g_rep = 0; s_.g_np = 0; s_.g_bitpos = 0;
                s_.phase = GEN_PILOT;
                return started();
            }
            case 0x20:
                if (!get(h, 2)) return bad();
                if (le(h, 2) == 0) {
                    stops_++;               // "stop the tape": played through (see Machine)
                    if (!start_pause(1000)) return bad();
                }
                else start_pause(le(h, 2));
                return started();
            case 0x2A:
                if (!skip(4)) return bad();
                stops_++;
                stops48_++;
                start_pause(1000);
                return started();
            case 0x2B:
                if (!get(h, 5)) return bad();
                level_ = h[4] ? 1 : 0;
                continue;
            case 0x23: {
                if (!get(h, 2)) return bad();
                int32_t rel = (int16_t) le(h, 2);
                if (rel == 0) continue;
                if (!goto_block((int32_t) at + rel)) return bad();
                continue;
            }
            case 0x24:
                if (!get(h, 2)) return bad();
                s_.loop_left = le(h, 2);
                s_.loop_start = f_->tell();
                s_.loop_block = block_;
                continue;
            case 0x25:
                if (s_.loop_left > 1) {
                    s_.loop_left--;
                    if (!f_->seek(s_.loop_start)) return bad();
                    block_ = s_.loop_block;
                }
                else s_.loop_left = 0;
                continue;
            case 0x26:
                if (!get(h, 2)) return bad();
                s_.call_n = le(h, 2);
                if (!skip(2 * s_.call_n)) return bad();
                s_.call_at = f_->tell();
                s_.call_block = at;
                s_.call_after = block_;
                s_.call_i = 0;
                s_.in_call = true;
                if (!next_call()) return bad();
                if (!s_.in_call) block_ = s_.call_after;
                continue;
            case 0x27:
                if (!s_.in_call) continue;
                if (!next_call()) return bad();
                if (!s_.in_call) block_ = s_.call_after;
                continue;
            default:
                if (!skip_body(id)) return bad();
                continue;
            }
        }
    }

    bool started() {
        s_.idle = 0;
        return true;
    }

    struct Saved {
        State s;
        uint32_t pos, block, stops, stops48, unsupported, data_started, read_in, loaded;
        bool in_data;
        uint8_t level, closing;
        TapeEnd end;
    };
    void save(Saved& v) const {
        v.s = s_; v.pos = f_->tell(); v.block = block_; v.stops = stops_; v.stops48 = stops48_; v.unsupported = unsupported_;
        v.data_started = data_started_; v.in_data = in_data_; v.read_in = read_in_; v.loaded = loaded_;
        v.level = level_; v.closing = closing_; v.end = end_;
    }
    void restore(const Saved& v) {
        s_ = v.s; block_ = v.block; stops_ = v.stops; stops48_ = v.stops48; unsupported_ = v.unsupported;
        data_started_ = v.data_started; in_data_ = v.in_data; read_in_ = v.read_in; loaded_ = v.loaded;
        level_ = v.level; closing_ = v.closing; end_ = v.end;
        if (!f_->seek(v.pos)) bad();
    }

    File* f_ = nullptr;
    bool tzx_ = false;
    State s_;
    uint8_t level_ = 0;
    TapeEnd end_ = TapeEnd::BAD;
    uint32_t block_ = 0;
    uint32_t stops_ = 0;
    uint32_t stops48_ = 0;
    uint32_t data_started_ = 0;     // data blocks begun (see data_position)
    bool in_data_ = false;          // the block playing is one of them
    uint32_t read_in_ = 0, loaded_ = 0;     // see loaded_position
    uint8_t closing_ = 0;
public:
    uint32_t unsupported_ = 0;      // blocks that make a signal but are not played (CSW Z-RLE)
};

// ---------------------------------------------------------------- the Spectrum

enum class Status : uint8_t {
    RUNNING,
    LOADED,             // the tape has ended and the game has run for a while (see suspect())
    TAPE_ERROR,         // the ROM said R Tape loading error
    BAD_TAPE,           // the file is damaged or has a block that is not known
    TOO_LONG,           // gave up: much longer than the tape
    STOPPED,            // the Spectrum stopped: HALT with interrupts off (a loader's "failed")
    BASIC_ERROR,        // back in BASIC with an error report (the program crashed or stopped)
    RESET,              // the program reset the Spectrum (often: the tape needs a 128K, or
                        // on a 128K: an old loader that only works on a 48K)
    SAVE,               // the program went into the ROM's SAVE ("Start tape, then press any
                        // key"): its BASIC loader went wrong (seen on a 128K with old tapes)
};

// Why a LOADED snapshot may not be the game (it is still saved)
enum class Suspect : uint8_t {
    NONE,
    STILL_READING,      // after the tape, the program kept reading the tape port in a small
                        // loop: a loader waiting for more (or a game waiting for a key)
    IN_BASIC,           // waiting in BASIC with no error (the program ended)
};

static const uint32_t FRAME_T = 69888;          // T-states in a 48K frame
static const uint32_t INT_T = 32;               // how long the interrupt is held
static const uint32_t FRAME_T_128 = 70908;      // the same for a 128K
static const uint32_t INT_T_128 = 36;

template <class File>
class Machine {
public:
    struct Stats {
        uint32_t frames;                        // emulated so far
        uint32_t tape_end_frame;                // when the tape ended (0: not yet)
        uint32_t busy_frames;                   // frames after it with many tape port reads in a small loop
        uint32_t ignored_frames;                // frames in a row of signal that nothing loaded
        uint32_t stores_last;                   // bytes stored in runs in the last frame
        uint64_t ops;                           // instructions run (what the time on the Pico goes by)
        uint64_t skipped;                       // loop passes skipped by the loader acceleration
        uint32_t flashed;                       // blocks taken in at once by flash_load
        uint32_t edge_waits;                    // edge waits done at once (edge_wait)
        uint32_t held;                          // frames the tape was held in a pause (hold_tape)
        uint32_t unstored_frames;               // frames in a row of data with nothing stored
        uint32_t early_frame;                   // when the tape was given up as no longer read (0: not)
        uint32_t ear_reads_last;                // port 0xFE reads in the last whole frame
        uint32_t ear_reads_max_after;           // most in a frame after the tape ended
        uint32_t error_code;                    // the ROM's error report, if one was seen (+1)
        uint16_t pc_low, pc_high;               // PC range in the last frame
    };

    // A 48K Spectrum. rom: 16384 bytes. ram: 49152 bytes, address 0x4000 first. after_ms:
    // how long the game runs after the tape ends before it is saved.
    bool begin(const uint8_t* rom, uint8_t* ram, File* tape, bool tzx, uint32_t after_ms = 2000) {
        model128_ = false;
        rom_ = rom;
        ram_ = ram;
        memset(ram_, 0, 49152);
        slot_[0] = const_cast<uint8_t*>(rom);
        slot_[1] = ram;
        slot_[2] = ram + 16384;
        slot_[3] = ram + 32768;
        frame_t_ = FRAME_T;
        int_t_ = INT_T;
        flash_rom_ = loader_is_standard(rom);
        return start(tape, tzx, after_ms);
    }

    // A 128K Spectrum. rom0: the 128K editor ROM, rom1: the 48K BASIC ROM (16384 bytes each).
    // banks: the eight RAM banks of 16384 bytes, bank 0 first (they can be anywhere, such as
    // in the layout of snapload's 128K buffer). The tape is started from the menu's Tape
    // Loader.
    bool begin128(const uint8_t* rom0, const uint8_t* rom1, uint8_t* const banks[8], File* tape,
                  bool tzx, uint32_t after_ms = 2000) {
        model128_ = true;
        rom128_[0] = rom0;
        rom128_[1] = rom1;
        for (int i = 0; i < 8; i++) {
            bank_[i] = banks[i];
            memset(bank_[i], 0, 16384);
        }
        rom_ = rom0;
        ram_ = bank_[5];
        locked_ = false;
        page(0);
        slot_[1] = bank_[5];
        slot_[2] = bank_[2];
        frame_t_ = FRAME_T_128;
        int_t_ = INT_T_128;
        memset(ay_, 0, sizeof(ay_));
        ay_sel_ = 0;
        flash_rom_ = loader_is_standard(rom1);
        return start(tape, tzx, after_ms);
    }

    bool is128() const { return model128_; }
    uint8_t port_7ffd() const { return p7ffd_; }
    const uint8_t* ay_registers() const { return ay_; }
    uint8_t ay_selected() const { return ay_sel_; }
    // The screen being shown (for a look at it on a PC)
    const uint8_t* screen() const { return model128_ ? bank_[(p7ffd_ & 8) ? 7 : 5] : ram_; }

private:
    bool start(File* tape, bool tzx, uint32_t after_ms) {
        memset(&st_, 0, sizeof(st_));
        ec_ok_ = false;
        after_frames_ = after_ms / 20;
        keys_step_ = 0;
        keys_frame_ = 0;
        playing_ = false;
        set_flags();
        t_ = 0;
        next_change_ = 0;
        ear_ = 0;
        border_ = 7;
        memset(rows_, 0xFF, sizeof(rows_));
        keys_ = false;
        scr_state_ = 0;
        scr_new_ = false;
        flashing_ = false;
        scr_pix_ = scr_attr_ = 0;
        status_ = Status::RUNNING;
        if (!tape_.begin(tape, tzx)) {
            status_ = Status::BAD_TAPE;
            return false;
        }
        memset(&cpu_, 0, sizeof(cpu_));
        cpu_.context = this;
        cpu_.fetch_opcode = &Machine::fetch_opcode;
        cpu_.fetch = &Machine::read;
        cpu_.read = &Machine::read;
        cpu_.write = &Machine::write;
        cpu_.in = &Machine::in;
        cpu_.out = &Machine::out;
        cpu_.nop = &Machine::read;
        cpu_.int_fetch = &Machine::read;
        cpu_.options = Z80_MODEL_ZILOG_NMOS;
        z80_power(&cpu_, true);
        return true;
    }

    // 128K paging: port 0x7FFD
    void page(uint8_t v) {
        if (locked_) return;
        ec_ok_ = false;
        p7ffd_ = v;
        slot_[3] = bank_[v & 7];
        slot_[0] = const_cast<uint8_t*>(rom128_[(v >> 4) & 1]);
        rom_ = rom128_[(v >> 4) & 1];
        if (v & 0x20) locked_ = true;
    }

    // The 48K BASIC ROM is the one in (always on a 48K): its addresses mean something
    bool basic_rom() const { return !model128_ || (p7ffd_ & 0x10); }

public:
    // Runs up to 'frames' frames (20 ms each of Spectrum time). Returns the status.
    // Comes back early (after the frame) when the loading screen has just been loaded: see
    // loading_screen().
    Status run(uint32_t frames) {
        while (frames-- > 0 && status_ == Status::RUNNING) {
            frame();
            if (scr_new_) break;
        }
        return status_;
    }

    // ---- the loading screen
    // The tape's loading screen is seen being loaded: stores of tape data that cover the
    // screen (6144 into the picture, 768 into the colours, while data is playing or a block
    // is flash-loaded), and then a store of tape data somewhere else: the loader has gone on
    // to the next thing. ("Cover": 5800 and 700 stores, as a loader may skip a few bytes.)
    // A screen that is all one colour is not counted (a CLS, or a loader
    // that clears it), and the count starts again. True once, just after that frame: the
    // screen is then the 6912 bytes at screen_data() (address 0x4000), as a .scr file.
    bool loading_screen() {
        bool n = scr_new_;
        scr_new_ = false;
        return n;
    }
    const uint8_t* screen_data() const { return ram_; }
    void no_screen_watch() { scr_state_ = 2; }

    // The snapshot: the 27 byte .sna header (registers, PC NOT pushed) and the PC. The RAM
    // is the 48K given to begin. Call only when run() has returned LOADED.
    void sna_header(uint8_t h[27], uint16_t& pc) {
        Z80& c = cpu_;
        auto p16 = [&](int at, uint16_t v) { h[at] = (uint8_t) v; h[at + 1] = (uint8_t) (v >> 8); };
        h[0] = c.i;
        p16(1, Z80_HL_(c)); p16(3, Z80_DE_(c)); p16(5, Z80_BC_(c)); p16(7, Z80_AF_(c));
        p16(9, Z80_HL(c)); p16(11, Z80_DE(c)); p16(13, Z80_BC(c));
        p16(15, Z80_IY(c)); p16(17, Z80_IX(c));
        h[19] = c.iff2 ? 0x04 : 0;
        h[20] = z80_r(&c);
        p16(21, Z80_AF(c));
        p16(23, Z80_SP(c));
        h[25] = c.im;
        h[26] = border_;
        pc = Z80_PC(c);
        if (c.halt_line) pc--;          // halted: start again on the HALT
    }

    // After LOADED: runs on (for testing on a PC: does the game keep going?). keys_row/bit
    // hold a key down (row 0xFF: none).
    void run_on(uint32_t frames, uint8_t row = 0xFF, uint8_t bit = 0) {
        memset(rows_, 0xFF, sizeof(rows_));
        keys_ = row < 8;
        if (row < 8) rows_[row] &= (uint8_t) ~(1 << bit);
        while (frames-- > 0) {
            z80_int(&cpu_, true);
            run_for(int_t_);
            z80_int(&cpu_, false);
            run_for(frame_t_ - int_t_);
        }
        memset(rows_, 0xFF, sizeof(rows_));
        keys_ = false;
    }
    const uint8_t* ram() const { return ram_; }
    // for tests on a PC: the registers and the time, to compare two runs
    uint64_t state_hash() const {
        uint64_t h = t_ * 1000003u;
        const Z80& c = cpu_;
        uint16_t r[] = { Z80_AF(c), Z80_BC(c), Z80_DE(c), Z80_HL(c), Z80_IX(c), Z80_IY(c), Z80_SP(c), Z80_PC(c), (uint16_t) z80_r(&c) };
        for (uint16_t v : r) h = h * 131 + v;
        return h;
    }
    void play_to_end() { early_ = false; }      // never stop before the end of the tape
    void no_acceleration() { accel_ = false; set_flags(); }    // run every pass of a loader's loops
    void no_flash() { flash_ = false; }         // play the ROM's blocks too, edge by edge
    void no_hold() { hold_ = false; }           // never hold the tape in a pause
    void test_switches(bool delays, bool learned) { delays_ = delays; learned_ = learned; set_flags(); }  // (for tests)
    void no_edge_waits() { edges_ = false; set_flags(); }   // (for tests) edge_wait off: the loop acceleration only

    const Stats& stats() const { return st_; }
    Suspect suspect() const { return suspect_; }
    const Player<File>& tape() const { return tape_; }
    uint32_t tstates() const { return (uint32_t) t_; }

private:
    // ---- typing LOAD "" (and ENTER) on the keyboard, a key held for 4 frames, then 4 up
    struct Key { uint8_t row, bit, row2, bit2; };
    static const Key* keys() {
        static const Key k[] = {
            { 6, 3, 0xFF, 0 },          // J: LOAD (the cursor is K)
            { 5, 0, 7, 1 },             // symbol shift P: "
            { 5, 0, 7, 1 },             // "
            { 6, 0, 0xFF, 0 },          // ENTER
        };
        return k;
    }
    static const uint32_t KEY_COUNT = 4;
    static const uint32_t BOOT_FRAMES = 160;    // the ROM's start up (about 1.5 s) and a margin
    static const uint32_t BOOT_FRAMES_128 = 200;    // to the 128K menu

    void keyboard_frame() {
        uint32_t f = st_.frames;
        if (model128_ && keys_step_ < KEY_COUNT * 2 - 2) {
            keys_step_ = KEY_COUNT * 2 - 2;     // the menu's first line, Tape Loader: only ENTER
        }
        if (f < (model128_ ? BOOT_FRAMES_128 : BOOT_FRAMES) || keys_step_ >= KEY_COUNT * 2) return;
        if (f - keys_frame_ < 4 && keys_frame_ != 0) return;
        keys_frame_ = f;
        memset(rows_, 0xFF, sizeof(rows_));
        keys_ = keys_step_ % 2 == 0;
        if (keys_step_ % 2 == 0) {
            const Key& k = keys()[keys_step_ / 2];
            rows_[k.row] &= (uint8_t) ~(1 << k.bit);
            if (k.row2 != 0xFF) rows_[k.row2] &= (uint8_t) ~(1 << k.bit2);
        }
        keys_step_++;
        if (keys_step_ == KEY_COUNT * 2) {
            playing_ = true;            // ENTER is let go: press PLAY
            set_flags();
            next_change_ = t_;
        }
    }

    // ---- one 20 ms frame, the interrupt at its start
    void frame() {
        keyboard_frame();
        reads_ = 0;
        seq_writes_ = 0;
        fresh_writes_ = 0;
        pc_lo_ = 0xFFFF;
        pc_hi_ = 0;
        z80_int(&cpu_, true);
        run_for(int_t_);
        z80_int(&cpu_, false);
        run_for(frame_t_ - int_t_);
        st_.frames++;
        st_.ops += ops_;
        ops_ = 0;
        st_.ear_reads_last = reads_;
        st_.stores_last = seq_writes_;
        st_.pc_low = pc_lo_;
        st_.pc_high = pc_hi_;
        if (status_ != Status::RUNNING) return;
        if (tape_.end() == TapeEnd::BAD) {
            status_ = Status::BAD_TAPE;
            return;
        }
        if (playing_ && tape_.end() == TapeEnd::ENDED) {
            // the tape plays on to the end with no one reading it: catch up now
            if (st_.tape_end_frame == 0) st_.tape_end_frame = st_.frames;
            if (reads_ > st_.ear_reads_max_after) st_.ear_reads_max_after = reads_;
            if (reads_ >= BUSY_READS && pc_hi_ >= pc_lo_ && (uint32_t) (pc_hi_ - pc_lo_) < BUSY_RANGE) st_.busy_frames++;
            if (st_.frames - st_.tape_end_frame >= after_frames_) {
                finish();
                judge();
            }
        }
        else if (playing_) {
            hold_tape();
            advance_tape(t_);           // so the end of the tape is seen even if not read
            // Data is playing but nothing is loading it: the tape port is hardly read, or read
            // in a loop too small to be taking the bits in, such as a game's "press SPACE". After IGNORED_FRAMES of that the load is over:
            // what is left on the tape is for later (a multi-load game's next level).
            uint8_t now = tape_.playing_now();
            bool small = pc_hi_ >= pc_lo_ && (uint32_t) (pc_hi_ - pc_lo_) < TINY_LOOP;
            // Only data bits count: tones and pulses can be meant to be ignored (Speedlock 4
            // plays 14 s of clicks while its loader decrypts itself).
            // Or: data is playing, the port is read, but no bytes are being stored one after
            // another, as every loader does with what it takes in (a game waiting for a key
            // with the tape still running). Not while the ROM's loader runs: it reads a block
            // with the wrong flag without storing it. This one waits 10 s.
            bool rom_loader = basic_rom() && pc_lo_ <= 0x0605 && pc_hi_ >= 0x04C2;
            // A loop of a loader's size that stores bytes in new places is loading too, a
            // byte at a time all over (720 Degrees). Not a game running (its code is all
            // over, and what it stores proves nothing).
            bool scattered = pc_hi_ >= pc_lo_ && (uint32_t) (pc_hi_ - pc_lo_) < BUSY_RANGE &&
                             fresh_writes_ >= FEW_STORES;
            // a frame of loading, for loaded_position: the ROM's loader (only: BASIC's PAUSE and
            // the interrupt routine span its addresses too), or a loop of a
            // loader's size reading the port and storing (a game waiting for a key runs its
            // interrupt routine and more, all over the memory)
            bool compact = pc_hi_ >= pc_lo_ && (uint32_t) (pc_hi_ - pc_lo_) < BUSY_RANGE;
            bool in_ld_bytes = basic_rom() && pc_lo_ >= 0x04C2 && pc_hi_ <= 0x0605 && pc_hi_ >= pc_lo_;
            if (in_ld_bytes || (compact && reads_ >= FEW_READS &&
                               (seq_writes_ >= FEW_STORES || fresh_writes_ >= FEW_STORES))) {
                tape_.note_loading();
            }
            if (now == 2 && seq_writes_ < FEW_STORES && !rom_loader && !scattered) {
                if (++st_.unstored_frames >= UNSTORED_FRAMES && early_) {
                    st_.early_frame = st_.frames;
                    st_.tape_end_frame = st_.frames;
                    finish();
                    judge();
                    return;
                }
            }
            else if (now == 2) {
                st_.unstored_frames = 0;
            }
            if (now == 2 && (reads_ < FEW_READS || small)) {
                if (++st_.ignored_frames >= IGNORED_FRAMES && early_) {
                    st_.early_frame = st_.frames;
                    st_.tape_end_frame = st_.frames;
                    finish();
                    judge();
                }
            }
            else if (now == 2) {
                st_.ignored_frames = 0;
            }
        }
        if (st_.frames > 30 * 60 * 50) status_ = Status::TOO_LONG;   // 30 minutes
    }

    void run_for(uint32_t t) {
        // z80_run may go past t by an instruction: that is taken off the next run
        int32_t want = (int32_t) t - overrun_;
        if (want <= 0) {
            overrun_ = -want;
            return;
        }
        run_base_ = t_;
        uint32_t done = (uint32_t) z80_run(&cpu_, (zusize) want);
        t_ += done;
        overrun_ = (int32_t) done - want;
    }

    static const uint32_t FEW_READS = 50;       // a loader reads the port 700+ times a frame
    static const uint32_t HOLD_FRAMES = 500;    // 10 s: the longest a pause is held
    static const uint32_t TINY_LOOP = 16;       // bytes: a loop reading the port and nothing else
    static const uint32_t IGNORED_FRAMES = 250; // 5 s of signal not loaded
    static const uint32_t FEW_STORES = 1;       // a loader stores 2+ bytes a frame (3 at ROM speed)...
    static const uint32_t UNSTORED_FRAMES = 500;    // ...so 10 s of data with none stored: not loading
    static const uint32_t BUSY_READS = 500;     // a loader waiting reads the port 1000+ times a frame
    static const uint32_t BUSY_RANGE = 512;

    // At the end: the game, or a Spectrum that is stuck
    void judge() {
        suspect_ = Suspect::NONE;
        uint16_t pc = Z80_PC(cpu_);
        if (cpu_.halt_line && !cpu_.iff1) {
            status_ = Status::STOPPED;
            return;
        }
        if (cpu_.halt_line && pc == 0x1304 && basic_rom()) {   // MAIN-4 waiting for a key after a report
            if (st_.error_code != 0) {
                status_ = Status::BASIC_ERROR;
                return;
            }
            suspect_ = Suspect::IN_BASIC;
        }
        else if (st_.busy_frames * 4 >= after_frames_ * 3) {
            suspect_ = Suspect::STILL_READING;
        }
        status_ = Status::LOADED;
    }

    // Leaves the CPU at a whole instruction (after a DD or FD prefix the emulator can stop)
    void finish() {
        // (a CPU in HALT is fine as it is: the snapshot starts it on the HALT again)
        while (cpu_.resume != 0 && !cpu_.halt_line) {
            run_base_ = t_;
            t_ += z80_run(&cpu_, 1);
        }
    }

    // A pause on the tape is held while nothing reads the tape port, as an emulator's tape
    // stops when no loader is running: a frame without a read doesn't count towards it.
    // Some tape files have the pauses of the real tape cut short, and a loader that spends
    // longer between blocks (720 Degrees decrypts itself and draws for 6 s, in a pause of
    // 1.8 s) would miss the next block's pilot. Only in a silence with a block after it (not
    // at the end of the tape), and at most HOLD_FRAMES for one pause (a game that has loaded
    // and runs while the tape plays on to a next level's blocks).
    void hold_tape() {
        if (!hold_ || reads_ != 0 || !tape_.silent()) {
            if (reads_ != 0) hold_frames_ = 0;
            return;
        }
        if (hold_block_ != tape_.blocks()) {
            hold_block_ = tape_.blocks();
            hold_ok_ = tape_.silence_before_signal();
            hold_frames_ = 0;
        }
        if (!hold_ok_ || hold_frames_ >= HOLD_FRAMES) return;
        hold_frames_++;
        st_.held++;
        next_change_ += frame_t_;
    }

    void advance_tape(uint64_t now) {
        while (now >= next_change_) {
            uint32_t dur;
            uint8_t level;
            if (!tape_.next(dur, level)) {
                next_change_ = ~(uint64_t) 0;
                ear_ = 0;
                return;
            }
            ear_ = level;
            next_change_ += dur;
        }
    }

    // ---- loader acceleration
    // A loader waits for the next edge of the signal in a loop that reads the tape port, with
    // nothing changing from one pass to the next but B (a count, up or down by one), R, the
    // flags of that count and the time, and nothing written. Once the same read has been
    // seen to go round like that 3 times with the same steps, the loop is moved on to a
    // couple of passes before the next edge: B, R, F and the time are set to what those
    // passes would have left. The loop then sees the edge on the very pass it would have, so
    // the result is the same as running every pass (checked: the snapshots are the same). Not
    // with interrupts on (they would be missed), not across the end of the run.
    struct Regs {
        uint16_t pc, port, c, de, hl, ix, iy, sp, af_, bc_, de_, hl_;
        uint8_t i;
        bool operator==(const Regs& o) const {
            return pc == o.pc && port == o.port && c == o.c && de == o.de && hl == o.hl && ix == o.ix &&
                iy == o.iy && sp == o.sp && af_ == o.af_ && bc_ == o.bc_ && de_ == o.de_ && hl_ == o.hl_ && i == o.i;
        }
    };
    struct Accel {
        Regs regs;
        uint64_t t, edge;               // the time of the read, the tape's next change then
        uint32_t writes, dt;
        uint8_t b, r, f, dr, count;
        int8_t db;
        bool fconst;                    // F has been the same on every pass (DJNZ)
        bool fcount;                    // F has been what INC/DEC B leaves on every pass
    };

    // Loops proved so far (by their read's address and steps)
    struct Known { uint16_t pc; uint16_t dt; int8_t db; uint8_t dr; bool fcount; };
    Known known_[4] = {};
    uint8_t known_next_ = 0;
    bool known(uint16_t pc, uint32_t dt, int8_t db, uint8_t dr, bool fcount) const {
        for (const Known& k : known_) {
            if (k.dt != 0 && k.pc == pc && k.dt == dt && k.db == db && k.dr == dr && k.fcount == fcount) return true;
        }
        return false;
    }
    void learn(uint16_t pc, uint32_t dt, int8_t db, uint8_t dr, bool fcount) {
        if (known(pc, dt, db, dr, fcount) || dt > 0xFFFF) return;
        known_[known_next_] = Known{ pc, (uint16_t) dt, db, dr, fcount };
        known_next_ = (uint8_t) ((known_next_ + 1) & 3);
    }

    // ---- delay loops, run at once: DEC A / JR NZ to itself and DJNZ to itself (the ROM's and
    // its copies' LD-DELAY and LD-WAIT). Seen at their first instruction; all but the last pass
    // are done here (registers, flags, R, MEMPTR and time as they would be), and the CPU runs
    // the last one. Within the run only, so no interrupt is missed.
    TZXCONV_NOINLINE void delay_loop(uint16_t pc) {
        Z80& c = cpu_;
        uint8_t b0 = peek(pc), b1 = peek((uint16_t) (pc + 1)), b2 = peek((uint16_t) (pc + 2));
        uint64_t room = c.cycle_limit > c.cycles + 64 ? (c.cycle_limit - c.cycles - 64) : 0;
        if (b0 == 0x3D && b1 == 0x20 && b2 == 0xFD) {                   // DEC A; JR NZ,$-1
            uint32_t a = Z80_A(c) ? Z80_A(c) : 256;
            uint64_t k = a - 1;                                         // passes before the last
            if (k > room / 16) k = room / 16;
            if (k < 2) return;
            uint8_t na = (uint8_t) (a - k);
            Z80_A(c) = na;
            // DEC A of the last pass done: the flags of a result na (C kept)
            Z80_F(c) = (uint8_t) ((Z80_F(c) & 0x01) | 0x02 | (na & 0xA8) | (na == 0 ? 0x40 : 0) |
                                  ((na & 0x0F) == 0x0F ? 0x10 : 0) | (na == 0x7F ? 0x04 : 0));
            c.r = (uint8_t) (c.r + 2 * k);
            c.cycles += (zusize) (16 * k);
            Z80_MEMPTR(c) = pc;
            c.q = 0;
            st_.skipped += k;
        } else if (b0 == 0x10 && b1 == 0xFE) {                          // DJNZ $
            uint32_t b = Z80_B(c) ? Z80_B(c) : 256;
            uint64_t k = b - 1;
            if (k > room / 13) k = room / 13;
            if (k < 2) return;
            Z80_B(c) = (uint8_t) (b - k);
            c.r = (uint8_t) (c.r + k);
            c.cycles += (zusize) (13 * k);
            Z80_MEMPTR(c) = pc;
            c.q = 0;
            st_.skipped += k;
        }
    }

    // ---- edge waits: LD-EDGE and its copies, recognised by their code
    // Nearly every loader (the ROM's, Speedlock 1 to 7, Power-Load, SoftLock, the Erbe and
    // Ocean loaders seen so far) waits for an edge with the ROM's own loop, and most then go on
    // with the ROM's own ending:
    //     L: INC B / RET Z / LD A,hi / IN A,(lo) / RRA / [RET NC or NOP] / XOR C / AND $20 / JR Z,L
    //        LD A,C / CPL / LD C,A / AND m / OR o / OUT (p),A / SCF / RET
    // (B counts the passes, RET Z gives up when it wraps; the edge is the level in bit 6 of the
    // port differing from bit 5 of C; the ending turns C round for the next edge and sets the
    // border.) Seen at its INC B, the tape tells which pass will see the edge, so the passes
    // before it are done at once: they leave B (+1 each), R (an M1 per instruction), the time
    // (dt each), and after one or more A = 0, F = 0x54 (AND $20 of 0) and MEMPTR = L (the JR).
    // With the ending recognised too, the pass that sees the edge and the ending are done as
    // well, instruction by instruction (registers, flags, MEMPTR, R, the time, the border), up
    // to the RET; or, if B wraps first, the INC B and RET Z of that pass. The CPU goes on at the
    // return address. Without the ending the CPU runs the pass that sees the edge itself. The
    // result is the same as running every pass (checked: the snapshots are the same byte for
    // byte). Only with interrupts off and within the run (as the loop acceleration), and not
    // if a key would stop the loop (RET NC: SPACE down).
    // True: the routine is done, and the CPU goes on at its (new) PC.
    static uint8_t szp_flags(uint8_t a) {
        return (uint8_t) ((a & 0xA8) | (a == 0 ? 0x40 : 0) | (__builtin_parity(a) ? 0 : 0x04));
    }

    // The routine last recognised (its code is read once, then kept until a store into it, or
    // paging, could have changed it)
    struct EdgeCode {
        uint16_t pc, end;                   // the INC B, and the ending after the JR Z
        uint16_t port;
        uint8_t dt, m1, xt;                 // a pass: T-states, M1 cycles; the RET NC or NOP's T
        uint8_t and_m, or_o, out_p;         // the ending's AND, OR and OUT
        bool retnc, ending, no_border;
    };
    EdgeCode ec_ = {};
    bool ec_ok_ = false;

    bool edge_code(uint16_t pc) {
        // the loop's code
        if (peek((uint16_t) (pc + 1)) != 0xC8 || peek((uint16_t) (pc + 2)) != 0x3E || peek((uint16_t) (pc + 4)) != 0xDB ||
            peek((uint16_t) (pc + 6)) != 0x1F) return false;
        uint8_t hi = peek((uint16_t) (pc + 3)), lo = peek((uint16_t) (pc + 5));
        if (lo & 1) return false;                                   // the ULA's port: even
        EdgeCode e = {};
        uint8_t extra = peek((uint16_t) (pc + 7));
        uint16_t at = (uint16_t) (pc + 7);
        e.dt = 54;
        e.m1 = 8;
        e.retnc = extra == 0xD0;
        if (extra == 0xD0 || extra == 0x00) {                       // RET NC (5 T) or NOP (4 T)
            at++;
            e.xt = e.retnc ? 5 : 4;
            e.dt = (uint8_t) (e.dt + e.xt);
            e.m1++;
        }
        if (peek(at) != 0xA9 || peek((uint16_t) (at + 1)) != 0xE6 || peek((uint16_t) (at + 2)) != 0x20 ||
            peek((uint16_t) (at + 3)) != 0x28 || (uint16_t) (at + 5 + (int8_t) peek((uint16_t) (at + 4))) != pc) return false;
        e.pc = pc;
        e.port = (uint16_t) ((hi << 8) | lo);
        // the ending: LD A,C / CPL / LD C,A / AND m / OR o / OUT (p),A (or NOP / NOP: no border,
        // as New Generation's loader) / SCF / RET
        const uint16_t end = (uint16_t) (at + 5);
        e.end = end;
        e.no_border = peek((uint16_t) (end + 7)) == 0x00 && peek((uint16_t) (end + 8)) == 0x00;
        e.ending = peek(end) == 0x79 && peek((uint16_t) (end + 1)) == 0x2F && peek((uint16_t) (end + 2)) == 0x4F &&
            peek((uint16_t) (end + 3)) == 0xE6 && peek((uint16_t) (end + 5)) == 0xF6 &&
            (peek((uint16_t) (end + 7)) == 0xD3 || e.no_border) &&
            peek((uint16_t) (end + 9)) == 0x37 && peek((uint16_t) (end + 10)) == 0xC9;
        e.and_m = peek((uint16_t) (end + 4));
        e.or_o = peek((uint16_t) (end + 6));
        e.out_p = peek((uint16_t) (end + 8));
        ec_ = e;
        ec_ok_ = true;
        return true;
    }

    TZXCONV_NOINLINE bool edge_wait(uint16_t pc) {
        Z80& c = cpu_;
        if (c.iff1) return false;
        if (!(ec_ok_ && ec_.pc == pc) && !edge_code(pc)) return false;
        const EdgeCode& e = ec_;
        const uint32_t dt = e.dt, m1 = e.m1, xt = e.xt;
        const bool retnc = e.retnc, ending = e.ending, no_border = e.no_border;
        const uint16_t end = e.end, port = e.port;
        if (retnc && !(port_value(port) & 1)) return false;         // SPACE down: the loop would stop
        // passes that can be done: B not to wrap (that pass ends the loop), and the run's end
        // not passed, with room for the last pass and the ending (105 T)
        const uint8_t b = Z80_B(c);
        const uint32_t kwrap = 255u - b;
        uint32_t kmax = kwrap;
        zusize left = c.cycle_limit > c.cycles + 64 + 110 ? c.cycle_limit - c.cycles - 64 - 110 : 0;
        if (c.cycle_limit <= c.cycles + 64 + 110) return false;
        if (kmax * dt > left) kmax = (uint32_t) (left / dt);
        // the read of pass j is at the pass's start + 23 (INC B, RET Z, LD A,n, then 7 T into
        // the IN), and sees the level at that time + 2 (as in())
        const uint8_t level = (uint8_t) ((Z80_C(c) >> 5) & 1);
        const uint64_t start = run_base_ + c.cycles;
        // times from here on are counted from start, in 32 bits (all within kmax passes)
        auto rel = [&](uint64_t t) -> uint32_t {
            return t <= start ? 0 : t - start > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t) (t - start);
        };
#ifdef TZXCONV_EDGE_CHECK
        typename Player<File>::Saved saved_tape;
        tape_.save(saved_tape);
        const uint8_t saved_ear = ear_;
        const uint64_t saved_change = next_change_;
#endif
        uint32_t k = 0, s = 25;                                     // pass k's read (from start)
        uint32_t change = rel(next_change_);
        bool edge = false;
        while (k < kmax) {
            if (s >= change) {
                advance_tape(start + s);
                change = rel(next_change_);
            }
            if (ear_ != level) {                                    // pass k sees the edge
                edge = true;
                break;
            }
            tape_.note_read();                                      // (each read notes the block playing)
            // the level stays as it is until next_change_: the passes up to it see no edge
            uint32_t more = change > s ? (change - s + dt - 1) / dt : 1;
            if (more > kmax - k) more = kmax - k;
            k += more;
            s += more * dt;
        }
#ifdef TZXCONV_EDGE_CHECK
        tape_.restore(saved_tape);
        ear_ = saved_ear;
        next_change_ = saved_change;
        TZXCONV_EDGE_CHECK(st_.frames, pc, end, b, k, kmax, start, next_change_, ear_, level);
        return false;
#endif
        // The tape may now be past the read of pass k (at the edge): the loop acceleration
        // must not take that read as one more pass of a loop it knows (it would jump on to
        // the next edge), so what it has seen is forgotten.
        acc_.count = 0;
        acc_.regs.pc = 0xFFFF;
        // the passes that see no edge
        uint8_t nb = (uint8_t) (b + k);
        uint8_t a = Z80_A(c), f = Z80_F(c);
        if (k > 0) {
            a = 0;
            f = 0x54;
            reads_ += k;
            st_.skipped += k;
        }
        const bool wrap = !edge && k == kwrap;
        if (!(edge && ending) && !wrap) {
            if (k == 0) return false;
            Z80_B(c) = nb;
            c.r = (uint8_t) (c.r + k * m1);
            c.cycles += (zusize) (k * dt);
            Z80_A(c) = a;
            Z80_F(c) = f;
            Z80_MEMPTR(c) = pc;
            st_.edge_waits++;
            return false;                                           // the CPU runs on from pass k
        }
        uint32_t t = k * dt;
        uint32_t r = k * m1;
        uint16_t sp = Z80_SP(c);
        uint16_t ret = (uint16_t) (peek(sp) | (peek((uint16_t) (sp + 1)) << 8));
        // INC B (the carry stays)
        nb++;
        f = (uint8_t) ((f & 0x01) | (nb & 0xA8) | (nb == 0 ? 0x40 : 0) | ((nb & 0x0F) == 0 ? 0x10 : 0) | (nb == 0x80 ? 0x04 : 0));
        if (wrap) {
            // B has wrapped: RET Z is taken (no edge in time)
            t += 4 + 11;
            r += 2;
        } else {
            // the rest of the pass that sees the edge: RET Z (not taken), LD A,hi, IN A,(lo) (its
            // read was taken above), RRA, [RET NC or NOP], XOR C, AND $20, JR Z (not taken)
            uint8_t v = port_value(port);
            reads_++;
            tape_.note_read();
            uint8_t cin = f & 0x01;
            a = (uint8_t) ((v >> 1) | (cin << 7));
            f = (uint8_t) ((f & 0xC4) | (a & 0x28) | (v & 0x01));
            a ^= Z80_C(c);
            f = szp_flags(a);
            a &= 0x20;
            f = (uint8_t) (szp_flags(a) | 0x10);
            t += 4 + 5 + 7 + 11 + 4 + xt + 4 + 7 + 7;
            r += m1;
            // the ending: LD A,C / CPL / LD C,A / AND m / OR o / OUT (p),A / SCF / RET
            a = (uint8_t) ~Z80_C(c);
            f = (uint8_t) ((f & 0xC5) | (a & 0x28) | 0x12);
            Z80_C(c) = a;
            a &= e.and_m;
            f = (uint8_t) (szp_flags(a) | 0x10);
            a |= e.or_o;
            f = szp_flags(a);
            if (no_border) {
                t += 4 + 4;                                         // NOP / NOP
                r += 2;
            } else {
                out(this, (uint16_t) ((a << 8) | e.out_p), a);            // (the border; out() doesn't use the time)
                t += 11;
                r += 1;
            }
            f = (uint8_t) ((f & 0xC4) | (a & 0x28) | 0x01);
            t += 4 + 4 + 4 + 7 + 7 + 4 + 10;
            r += 7;
            if (pc_hi_ < (uint16_t) (end + 10)) pc_hi_ = (uint16_t) (end + 10);
        }
        // RET
        Z80_B(c) = nb;
        Z80_A(c) = a;
        Z80_F(c) = f;
        Z80_SP(c) = (uint16_t) (sp + 2);
        Z80_PC(c) = ret;
        Z80_MEMPTR(c) = ret;
        c.r = (uint8_t) (c.r + r);
        c.cycles += (zusize) t;
        st_.edge_waits++;
        return true;
    }

    // What a read of the port gives (as in(), for a port with A0 low, without its side effects)
    uint8_t port_value(uint16_t port) const {
        uint8_t v = 0x1F;
        if (keys_) {
            uint8_t high = (uint8_t) (port >> 8);
            for (int r = 0; r < 8; r++) {
                if (!(high & (1 << r))) v &= rows_[r];
            }
        }
        return (uint8_t) (v | 0xA0 | (ear_ ? 0x40 : 0));
    }

    // The flags INC B (db 1) or DEC B (db -1) leave for the result b, with the carry of f
    static uint8_t count_flags(uint8_t f, uint8_t b, int8_t db) {
        uint8_t r = (uint8_t) ((f & 0x01) | (b & 0xA8) | (b == 0 ? 0x40 : 0));
        if (db > 0) r |= (uint8_t) (((b & 0x0F) == 0 ? 0x10 : 0) | (b == 0x80 ? 0x04 : 0));
        else r |= (uint8_t) (0x02 | ((b & 0x0F) == 0x0F ? 0x10 : 0) | (b == 0x7F ? 0x04 : 0));
        return r;
    }

    // At a read of the tape port (now: its time). Gives the T-states jumped (0: none).
    // Called on every read, so the cheap tests come first; the other registers are only
    // compared once those pass (but always kept, for the next read).
    uint32_t accelerate(uint16_t port, uint64_t now) {
        Z80& c = cpu_;
        Accel& a = acc_;
        Regs& o = a.regs;
        uint8_t b = Z80_B(c), r = c.r, f = Z80_F(c);
        uint16_t pc = Z80_PC(c);
        uint32_t dt = now - a.t < 2000 ? (uint32_t) (now - a.t) : 2000;
        int8_t db = (int8_t) (uint8_t) (b - a.b);
        uint8_t dr = (uint8_t) (r - a.r);
        // B must count (up or down by one): that is what ends a loader's wait if no edge
        // comes, and what bounds the jump. A read that comes round with B the same each time
        // can be separate calls that only look like one loop (seen in the ROM's loader).
        bool same = pc == o.pc && port == o.port && dt < 2000 && now > a.t && (db == 1 || db == -1) &&
                    writes_ == a.writes && a.edge == next_change_ && !c.iff1 &&
                    Z80_C(c) == o.c && Z80_DE(c) == o.de && Z80_HL(c) == o.hl && Z80_IX(c) == o.ix &&
                    Z80_IY(c) == o.iy && Z80_SP(c) == o.sp && Z80_AF_(c) == o.af_ && Z80_BC_(c) == o.bc_ &&
                    Z80_DE_(c) == o.de_ && Z80_HL_(c) == o.hl_ && c.i == o.i;
        if (!same) {
            o.pc = pc; o.port = port; o.c = Z80_C(c); o.de = Z80_DE(c); o.hl = Z80_HL(c); o.ix = Z80_IX(c);
            o.iy = Z80_IY(c); o.sp = Z80_SP(c); o.af_ = Z80_AF_(c); o.bc_ = Z80_BC_(c); o.de_ = Z80_DE_(c);
            o.hl_ = Z80_HL_(c); o.i = c.i;
            a.writes = writes_; a.edge = next_change_;
        }
        bool steady = same && a.count > 0 && dt == a.dt && db == a.db && dr == a.dr;
        // Which kind of loop: F left alone (as DJNZ does) or the flags of INC/DEC B. Some runs
        // of B give the same flags, so it is only decided when the passes seen fit one only.
        bool fconst = f == a.f;
        bool fcount = db != 0 && f == count_flags(a.f, b, db);
        if (steady) {
            a.fconst = a.fconst && fconst;
            a.fcount = a.fcount && fcount;
        } else {
            a.fconst = fconst;
            a.fcount = fcount;
        }
        bool fok = a.fconst != a.fcount;
        a.count = same ? (steady && (a.fconst || a.fcount) ? (uint8_t) (a.count < 9 ? a.count + 1 : 9) : 1) : 0;
        a.t = now; a.b = b; a.r = r; a.f = f; a.dt = dt; a.db = db; a.dr = dr;
        // A loop already proved at this read (the same steps, the same kind of flags) is trusted
        // again after one pass that matches it; a new one needs three
        if (learned_ && fok && a.count >= 1 && a.count < 3 && known(pc, dt, db, dr, a.fcount)) a.count = 3;
        if (a.count < 3 || !fok) return 0;
        learn(pc, dt, db, dr, a.fcount);
        // passes whose read (2 T-states into the IN's port cycle, as in()) still comes before the
        // next change of the signal, less one to be safe. B must not reach 0 (that ends the
        // loop): at most up to 254, or down to 2. So never more than 252 passes, which keeps
        // the sums in 32 bits.
        uint32_t bmax = db > 0 ? (b < 254 ? (uint32_t) (254 - b) : 0) : (b > 2 ? (uint32_t) (b - 2) : 0);
        uint64_t gap64 = next_change_ > now + 3 ? next_change_ - now - 3 : 0;
        uint32_t gap = gap64 < 256u * 2000u ? (uint32_t) gap64 : 256u * 2000u;
        uint32_t n = gap / dt;
        n = n > 1 ? n - 1 : 0;
        if (n > bmax) n = bmax;
        // and within this run (the frame's interrupt comes at its end)
        zusize left = c.cycle_limit > c.cycles + 64 ? c.cycle_limit - c.cycles - 64 : 0;
        if ((uint64_t) n * dt > left) n = (uint32_t) (left / dt);
        if (n < 4) return 0;
#ifdef TZXCONV_DEBUG_ACCEL
        TZXCONV_DEBUG_ACCEL(st_.frames, pc, b, n, dt, db, dr, now, next_change_, f, a.fconst);
#endif
#ifdef TZXCONV_VERIFY
        // test build: say what the jump would give, run every pass, and compare when it gets there
        TZXCONV_VERIFY(now + n * dt, (uint8_t) (b + (int) n * db), (uint8_t) ((r + n * dr) & 0x7F),
                       a.fcount ? count_flags(f, (uint8_t) (b + (int) n * db), db) : f, now, next_change_, b, n, ear_);
        return 0;
#endif
        uint32_t jump = n * dt;
        c.cycles += (zusize) jump;
        uint8_t nb = (uint8_t) (b + (int) n * db);
        Z80_B(c) = nb;
        c.r = (uint8_t) (r + n * dr);
        if (a.fcount) Z80_F(c) = count_flags(f, nb, db);
        reads_ += n;
        st_.skipped += n;
        a.t = now + jump; a.b = nb; a.r = c.r; a.f = Z80_F(c);
        return jump;
    }

    // ---- flash loading
    // The ROM's LD-BYTES (0x0556-0x0604) is the same in every 48K BASIC ROM: checked by a hash
    static bool loader_is_standard(const uint8_t* rom) {
        uint32_t h = 0x811C9DC5u;
        for (uint32_t a = 0x0556; a < 0x0605; a++) h = (h ^ rom[a]) * 0x01000193u;
        return h == 0x3128CFE0u;
    }

    // At LD-START (0x056C, which the ROM's loader comes back to while it waits for a signal),
    // when the next thing it would find is a standard speed block that it would load: the
    // block is loaded at once, as Fuse does. The registers are left as the ROM leaves them
    // (B, C, DE, H, L, IX, A' and F'; A and F are made by the CPU, which goes on at 0x05DF:
    // LD A,H / CP 1 / RET) and the tape goes on at the block's pause. Not done (the tape is
    // played to the ROM as it is): a VERIFY, a block with the wrong flag or too short for
    // the bytes asked for, no bytes asked for, or a block that is not at standard speed.
    // The time the load would have taken is not counted: the snapshot is not the same as
    // with every edge played, but the game is.
    TZXCONV_NOINLINE bool flash_load() {
        Z80& c = cpu_;
        uint16_t de = Z80_DE(c);
        if (!(Z80_F_(c) & 0x01) || de == 0) return false;
        uint32_t len;
        if (!tape_.flash_ready(Z80_A_(c), de + 2u, len)) return false;
        uint8_t level = tape_.flash_level();
        // the flag byte, the de bytes stored, and the parity byte after them
        uint8_t buf[32];
        uint8_t parity = 0, last = 0;
        uint16_t ix = Z80_IX(c);
        uint32_t todo = de + 2u, at = 0;
        while (at < todo) {
            uint32_t n = todo - at < sizeof(buf) ? todo - at : (uint32_t) sizeof(buf);
            if (!tape_.flash_read(buf, n)) return false;        // a damaged file: seen at the frame's end
            for (uint32_t i = 0; i < n; i++, at++) {
                uint8_t b = buf[i];
                parity ^= b;
                last = b;
                if (at >= 1 && at <= de) {
                    flashing_ = true;
                    write(this, ix++, b);
                    flashing_ = false;
                }
            }
        }
        tape_.flash_done();
        // C: bit 5 follows the level, every edge inverts it, after the sync the colour bits are
        // changed (XOR 3), and LD-FLAG leaves bit 7 clear (data edges come in pairs)
        uint8_t cc = Z80_C(c);
        if (((cc >> 5) & 1) != level) cc = (uint8_t) ~cc;
        cc = (uint8_t) ((cc ^ 0x03) & 0x7F);
        Z80_C(c) = cc;
        Z80_B(c) = 0xB0;
        Z80_DE(c) = 0;
        Z80_IX(c) = ix;
        Z80_H(c) = parity;
        Z80_L(c) = last;
        Z80_AF_(c) = (uint16_t) ((cc << 8) | 0x45 | (cc & 0x28));
        Z80_PC(c) = 0x05DF;
        uint64_t now = run_base_ + c.cycles;
        ear_ = level;
        next_change_ = now;
        advance_tape(now);
        st_.flashed++;
        return true;
    }

    // ---- the bus
    static Machine* self(void* c) { return static_cast<Machine*>(c); }

    // Every instruction comes through here, so it is kept short (it was a third of the time on
    // the Pico): the few addresses that need a look go to trap()
    static zuint8 fetch_opcode(void* c, zuint16 a) {
        Machine* m = self(c);
#ifdef TZXCONV_DEBUG_PC
        TZXCONV_DEBUG_PC(m->st_.frames, a);
#endif
        uint8_t op = m->slot_[a >> 14][a & 0x3FFF];
        m->ops_++;
        if (a < m->pc_lo_) m->pc_lo_ = a;
        if (a > m->pc_hi_) m->pc_hi_ = a;
        if (op == 0x04 && m->edge_on_ && m->edge_wait(a)) {
            // LD-EDGE done up to its RET: this fetch is of the instruction at the return address
            a = Z80_PC(m->cpu_);
            op = m->slot_[a >> 14][a & 0x3FFF];
            if (a < m->pc_lo_) m->pc_lo_ = a;
            if (a > m->pc_hi_) m->pc_hi_ = a;
        }
        if ((op == 0x3D || op == 0x10) && m->delay_on_) m->delay_loop(a);
        if ((a & 0xFFF7) == 0 || a == 0x056C || a == 0x0970) return m->trap(a, op);
        return op;
    }

    // 0x0000 (a reset), 0x0008 (RST 8: an error report), 0x056C (LD-START: flash loading) and
    // 0x0970 (SA-CONTRL: SAVE). Gives the opcode to run.
    TZXCONV_NOINLINE uint8_t trap(uint16_t a, uint8_t op) {
        if (status_ != Status::RUNNING) return op;
        if (a == 0x0000) {
            // a jump to 0 after the tape started: the Spectrum was reset. A 128K-only loader
            // does this on a 48K machine.
            if (playing_) {
                status_ = Status::RESET;
                z80_break(&cpu_);
            }
        }
        else if (!basic_rom()) {
        }
        else if (a == 0x0970) {
            if (playing_) {
                status_ = Status::SAVE;         // SA-CONTRL, the start of SAVE
                z80_break(&cpu_);
            }
        }
        else if (a == 0x0008) {
            // RST 8, the ROM's error report: the code is the byte after the RST
            uint16_t sp = Z80_SP(cpu_);
            uint16_t ret = (uint16_t) (peek(sp) | (peek((uint16_t) (sp + 1)) << 8));
            uint8_t code = peek(ret);
            st_.error_code = code + 1u;
            if (code == 0x1A) {
                status_ = Status::TAPE_ERROR;
                z80_break(&cpu_);
            }
        }
        else if (flash_ && flash_rom_ && playing_ && flash_load()) {
            return peek(Z80_PC(cpu_));          // 0x056C: goes on at 0x05DF
        }
        return op;
    }

    uint8_t peek(uint16_t a) const { return slot_[a >> 14][a & 0x3FFF]; }

    static zuint8 read(void* c, zuint16 a) { return self(c)->peek(a); }

    static void write(void* c, zuint16 a, zuint8 v) {
        Machine* m = self(c);
        m->writes_++;
        if ((uint16_t) (a - m->ec_.pc) <= 24) m->ec_ok_ = false;    // the edge routine's code may change
        if (a >= 0x4000) m->slot_[a >> 14][a & 0x3FFF] = v;
        if (m->scr_state_ < 2 && m->playing_) m->screen_watch(a);
#ifdef TZXCONV_DEBUG_WRITES
        TZXCONV_DEBUG_WRITES(m->st_.frames, a, Z80_PC(m->cpu_));
#endif
        // Bytes stored one after another (up or down), as a loader stores what it reads,
        // even with other stores in between (a checksum, a counter): up to 4 runs are
        // followed. A byte counts from the 3rd of a run, so a 16-bit store (2 bytes) or a
        // PUSH does not. A block copy (LDIR) counts: that only makes the tape play longer.
        // The stack is left out: a CALL between every byte would push the loader's run out.
        if ((uint16_t) (a - Z80_SP(m->cpu_) + 4) < 8) return;
        // A store into the ROM is thrown away: a loader reading a block it doesn't need (Where
        // Time Stood Still's 128K Speedlock 4 loader reads a 22K block into address 0). That
        // is still loading, so it counts. (The runs below are followed as before: what they
        // count depends on which of them are kept, so that is left alone.)
        if (a < 0x4000) m->seq_writes_++;
        // A store to a place not stored to in the last 8 stores: a loader that puts each
        // byte somewhere else (720 Degrees' loading screen goes in byte by byte, scattered)
        // makes one for every byte, a loop waiting for a key none (see the frame's checks)
        bool fresh = true;
        for (int i = 0; i < 8; i++) {
            if (m->recent_[i] == a) {
                fresh = false;
                break;
            }
        }
        if (fresh) {
            m->fresh_writes_++;
            m->recent_[m->recent_next_] = a;
            m->recent_next_ = (uint8_t) ((m->recent_next_ + 1) & 7);
        }
        Stream* st = m->streams_;
        for (int i = 0; i < 4; i++) {
            Stream& r = st[i];
            if (r.len > 0 && (r.dir == 0 ? (uint16_t) (a - r.last + 1) <= 2 && a != r.last
                                         : a == (uint16_t) (r.last + r.dir))) {
                if (r.dir == 0) r.dir = (int8_t) (a == (uint16_t) (r.last + 1) ? 1 : -1);
                r.last = a;
                if (++r.len >= 3) m->seq_writes_++;
                return;
            }
        }
        Stream& r = st[m->stream_next_];       // a new run, in place of the oldest
        m->stream_next_ = (uint8_t) ((m->stream_next_ + 1) & 3);
        r.last = a;
        r.dir = 0;
        r.len = 1;
    }

    static zuint8 in(void* c, zuint16 port) {
        Machine* m = self(c);
#ifdef TZXCONV_DEBUG_IO
        if ((port & 1) && (port >> 8) != 0x7F) TZXCONV_DEBUG_IO(m->st_.frames, 'I', port, 0, Z80_PC(m->cpu_));
#endif
#ifdef TZXCONV_VERIFY_CHECK
        if (!(port & 1)) TZXCONV_VERIFY_CHECK(m->run_base_ + m->cpu_.cycles + z80_in_cycle(&m->cpu_), Z80_B(m->cpu_), (uint8_t) (m->cpu_.r & 0x7F), Z80_F(m->cpu_), Z80_PC(m->cpu_));
#endif
        if (m->model128_ && (port & 0xC002) == 0xC000) return m->ay_[m->ay_sel_ & 15];  // sound chip
        if (port & 1) return 0xFF;      // nothing else is there (no floating bus yet)
        m->reads_++;
        if (m->playing_) {
            uint64_t now = m->run_base_ + m->cpu_.cycles + z80_in_cycle(&m->cpu_);
            if (m->accel_) now += m->accelerate(port, now);
            if (now + 2 >= m->next_change_) m->advance_tape(now + 2);
            m->tape_.note_read();
        }
        uint8_t v = 0x1F;
        if (m->keys_) {
            uint8_t high = (uint8_t) (port >> 8);
            for (int r = 0; r < 8; r++) {
                if (!(high & (1 << r))) v &= m->rows_[r];
            }
        }
        return (uint8_t) (v | 0xA0 | (m->ear_ ? 0x40 : 0));
    }

    static void out(void* c, zuint16 port, zuint8 v) {
        Machine* m = self(c);
#ifdef TZXCONV_DEBUG_IO
        TZXCONV_DEBUG_IO(m->st_.frames, 'O', port, v, Z80_PC(m->cpu_));
#endif
        if (!(port & 1)) m->border_ = v & 7;
        if (!m->model128_) return;
        if ((port & 0x8002) == 0) m->page(v);                           // 0x7FFD
        else if ((port & 0xC002) == 0xC000) m->ay_sel_ = v;             // 0xFFFD: register
        else if ((port & 0xC002) == 0x8000) m->ay_[m->ay_sel_ & 15] = v; // 0xBFFD: value
    }

    Z80 cpu_;
    const uint8_t* rom_ = nullptr;
    uint8_t* ram_ = nullptr;
    uint8_t* slot_[4] = {};                     // what is at 0x0000, 0x4000, 0x8000, 0xC000
    bool model128_ = false;
    const uint8_t* rom128_[2] = {};
    uint8_t* bank_[8] = {};
    uint8_t p7ffd_ = 0;
    bool locked_ = false;
    uint8_t ay_[16] = {};
    uint8_t ay_sel_ = 0;
    uint32_t frame_t_ = FRAME_T, int_t_ = INT_T;
    Player<File> tape_;
    Stats st_;
    Status status_ = Status::RUNNING;
    Suspect suspect_ = Suspect::NONE;
    uint64_t t_ = 0, run_base_ = 0, next_change_ = 0;
    int32_t overrun_ = 0;
    struct Stream { uint16_t last; int8_t dir; uint8_t len; };
    Stream streams_[4] = {};
    uint8_t stream_next_ = 0;
    uint32_t seq_writes_ = 0, fresh_writes_ = 0;
    uint16_t recent_[8] = {};
    uint8_t recent_next_ = 0;
    uint32_t writes_ = 0;
    Accel acc_ = {};
    bool accel_ = true, delays_ = true, learned_ = true;
    bool flash_ = true, flash_rom_ = false;
    bool hold_ = true;
    uint32_t hold_block_ = ~0u, hold_frames_ = 0;
    bool hold_ok_ = false;
    uint32_t reads_ = 0, after_frames_ = 100, keys_step_ = 0, keys_frame_ = 0;
    uint16_t pc_lo_ = 0, pc_hi_ = 0;
    uint8_t ear_ = 0, border_ = 7;
    uint8_t rows_[8];
    bool keys_ = false;                         // a key is down (rows_ not all 0xFF)
    bool playing_ = false;
    bool delay_on_ = false;                     // accel_ && delays_ && playing_
    bool edges_ = true;
    bool edge_on_ = false;                      // accel_ && edges_ && playing_
    uint32_t ops_ = 0;                          // instructions this frame (to st_.ops at its end)
    void set_flags() { delay_on_ = accel_ && delays_ && playing_; edge_on_ = accel_ && edges_ && playing_; }
    // the loading screen: 0 watching, 1 covered (waiting for a store elsewhere), 2 done
    uint8_t scr_state_ = 0;
    bool scr_new_ = false, flashing_ = false;
    uint16_t scr_pix_ = 0, scr_attr_ = 0;
    static const uint16_t SCR_PIX = 5800, SCR_ATTR = 700;
    TZXCONV_NOINLINE void screen_watch(uint16_t a) {
        if (!flashing_ && tape_.playing_now() != 2) return;
        if (a >= 0x4000 && a < 0x5B00) {
            if (scr_state_ == 0) {
                if (a < 0x5800) { if (scr_pix_ < 6144) scr_pix_++; }
                else if (scr_attr_ < 768) scr_attr_++;
                // nearly all of it: Speedlock leaves a few bytes alone (about 6130 of 6144)
                if (scr_pix_ >= SCR_PIX && scr_attr_ >= SCR_ATTR) scr_state_ = 1;
            }
            return;
        }
        if (scr_state_ != 1 || (uint16_t) (a - Z80_SP(cpu_) + 4) < 8) return;
        // a store of tape data elsewhere: the screen is done, if it shows something
        bool blank = true;
        for (uint32_t i = 1; i < 6144 && blank; i++) blank = ram_[i] == ram_[0];
        for (uint32_t i = 6145; i < 6912 && blank; i++) blank = ram_[i] == ram_[6144];
        if (blank) {
            scr_state_ = 0;
            scr_pix_ = scr_attr_ = 0;
            return;
        }
        scr_state_ = 2;
        scr_new_ = true;
    }
    bool early_ = true;
};

} // namespace tzxconv

#endif
