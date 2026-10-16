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

// The tape converter on a PC: tzxconv_host 48.rom tape.tzx out.z80 [after_ms]
// With a 32K ROM file (the 128K editor ROM, then the 48K BASIC ROM: 128.rom) it is a 128K.
// Loads the tape on the emulated Spectrum (tzxconv.h) and saves a .z80 with snapload.h, the
// same code the Pico uses. Prints what happened, for testing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tzxconv.h"
#include "snapload.h"

struct HostFile {
    FILE* f = nullptr;
    int read(void* p, uint32_t n) { return (int) fread(p, 1, n, f); }
    bool seek(uint32_t pos) { return fseek(f, (long) pos, SEEK_SET) == 0; }
    uint32_t tell() { return (uint32_t) ftell(f); }
};

struct HostSink {
    FILE* f;
    bool write(const void* p, uint32_t n) { return fwrite(p, 1, n, f) == n; }
};

static const char* status_name(tzxconv::Status s) {
    switch (s) {
    case tzxconv::Status::RUNNING: return "running";
    case tzxconv::Status::LOADED: return "loaded";
    case tzxconv::Status::TAPE_ERROR: return "R Tape loading error";
    case tzxconv::Status::BAD_TAPE: return "damaged tape or unknown block";
    case tzxconv::Status::TOO_LONG: return "gave up (too long)";
    case tzxconv::Status::STOPPED: return "the Spectrum stopped (HALT, interrupts off)";
    case tzxconv::Status::BASIC_ERROR: return "back in BASIC with an error report";
    case tzxconv::Status::RESET: return "the Spectrum was reset (the tape may need the other machine: 48K or 128K)";
    case tzxconv::Status::SAVE: return "the program went into SAVE (its loader went wrong: try the other machine)";
    }
    return "?";
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s 48.rom tape.tzx|tape.tap out.z80 [after_ms]\n", argv[0]);
        return 2;
    }
    static uint8_t rom[32768];
    static uint8_t sna[27 + 49152];
    static uint8_t buf128[snapload::BUFFER128_SIZE];
    FILE* rf = fopen(argv[1], "rb");
    size_t romlen = rf ? fread(rom, 1, sizeof(rom), rf) : 0;
    if (romlen != 16384 && romlen != 32768) {
        fprintf(stderr, "cannot read a 16K (48K) or 32K (128K) ROM from %s\n", argv[1]);
        return 2;
    }
    fclose(rf);
    const bool m128 = romlen == 32768;
    HostFile tape;
    tape.f = fopen(argv[2], "rb");
    if (!tape.f) {
        fprintf(stderr, "cannot open %s\n", argv[2]);
        return 2;
    }
    size_t n = strlen(argv[2]);
    bool tzx = n >= 4 && strcasecmp(argv[2] + n - 4, ".tzx") == 0;
    uint32_t after = argc > 4 ? (uint32_t) atoi(argv[4]) : 2000;

    static tzxconv::Machine<HostFile> m;
    clock_t c0 = clock();
    if (m128) {
        uint8_t* banks[8];
        for (int i = 0; i < 8; i++) banks[i] = snapload::bank_memory(buf128, (uint32_t) i);
        memset(buf128, 0, snapload::HEADER_AREA);
        m.begin128(rom, rom + 16384, banks, &tape, tzx, after);
    }
    else {
        m.begin(rom, sna + 27, &tape, tzx, after);
    }
    if (getenv("TZXFULL")) m.play_to_end();
    if (getenv("TZXNOACCEL")) m.no_acceleration();
    if (getenv("TZXNOFLASH")) m.no_flash();
    if (getenv("TZXSWITCH")) m.test_switches(getenv("TZXSWITCH")[0] == '1', getenv("TZXSWITCH")[1] == '1');
    tzxconv::Status s;
    while ((s = m.run(getenv("TZXSTEP") ? atoi(getenv("TZXSTEP")) : 50)) == tzxconv::Status::RUNNING) {
        if (m.loading_screen()) {
            printf("loading screen at %.1f s\n", m.stats().frames / 50.0);
            if (const char* g = getenv("TZXGRAB")) {     // TZXGRAB=file.scr: save it
                FILE* f = fopen(g, "wb");
                if (f) { fwrite(m.screen_data(), 1, 6912, f); fclose(f); }
            }
        }
        if (getenv("TZXDUMP") && m.stats().frames == (uint32_t) atoi(getenv("TZXDUMP"))) {
            FILE* d = fopen("/tmp/tzxdump.bin", "wb");     // the 48K, for a look at a loader
            fwrite(sna + 27, 1, 49152, d);
            fclose(d);
        }
        if (getenv("TZXHASH")) {
            uint64_t h = m.state_hash();
            for (int i = 0; i < 49152; i++) h = h * 31 + sna[27 + i];
            printf("F %u %016llx\n", m.stats().frames, (unsigned long long) h);
        }
        if (getenv("TZXTRACE")) {
            const auto& st = m.stats();
            printf("  %5.1f s  block %u  reads %u  pc %04X-%04X  stores %u  playing %u\n", st.frames / 50.0,
                m.tape().blocks(), st.ear_reads_last, st.pc_low, st.pc_high, st.stores_last, m.tape().playing_now());
        }
    }
    double secs = (double) (clock() - c0) / CLOCKS_PER_SEC;
    const auto& st = m.stats();
    printf("result: %s\n", status_name(s));
    printf("emulated: %.1f s (tape ended at %.1f s), host time %.2f s\n",
        st.frames / 50.0, st.tape_end_frame / 50.0, secs);
    printf("instructions: %.1f million (loop passes skipped: %.1f million, blocks flash-loaded: %u)\n", st.ops / 1e6, st.skipped / 1e6, st.flashed);
    if (st.early_frame)
        printf("stopped early: nothing loaded the signal for 5 s (at %.1f s; the rest is not needed)\n",
            st.early_frame / 50.0);
    printf("blocks: %u, stops played through: %u, not played: %u\n",
        m.tape().blocks(), m.tape().stops(), m.tape().unsupported_);
    printf("after the tape: most EAR reads in a frame %u, last frame %u, pc %04X-%04X\n",
        st.ear_reads_max_after, st.ear_reads_last, st.pc_low, st.pc_high);
    if (st.error_code) printf("ROM error report seen: code %u\n", st.error_code - 1);
    if (s == tzxconv::Status::LOADED && m.suspect() != tzxconv::Suspect::NONE)
        printf("warning: %s\n", m.suspect() == tzxconv::Suspect::STILL_READING ?
            "still reading the tape port at the end (a loader waiting?)" : "stopped in BASIC");
    if (s != tzxconv::Status::LOADED) return 1;

    uint16_t pc;
    m.sna_header(sna, pc);
    printf("PC %04X SP %02X%02X IM %u IFF %u R %02X border %u", pc, sna[24], sna[23], sna[25],
        (sna[19] >> 2) & 1, sna[20], sna[26]);
    if (m128) printf(" 7FFD %02X", m.port_7ffd());
    printf("\n");
    HostSink out{ fopen(argv[3], "wb") };
    if (!out.f) return 2;
    static uint8_t io[512];
    bool ok;
    if (m128) {
        // the header as snapload's 128K buffer has it, then the same writer as a 128K save
        snapload::sna_to_z80_header(sna, (uint8_t) pc, (uint8_t) (pc >> 8), buf128);
        buf128[snapload::X_7FFD] = m.port_7ffd();
        buf128[snapload::X_AYFLAG] = 1;
        buf128[snapload::X_AYSEL] = m.ay_selected();
        memcpy(buf128 + snapload::X_AYREGS, m.ay_registers(), 16);
        ok = snapload::write_z80_128(out, buf128, io, sizeof(io));
    }
    else {
        ok = snapload::write_z80_48(out, sna, io, sizeof(io), pc);
    }
    fclose(out.f);
    if (getenv("TZXSCR")) {             // the screen, for a look
        FILE* f = fopen(getenv("TZXSCR"), "wb");
        fwrite(sna + 27, 1, 6912, f);
        fclose(f);
    }
    printf("saved %s: %s\n", argv[3], ok ? "ok" : "FAILED");
    if (const char* shots = getenv("TZXSHOTS")) {
        // the screen now, then after the game has run on: 3 s, 3 s more, then 2 s with
        // SPACE held and 2 s with ENTER held and 4 s more (to get past a title page)
        auto shot = [&](const char* tag) {
            char name[512];
            snprintf(name, sizeof(name), "%s-%s.scr", shots, tag);
            FILE* f = fopen(name, "wb");
            if (f) { fwrite(m.screen(), 1, 6912, f); fclose(f); }
        };
        shot("0");
        m.run_on(150); shot("1");
        m.run_on(150); shot("2");
        // TZXKEY=row,bit: hold that key instead (row 3 bit 0 is "1"; see the keyboard map)
        if (const char* k = getenv("TZXKEY")) {
            m.run_on(100, (uint8_t) atoi(k), (uint8_t) atoi(strchr(k, ',') ? strchr(k, ',') + 1 : "0"));
            m.run_on(300);
        }
        else {
            m.run_on(100, 7, 0); m.run_on(100, 6, 0); m.run_on(200);
        }
        shot("3");
    }
    if (!ok) return 1;
    return m.suspect() == tzxconv::Suspect::NONE ? 0 : 3;    // 3: saved, with a warning
}
