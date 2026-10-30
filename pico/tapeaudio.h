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

// A tape (.tap or .tzx) as sound: the signal tzxconv::Player makes (levels held for so many
// T-states at 3.5 MHz) turned into 16-bit samples at the rate a Bluetooth speaker takes
// (44100 or 48000 a second), for the Spectrum's EAR socket, as a phone's tape app plays it.
//
// Each sample is the average of the signal over its time (-amp for the low level, +amp for
// the high), so an edge that falls inside a sample gives a value between: the edge's time is
// kept to a fraction of a sample, not rounded to one (a sample is about 79 T-states at
// 44100). Nothing is allocated; the time is counted exactly (in T-states times the rate), so
// the tape does not drift however long it plays.
//
// A "stop the tape" block (0x20 with no pause) holds it, as a tape deck would be stopped;
// play() goes on from there. The 0x2A "stop if 48K" is played through (a 128K carries on).

#ifndef INCLUDED_TAPEAUDIO_H
#define INCLUDED_TAPEAUDIO_H

#include <stdint.h>
#include "tzxconv.h"

namespace tapeaudio {

static const uint32_t CLOCK = 3500000;          // T-states a second (as the Player counts them)

template <class File>
class Audio {
public:
    enum State : uint8_t { PLAYING, HELD, ENDED, BAD };

    // The tape in f (tzx: a .tzx, else a .tap) from its start; rate: samples a second
    bool begin(File* f, bool tzx, uint32_t rate) {
        rate_ = rate;
        left_ = 0;
        level_ = 0;
        state_ = PLAYING;
        played_t_ = 0;
        fresh_ = true;
        had_data_ = false;
        if (!player_.begin(f, tzx)) {
            state_ = BAD;
            return false;
        }
        stops_ = player_.stops() - player_.stops48();
        return true;
    }

    // Goes on (quickly, with no sound) to the start of data block n, counted as the tape trap
    // counts them (tzxconv::Player::data_position): playing starts in the pause before it.
    // False if the tape ends first.
    bool skip_to(uint32_t n) {
        uint32_t dur;
        uint8_t lv;
        while (player_.data_position() < n) {
            if (!player_.next(dur, lv)) {
                state_ = player_.end() == tzxconv::TapeEnd::BAD ? BAD : ENDED;
                return false;
            }
        }
        stops_ = player_.stops() - player_.stops48();
        level_ = 0;
        left_ = 0;
        had_data_ = false;
        return true;
    }

    // n samples into out (mono). Once the tape has ended, or while it is held, the samples
    // are silence (0). (32-bit sums and a multiply in place of a division a sample: on the
    // Pico's Cortex-M0+ a 64-bit division costs hundreds of cycles, and there are 44100 a second)
    void fill(int16_t* out, uint32_t n, int16_t amp) {
        const uint32_t per = CLOCK;                 // a sample, in T-states times the rate
        const int32_t scale = (int32_t) (((int64_t) amp << 16) / per);    // amp / per, 16.16
        const int16_t high = (int16_t) ((scale * (int32_t) per) >> 16), low = (int16_t) -high;
        for (uint32_t i = 0; i < n; i++) {
            if (state_ != PLAYING) {
                out[i] = 0;
                continue;
            }
            if (left_ >= per) {                     // no edge in this sample (nearly all of them)
                left_ -= per;
                out[i] = level_ ? high : low;
                continue;
            }
            // the signal over this sample: +1 or -1 times its share of the sample's time
            int32_t sum = 0;
            uint32_t need = per;
            while (need > 0) {
                if (left_ == 0 && !more()) break;
                uint32_t take = left_ < need ? (uint32_t) left_ : need;
                sum += level_ ? (int32_t) take : -(int32_t) take;
                left_ -= take;
                need -= take;
            }
            out[i] = (int16_t) ((sum * scale) >> 16);
        }
    }

    State state() const { return state_; }
    // Plays on after a "stop the tape" (or from where skip_to left it). A stop met before the
    // tape makes a sound again is passed (P pressed just before one means play)
    void play() {
        if (state_ == HELD) state_ = PLAYING;
        fresh_ = true;
    }
    uint32_t data_position() const { return player_.data_position(); }
    uint32_t blocks() const { return player_.blocks(); }
    uint64_t played_ms() const { return played_t_ / (CLOCK / 1000); }   // the tape's own time

    // Whether the sound stopping now (the menu opened: the Spectrum stops listening, while the
    // receiver plays on what it holds, a moment) cuts a block: data is playing, or played less
    // than within_t T-states ago. cut_block(): that data block (counted as data_position).
    bool cut(uint64_t within_t) const {
        return player_.playing_now() == 2 || (had_data_ && played_t_ - data_end_t_ < within_t);
    }
    uint32_t cut_block() const { return player_.playing_now() == 2 ? player_.data_position() : data_block_; }

private:
    // The next piece of the signal into left_ (T-states times the rate)
    bool more() {
        uint32_t dur;
        uint8_t lv;
        if (!player_.next(dur, lv)) {
            state_ = player_.end() == tzxconv::TapeEnd::BAD ? BAD : ENDED;
            return false;
        }
        if (player_.stops() - player_.stops48() != stops_) {
            stops_ = player_.stops() - player_.stops48();
            if (!fresh_) state_ = HELD;             // "stop the tape": the rest of it waits
        }
        if (player_.playing_now() != 0) fresh_ = false;
        level_ = lv;
        left_ = (uint64_t) dur * rate_;
        played_t_ += dur;
        if (player_.playing_now() == 2) {
            had_data_ = true;
            data_end_t_ = played_t_;
            data_block_ = player_.data_position();
        }
        return state_ == PLAYING;
    }

    tzxconv::Player<File> player_;
    uint64_t left_;                 // of the piece being played: T-states times the rate
    uint64_t played_t_;
    uint32_t rate_;
    uint32_t stops_;
    uint8_t level_;
    State state_;
    bool fresh_ = false;            // play() just pressed: no sound made since
    bool had_data_ = false;         // data has played since begin() (or skip_to())
    uint64_t data_end_t_ = 0;       // where the last data played ended (played_t_)
    uint32_t data_block_ = 0;       // and its block
};

}   // namespace tapeaudio

#endif
