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

// picosim: the PicoMenu firmware (picoface.cxx, built for a PC) with its NMI ROM running on an
// emulated Spectrum, for testing the menus without a Picoface. The Spectrum is a 48K or a 128K
// (paging, so the NMI ROM's 128K tests see one). The SD card is a RAM disk, the flash a RAM
// littlefs. A script drives it:
//   sd <path on card> <file on PC>     copy a file to the SD card (before "boot")
//   flash <path> <file on PC>          ... to the flash
//   boot                               start the Spectrum (the 48K or 128K ROM) for 2 s
//   nmi                                press the Picoface button (the NMI menu)
//   key <c>                            press a key (a letter, digit, ENTER, BREAK, SPACE)
//   wait <frames>                      run (50 frames a second)
//   shot <file.scr>                    save the screen
//   expect <text>                      fail unless the screen shows the text (OCR of the font)
//   expectnot <text>                   fail if the screen shows the text
//   hide <path on card>                mark a file or folder on the card hidden (before "boot")
//   ls sd|flash <dir>                  list a folder
//   slice <frames>                     the converter runs so many frames a request (0: 120 ms)
//   get sd|flash <path> <file on PC>   copy a file off the card or flash
// usage: sim [-128 rom0 rom1] 48.rom script
#include <stdio.h>
extern "C" int host_conv_slice_frames;
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "pico/stdlib.h"
#include "ff.h"
#include "littlefs-lib/pico_hal.h"
#ifndef Z80_STATIC
#define Z80_STATIC
#endif
#include "Z80.h"

struct MYSTATE { uint32_t flags, nmi_active, writableStartAddress, flags_on_nmi_exit; };
extern volatile MYSTATE rom_state;
extern uint8_t rom_data[32768];
bool sendNmiRequest(uint8_t command, uint16_t param1, uint16_t param2);
void processNmiRequest();
void initFileSystem();

static const uint16_t EXITNMI = 0x125;
static uint8_t rom48[16384], rom128[2][16384];
static uint8_t bank[8][16384];
static bool m128 = false, nmi = false, locked = false;
static uint8_t p7ffd = 0, rows[8];
static Z80 cpu;
static uint64_t frames = 0;
static int requests = 0;

static const uint8_t* rom_for() {
    if (m128) return rom128[(p7ffd >> 4) & 1];
    return rom48;
}
static uint8_t* slot(uint16_t a) {
    switch (a >> 14) {
    case 1: return bank[5];
    case 2: return bank[2];
    default: return bank[m128 ? (p7ffd & 7) : 0];
    }
}
static zuint8 rd(void*, zuint16 a) {
    if (a < 0x4000) return nmi ? rom_data[16384 + a] : rom_for()[a];
    return slot(a)[a & 0x3FFF];
}
static zuint8 fetch_op(void* c, zuint16 a) {
    if (a == 0x66 && (rom_state.flags & 2) && !nmi) { nmi = true; rom_state.nmi_active = 1; rom_state.flags &= ~2u; }
    if (a == EXITNMI && nmi) { nmi = false; rom_state.nmi_active = 0; rom_state.flags = rom_state.flags_on_nmi_exit; }
    return rd(c, a);
}
static void wr(void*, zuint16 a, zuint8 v) {
    if (a < 0x4000) { if (nmi) rom_data[16384 + a] = v; return; }
    slot(a)[a & 0x3FFF] = v;
}
static zuint8 in(void*, zuint16 port) {
    if (!(port & 1)) {
        uint8_t v = 0x1F, high = port >> 8;
        for (int r = 0; r < 8; r++) if (!(high & (1 << r))) v &= rows[r];
        return v | 0xA0;
    }
    return 0xFF;
}
static void out(void*, zuint16 port, zuint8 v) {
    if (m128 && (port & 0x8002) == 0 && !locked) { p7ffd = v; if (v & 0x20) locked = true; }
}

static void run_frames(uint32_t n) {
    while (n--) {
        z80_int(&cpu, true);
        z80_run(&cpu, 32);
        z80_int(&cpu, false);
        // run in small steps so the Pico answers the NMI ROM's requests as it would
        for (int k = 0; k < 70; k++) {
            z80_run(&cpu, 1000);
            if (rom_data[16384] == 255) { requests++; processNmiRequest(); }
        }
        frames++;
    }
}

// The key matrix: row and bit of each key
static bool keypos(const std::string& k, int& row, int& bit, bool& shift) {
    static const char* rowkeys[8] = { "^zxcv", "asdfg", "qwert", "12345", "09876", "poiuy", "\nlkjh", " $mnb" };
    shift = false;
    if (k == "ENTER") { row = 6; bit = 0; return true; }
    if (k == "SPACE") { row = 7; bit = 0; return true; }
    if (k == "BREAK") { row = 7; bit = 0; shift = true; return true; }
    char c = (char) tolower(k[0]);
    for (int r = 0; r < 8; r++) for (int b = 0; b < 5; b++) if (rowkeys[r][b] == c) { row = r; bit = b; return true; }
    return false;
}
static void press(const std::string& k) {
    int row, bit; bool shift;
    if (!keypos(k, row, bit, shift)) { printf("unknown key %s\n", k.c_str()); exit(2); }
    memset(rows, 0xFF, 8);
    rows[row] &= ~(1 << bit);
    if (shift) rows[0] &= ~1;
    run_frames(5);
    memset(rows, 0xFF, 8);
    run_frames(5);
}

static const uint8_t* screen() { return (m128 && (p7ffd & 8)) ? bank[7] : bank[5]; }

// Reads the text of the screen with the help font: what chars match exactly
#include "helptext.h"
static std::string screen_text() {
    std::string all;
    const uint8_t* s = screen();
    for (int row = 0; row < 24; row++) {
        std::string line;
        for (int col = 0; col < 32; col++) {
            uint8_t g[8];
            for (int l = 0; l < 8; l++) g[l] = s[((row >> 3) << 11) | (l << 8) | ((row & 7) << 5) | col];
            char ch = '.';
            for (int c = 32; c < 128; c++) {
                const uint8_t* f = help_font[c - 32];
                bool same = true, inv = true;
                for (int l = 0; l < 8; l++) { same &= f[l] == g[l]; inv &= (uint8_t) ~f[l] == g[l]; }
                if (same || inv) { ch = (char) c; break; }
            }
            line += ch;
        }
        all += line + "\n";
    }
    return all;
}

static bool copy_in(bool sd, const char* path, const char* hostfile) {
    FILE* f = fopen(hostfile, "rb");
    if (!f) { printf("cannot read %s\n", hostfile); return false; }
    std::vector<uint8_t> d;
    uint8_t b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) d.insert(d.end(), b, b + n);
    fclose(f);
    // the folders on the way
    std::string sofar;
    for (const char* q = path + 1; *q; q++) {
        if (*q == '/') {
            sofar = std::string(path, q - path);
            if (sd) f_mkdir(sofar.c_str()); else pico_mkdir(sofar.c_str());
        }
    }
    if (sd) {
        FIL fil;
        if (f_open(&fil, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) { printf("cannot make %s\n", path); return false; }
        UINT w; f_write(&fil, d.data(), d.size(), &w); f_close(&fil);
        return w == d.size();
    }
    int h = pico_open(path, LFS_O_CREAT | LFS_O_TRUNC | LFS_O_WRONLY);
    if (h < 0) { printf("cannot make %s in the flash\n", path); return false; }
    pico_write(h, d.data(), d.size());
    pico_close(h);
    return true;
}

static void list(bool sd, const char* dir) {
    printf("-- %s %s:\n", sd ? "sd" : "flash", dir);
    if (sd) {
        DIR dj; FILINFO fi;
        if (f_opendir(&dj, dir) != FR_OK) return;
        while (f_readdir(&dj, &fi) == FR_OK && fi.fname[0]) printf("   %-48s %8lu\n", fi.fname, (unsigned long) fi.fsize);
        f_closedir(&dj);
    } else {
        int d = pico_dir_open(dir);
        struct lfs_info info;
        while (d >= 0 && pico_dir_read(d, &info) > 0) printf("   %-48s %8lu\n", info.name, (unsigned long) info.size);
        if (d >= 0) pico_dir_close(d);
    }
}

int main(int argc, char** argv) {
    int a = 1;
    if (argc > 3 && strcmp(argv[1], "-128") == 0) {
        m128 = true;
        FILE* f0 = fopen(argv[2], "rb"); FILE* f1 = fopen(argv[3], "rb");
        if (!f0 || !f1 || fread(rom128[0], 1, 16384, f0) != 16384 || fread(rom128[1], 1, 16384, f1) != 16384) { printf("128K ROMs?\n"); return 2; }
        a = 4;
    }
    if (argc < a + 2) { printf("usage: sim [-128 rom0 rom1] 48.rom script\n"); return 2; }
    FILE* f = fopen(argv[a], "rb");
    if (!f || fread(rom48, 1, 16384, f) != 16384) { printf("48.rom?\n"); return 2; }
    // a formatted card and flash, then the firmware's own start up
    static FATFS fs; static BYTE work[4096];
    MKFS_PARM opt = { FM_FAT32, 0, 0, 0, 0 };
    if (f_mkfs("0:", &opt, work, sizeof(work)) != FR_OK) { printf("mkfs failed\n"); return 2; }
    f_mount(&fs, "0:", 1);
    pico_mount(true);
    pico_unmount();
    memset(rows, 0xFF, 8);
    FILE* sc = fopen(argv[a + 1], "r");
    if (!sc) { printf("script?\n"); return 2; }
    char line[1024];
    int failures = 0;
    bool booted = false;
    while (fgets(line, sizeof(line), sc)) {
        char cmd[32] = "", p1[512] = "", p2[512] = "";
        if (line[0] == '#' || sscanf(line, "%31s %511s %511[^\n]", cmd, p1, p2) < 1) continue;
        std::string c = cmd;
        if (c == "sd" || c == "flash") {
            // sd|flash <path> <file on PC>: the path may have spaces, the PC file not
            std::string rest = std::string(p1) + " " + p2;
            while (!rest.empty() && (rest.back() == '\n' || rest.back() == '\r' || rest.back() == ' ')) rest.pop_back();
            size_t sp = rest.rfind(' ');
            if (sp != std::string::npos) {
                snprintf(p1, sizeof(p1), "%s", rest.substr(0, sp).c_str());
                snprintf(p2, sizeof(p2), "%s", rest.substr(sp + 1).c_str());
            }
            if (c == "flash" && !booted) { pico_mount(false); }
            if (!copy_in(c == "sd", p1, p2)) failures++;
            if (c == "flash" && !booted) pico_unmount();
        } else if (c == "boot") {
            f_unmount("0:");
            initFileSystem();           // the firmware mounts both
            memset(&cpu, 0, sizeof(cpu));
            cpu.fetch_opcode = fetch_op; cpu.fetch = rd; cpu.read = rd; cpu.write = wr;
            cpu.in = in; cpu.out = out; cpu.nop = rd; cpu.int_fetch = rd;
            cpu.options = Z80_MODEL_ZILOG_NMOS;
            z80_power(&cpu, true);
            run_frames(150);
            booted = true;
            printf("booted (%s)\n", m128 ? "128K" : "48K");
        } else if (c == "nmi") {
            if (!sendNmiRequest(0, 0, 0)) printf("NMI not sent\n");
            z80_nmi(&cpu);
            run_frames(25);
        } else if (c == "key") {
            press(p1);
        } else if (c == "wait") {
            run_frames(atoi(p1));
        } else if (c == "shot") {
            FILE* o = fopen(p1, "wb"); fwrite(screen(), 1, 6912, o); fclose(o);
        } else if (c == "text") {
            printf("%s", screen_text().c_str());
        } else if (c == "expectnot") {
            std::string want = line + 10;
            while (!want.empty() && (want.back() == '\n' || want.back() == '\r')) want.pop_back();
            std::string t = screen_text();
            bool ok = t.find(want) == std::string::npos;
            printf("%s expectnot \"%s\"\n", ok ? "PASS" : "FAIL", want.c_str());
            if (!ok) { failures++; printf("%s", t.c_str()); }
        } else if (c == "hide") {
            std::string path = line + 5;
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
            if (f_chmod(path.c_str(), AM_HID, AM_HID) != FR_OK) { failures++; printf("FAIL hide %s\n", path.c_str()); }
        } else if (c == "expect") {
            std::string want = line + 7;                // all of the rest of the line, spaces too
            while (!want.empty() && (want.back() == '\n' || want.back() == '\r')) want.pop_back();
            std::string t = screen_text();
            bool ok = t.find(want) != std::string::npos;
            printf("%s expect \"%s\"\n", ok ? "PASS" : "FAIL", want.c_str());
            if (!ok) { failures++; printf("%s", t.c_str()); }
        } else if (c == "get") {            // get sd|flash <path> <file on PC>
            char p3[512] = "";
            {   // the path may have spaces, the PC file not
                std::string rest = p2;
                while (!rest.empty() && (rest.back() == '\n' || rest.back() == '\r' || rest.back() == ' ')) rest.pop_back();
                size_t sp = rest.rfind(' ');
                if (sp != std::string::npos) {
                    snprintf(p3, sizeof(p3), "%s", rest.substr(0, sp).c_str());
                    snprintf(p2, sizeof(p2), "%s", rest.substr(sp + 1).c_str());
                }
            }
            std::vector<uint8_t> d(512 * 1024);
            size_t got = 0;
            if (strcmp(p1, "sd") == 0) {
                FIL fil; UINT n = 0;
                if (f_open(&fil, p3, FA_READ) == FR_OK) { f_read(&fil, d.data(), d.size(), &n); f_close(&fil); got = n; }
            } else {
                int h = pico_open(p3, LFS_O_RDONLY);
                if (h >= 0) { int n = pico_read(h, d.data(), d.size()); pico_close(h); got = n > 0 ? n : 0; }
            }
            FILE* o = fopen(p2, "wb");
            if (o) { fwrite(d.data(), 1, got, o); fclose(o); }
            printf("got %s (%zu bytes)\n", p3, got);
            if (!got) failures++;
        } else if (c == "slice") {
            host_conv_slice_frames = atoi(p1);
        } else if (c == "ls") {
            list(strcmp(p1, "sd") == 0, p2);
        } else {
            printf("unknown command %s\n", cmd);
        }
    }
    printf("%d requests answered, %llu frames, %d failure(s)\n", requests, (unsigned long long) frames, failures);
    return failures ? 1 : 0;
}
