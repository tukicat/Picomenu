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

// The Bluetooth tape's sound, checked on a PC:
//   btaudio_host tape.tzx out.tzx [rate] [mono|joint] [bitpool] [out.wav]
// The tape is made into sound as the Pico W makes it (pico/tapeaudio.h), coded with SBC as
// for Bluetooth (BTstack's encoder, the one on the Pico), decoded again as a speaker would,
// and the result turned back into a tape: each zero crossing is an edge, as at the
// Spectrum's EAR input, in a CSW block (0x18). out.tzx is then loaded with tzxconv_host, by
// the tape's own loader on the emulated Spectrum: if it loads, the sound would.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "tapeaudio.h"
extern "C" {
#include "btstack_sbc.h"
}

struct HostFile {
    FILE* f = nullptr;
    int read(void* p, uint32_t n) { return (int) fread(p, 1, n, f); }
    bool seek(uint32_t pos) { return fseek(f, (long) pos, SEEK_SET) == 0; }
    uint32_t tell() { return (uint32_t) ftell(f); }
};

static std::vector<int16_t> decoded;
static void on_pcm(int16_t* data, int n, int ch, int, void*) {
    for (int i = 0; i < n; i++) decoded.push_back(data[i * ch]);     // the first channel
}

static void put_le(std::vector<uint8_t>& v, uint32_t x, int n) {
    for (int i = 0; i < n; i++) v.push_back((uint8_t) (x >> (8 * i)));
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s tape.tzx|tape.tap out.tzx [44100|48000] [mono|joint] [bitpool] [out.wav]\n", argv[0]);
        return 2;
    }
    uint32_t rate = argc > 3 ? (uint32_t) atoi(argv[3]) : 44100;
    bool mono = argc > 4 ? strcmp(argv[4], "joint") != 0 : true;
    int bitpool = argc > 5 ? atoi(argv[5]) : (mono ? 31 : 53);
    HostFile f;
    f.f = fopen(argv[1], "rb");
    if (!f.f) { perror(argv[1]); return 2; }
    size_t n = strlen(argv[1]);
    bool tzx = n > 4 && strcasecmp(argv[1] + n - 4, ".tzx") == 0;
    static tapeaudio::Audio<HostFile> audio;
    if (!audio.begin(&f, tzx, rate)) { fprintf(stderr, "not a tape\n"); return 2; }

    static btstack_sbc_encoder_state_t enc;
    btstack_sbc_encoder_init(&enc, SBC_MODE_STANDARD, 16, 8, SBC_ALLOCATION_METHOD_LOUDNESS, rate, bitpool,
        mono ? SBC_CHANNEL_MODE_MONO : SBC_CHANNEL_MODE_JOINT_STEREO);
    static btstack_sbc_decoder_state_t dec;
    btstack_sbc_decoder_init(&dec, SBC_MODE_STANDARD, on_pcm, nullptr);
    const int frame = btstack_sbc_encoder_num_audio_frames();       // 128 samples
    std::vector<int16_t> sent;
    int16_t amp = 30000;
    uint64_t frames = 0, sbc_bytes = 0;
    int silence_after = 0;
    while (true) {
        int16_t m[128], st[256];
        audio.fill(m, frame, amp);
        if (audio.state() == tapeaudio::Audio<HostFile>::HELD) {
            if (++silence_after > (int) (rate * 2 / frame)) {         // a stop: 2 s, then play on
                audio.play();
                silence_after = 0;
                printf("stop the tape: played on after 2 s\n");
            }
        }
        for (int i = 0; i < frame; i++) sent.push_back(m[i]);
        int16_t* in = m;
        if (!mono) {
            for (int i = 0; i < frame; i++) st[2 * i] = st[2 * i + 1] = m[i];
            in = st;
        }
        btstack_sbc_encoder_process_data(in);
        sbc_bytes += btstack_sbc_encoder_sbc_buffer_length();
        btstack_sbc_decoder_process_data(&dec, 0, btstack_sbc_encoder_sbc_buffer(), btstack_sbc_encoder_sbc_buffer_length());
        frames++;
        if (audio.state() == tapeaudio::Audio<HostFile>::ENDED || audio.state() == tapeaudio::Audio<HostFile>::BAD) {
            if (++silence_after > (int) (rate / frame)) break;      // a second more
        }
    }
    printf("%s: %llu frames, %.1f s, SBC %u bytes/frame (%.0f kbit/s), tape %s\n", argv[1],
        (unsigned long long) frames, frames * frame / (double) rate, (unsigned) btstack_sbc_encoder_sbc_buffer_length(),
        sbc_bytes * 8.0 / (frames * frame / (double) rate) / 1000,
        audio.state() == tapeaudio::Audio<HostFile>::ENDED ? "ended" : "BAD");

    // edges at the zero crossings (a little hysteresis, as a comparator has), into CSW runs
    std::vector<uint8_t> csw;
    uint32_t pulses = 0, run = 0;
    int lev = 0;
    const int HYST = 1500;
    auto put_run = [&](uint32_t r) {
        if (r == 0) return;
        if (r < 256) csw.push_back((uint8_t) r);
        else { csw.push_back(0); put_le(csw, r, 4); }
        pulses++;
    };
    // BTHP=<Hz>: the speaker's output AC-coupled first (a high-pass filter, one pole), and
    // BTOFFSET=<n>: the comparator's threshold that far from the middle, as a real EAR input
    double hp = getenv("BTHP") ? atof(getenv("BTHP")) : 0;
    int offset = getenv("BTOFFSET") ? atoi(getenv("BTOFFSET")) : 0;
    if (hp > 0) {
        double a = 1.0 / (1.0 + 2 * 3.14159265 * hp / rate), y = 0, xp = 0;
        for (auto& v : decoded) { y = a * (y + v - xp); xp = v; v = (int16_t) (y > 32767 ? 32767 : y < -32768 ? -32768 : y); }
    }
    bool first = true;
    for (int16_t s0 : decoded) {
        int s = s0 - offset;
        int want = lev;
        if (s > HYST) want = 1;
        else if (s < -HYST) want = 0;
        if (want != lev) {
            if (first) { first = false; run = 0; }
            else put_run(run);
            run = 0;
            lev = want;
        }
        run++;
    }
    put_run(run);
    std::vector<uint8_t> t;
    const char hdr[] = "ZXTape!\x1A";
    t.insert(t.end(), hdr, hdr + 8);
    t.push_back(1); t.push_back(20);
    t.push_back(0x18);
    put_le(t, (uint32_t) (10 + csw.size()), 4);
    put_le(t, 0, 2);                        // pause after
    put_le(t, rate, 3);
    t.push_back(1);                         // RLE
    put_le(t, pulses, 4);
    t.insert(t.end(), csw.begin(), csw.end());
    FILE* o = fopen(argv[2], "wb");
    fwrite(t.data(), 1, t.size(), o);
    fclose(o);
    printf("%u edges into %s\n", (unsigned) pulses, argv[2]);
    if (argc > 6) {                         // the decoded sound as a WAV, to listen to
        std::vector<uint8_t> w;
        uint32_t bytes = (uint32_t) decoded.size() * 2;
        w.insert(w.end(), "RIFF", "RIFF" + 4); put_le(w, 36 + bytes, 4);
        w.insert(w.end(), "WAVEfmt ", "WAVEfmt " + 8); put_le(w, 16, 4); put_le(w, 1, 2); put_le(w, 1, 2);
        put_le(w, rate, 4); put_le(w, rate * 2, 4); put_le(w, 2, 2); put_le(w, 16, 2);
        w.insert(w.end(), "data", "data" + 4); put_le(w, bytes, 4);
        FILE* wf = fopen(argv[6], "wb");
        fwrite(w.data(), 1, w.size(), wf);
        fwrite(decoded.data(), 2, decoded.size(), wf);
        fclose(wf);
    }
    return 0;
}
