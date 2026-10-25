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
//   nmi2                               press it twice quickly (a screen grab, no menu)
//   key <c>                            press a key (a letter, digit, ENTER, BREAK, SPACE)
//   hold <c> <frames>                  hold a key down for so many frames
//   regs                               print PC, SP and the interrupt state
//   expecttape <n>                     fail unless the tape trap has handed over n blocks
//   expectmem <address> <byte>         fail unless the Spectrum's RAM holds that byte there
//   (keys: a letter or digit, ENTER, SPACE, BREAK, DOWN (CAPS SHIFT 6), SYM+<key> (with SYMBOL SHIFT))
//   wait <frames>                      run (50 frames a second)
//   shot <file.scr>                    save the screen
//   expect <text>                      fail unless the screen shows the text (OCR of the font)
//   expectnot <text>                   fail if the screen shows the text
//   hide <path on card>                mark a file or folder on the card hidden (before "boot")
//   ls sd|flash <dir>                  list a folder
//   slice <frames>                     the converter runs so many frames a request (0: 120 ms)
//   clock <ms>                         move the Pico's clock on (for screenshots of timers)
//   get sd|flash <path> <file on PC>   copy a file off the card or flash (fails if it is not there)
//   nofile sd|flash <path>             fail if the file is there
// usage: sim [-128 rom0 rom1] 48.rom script
// SIM_CARD=exfat32k formats the card as exFAT with 32 KB clusters; SIM_HEAP=n is the heap
// FatFs may take a block from (as on the Pico, where running out stops it)
extern "C" int ff_open_objects(void);
extern "C" int sim_card_stuck;
extern "C" long sim_read_us;
bool button_double_press();
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
extern "C" { extern unsigned char* ff_pool; extern size_t ff_pool_size; }

struct MYSTATE { uint32_t flags, nmi_active, writableStartAddress, flags_on_nmi_exit, exit_after_nmi, tape_entry, tape_exit; };
extern volatile MYSTATE rom_state;
extern uint8_t rom_data[32768];
bool sendNmiRequest(uint8_t command, uint16_t param1, uint16_t param2);
void processNmiRequest();
void initFileSystem();

#include "nmi.h"                     // the assembled NMI ROM's addresses (EXITNMI), as piohandler.S takes them
static uint8_t rom48[16384], rom128[2][16384];
static uint8_t bank[8][16384];
static bool m128 = false, nmi = false, locked = false;
static uint8_t p7ffd = 0, rows[8];
static Z80 cpu;
static uint64_t frames = 0;
static int requests = 0, tape_requests = 0;
static const uint16_t TAPE_MBOX = 0x3FF8;
// Tape in, as piohandler.S does it: lr (the watched fetch) is taken from rom_state.tape_entry at
// the start and each time the NMI ROM exits; while the internal ROM serves, an M1 fetch there
// (0x0562 playing, 0x04C2 recording) makes the Pico serve rom_data until the hook's last byte
// (rom_state.tape_exit: 0x0596 or 0x04E2) is read
static const uint16_t TAPE_IN_ENTRY = 0x0562, TAPE_IN_EXIT = 0x0596;
static uint32_t lr_watch = 1;           // the bus handler's lr
static uint32_t exit_watch = 1;         // its r_nmi_exit outside the NMI ROM (shifted, or 1)
static int tape_in_takeovers = 0;
void host_tape_poll();
void host_level_loaders_poll();
// The firmware waits for an NMI it asked for (the tape trap's "load memory"): the Spectrum
// runs on meanwhile, and takes the NMI as the Picoface's button would give it
extern "C" void host_step(void) {
    if ((rom_state.flags & 2) && !nmi) z80_nmi(&cpu);
    z80_run(&cpu, 200);
}

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
    if (a < 0x4000) {
        if (!nmi) {
            if (!(rom_state.flags & 1)) return rom_for()[a];
            uint8_t v = rom_data[a];
            if (exit_watch != 1 && ((uint32_t) a << 18) == exit_watch) {
                // exit_tape: the internal ROM again
                rom_state.flags &= ~1u;
                exit_watch = 1; rom_state.exit_after_nmi = 1;
                rom_state.flags_on_nmi_exit = 0;
                rom_state.writableStartAddress = 32768;
            }
            return v;
        }
        uint8_t v = rom_data[16384 + a];
        // the Picoface puts the Spectrum ROM back once exitnmi has been read: as the opcode of
        // the RET there, or as the last byte of the JP a snapshot load leaves before it
        if (a == EXITNMI) {
            nmi = false; rom_state.nmi_active = 0; rom_state.flags = rom_state.flags_on_nmi_exit;
            exit_watch = rom_state.exit_after_nmi;
            lr_watch = rom_state.tape_entry;
        }
        return v;
    }
    return slot(a)[a & 0x3FFF];
}
static zuint8 fetch_op(void* c, zuint16 a) {
    if (a == 0x66 && (rom_state.flags & 2) && !nmi) { nmi = true; rom_state.nmi_active = 1; rom_state.flags &= ~2u; }
    else if (!nmi && !(rom_state.flags & 1) && ((uint32_t) a << 17) == lr_watch) {
        // activate_tape: the hook's NOP is served, then rom_data until its RET
        tape_in_takeovers++;
        rom_state.flags |= 1;
        rom_state.flags_on_nmi_exit = 1;
        rom_state.writableStartAddress = TAPE_MBOX;
        exit_watch = rom_state.exit_after_nmi = rom_state.tape_exit;     // the watched hook's last byte
        return 0x00;
    }
    return rd(c, a);
}
static void wr(void*, zuint16 a, zuint8 v) {
    if (a < 0x4000) {
        if (nmi) rom_data[16384 + a] = v;
        // a ROM the Pico serves (the tape trap's) is writable from writableStartAddress
        else if ((rom_state.flags & 1) && a >= rom_state.writableStartAddress) rom_data[a] = v;
        return;
    }
    slot(a)[a & 0x3FFF] = v;
}
static uint8_t kempston = 0;            // the joystick: bit 0 right, 1 left, 2 down, 3 up, 4 fire
static uint8_t ay_reg[16], ay_sel = 0;  // the 128's sound chip (registers only)
static zuint8 in(void*, zuint16 port) {
    if (m128 && (port & 0xC002) == 0xC000) return ay_reg[ay_sel & 15];   // 0xFFFD
    if ((port & 0xE0) == 0 && (port & 1)) return kempston;     // port 0x1F (A5-A7 low, as the Picoface decodes it)
    if (!(port & 1)) {
        uint8_t v = 0x1F, high = port >> 8;
        for (int r = 0; r < 8; r++) if (!(high & (1 << r))) v &= rows[r];
        return v | 0xA0;
    }
    return 0xFF;
}
static bool trace_paging = false;
static void out(void*, zuint16 port, zuint8 v) {
    if (trace_paging && m128 && (port & 0x8002) == 0) printf("7FFD <- %02X at PC %04X\n", v, Z80_PC(cpu));
    if (m128 && (port & 0x8002) == 0 && !locked) { p7ffd = v; if (v & 0x20) locked = true; }
    if (m128 && (port & 0xC002) == 0xC000) ay_sel = v;                      // 0xFFFD
    if (m128 && (port & 0xC002) == 0x8000 && ay_sel < 16) ay_reg[ay_sel] = v;   // 0xBFFD
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
            // the tape trap's mailbox (as the firmware's main loop polls it)
            if (!nmi && (rom_state.flags & 1) && rom_data[TAPE_MBOX] == 1) { tape_requests++; host_tape_poll(); }
            if (!nmi) host_level_loaders_poll();       // as the firmware's main loop
        }
        frames++;
    }
}

// The key matrix: row and bit of each key
// shift: 1 CAPS SHIFT, 2 SYMBOL SHIFT with the key ("SYM+p" is ")
static bool keypos(const std::string& k, int& row, int& bit, int& shift) {
    static const char* rowkeys[8] = { "^zxcv", "asdfg", "qwert", "12345", "09876", "poiuy", "\nlkjh", " $mnb" };
    shift = 0;
    if (k == "ENTER") { row = 6; bit = 0; return true; }
    if (k == "SPACE") { row = 7; bit = 0; return true; }
    if (k == "BREAK") { row = 7; bit = 0; shift = 1; return true; }
    if (k == "DOWN") { row = 4; bit = 4; shift = 1; return true; }      // CAPS SHIFT 6
    if (k == "DELETE") { row = 4; bit = 0; shift = 1; return true; }    // CAPS SHIFT 0
    if (k.size() == 5 && k.compare(0, 4, "SYM+") == 0) {
        bool ok = keypos(k.substr(4), row, bit, shift);
        shift = 2;
        return ok;
    }
    char c = (char) tolower(k[0]);
    for (int r = 0; r < 8; r++) for (int b = 0; b < 5; b++) if (rowkeys[r][b] == c) { row = r; bit = b; return true; }
    return false;
}
static void press(const std::string& k) {
    int row, bit, shift;
    if (!keypos(k, row, bit, shift)) { printf("unknown key %s\n", k.c_str()); exit(2); }
    memset(rows, 0xFF, 8);
    rows[row] &= ~(1 << bit);
    if (shift == 1) rows[0] &= ~1;
    if (shift == 2) rows[7] &= ~2;
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
            if (sd) {
                // the test's own folders: FatFs is lent memory to clear the cluster (not the
                // firmware's doing, so not held against its heap)
                static unsigned char pool[65536];
                ff_pool = pool;
                ff_pool_size = sizeof(pool);
                f_mkdir(sofar.c_str());
                ff_pool = nullptr;
                ff_pool_size = 0;
            }
            else pico_mkdir(sofar.c_str());
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

// As the Pico's malloc: FatFs's blocks from the heap (ffsystem.c, built with FF_HOST_HEAP_LIMIT)
// stop the Pico when they don't fit in what a Pico has left (SIM_HEAP, default 4096 bytes:
// check_heap.py's least is 5 KB, littlefs holds about 2 KB of it)
extern "C" { extern unsigned char* ff_pool; extern size_t ff_pool_size; }
extern "C" void ff_host_heap_check(size_t n) {
    const char* e = getenv("SIM_HEAP");
    size_t limit = e != nullptr ? (size_t) atoi(e) : 4096;
    if (n > limit) {
        printf("PANIC: out of memory (FatFs asked the heap for %u bytes)\n", (unsigned) n);
        exit(3);
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
    // SIM_CARD=exfat32k: an exFAT card with 32 KB clusters (as a big card is), else FAT32
    const char* card = getenv("SIM_CARD");
    MKFS_PARM opt = { FM_FAT32, 0, 0, 0, 0 };
    if (card != nullptr && strcmp(card, "exfat32k") == 0) opt = { FM_EXFAT, 0, 0, 0, 32768 };
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
    int last_open = 0;
    while (fgets(line, sizeof(line), sc)) {
        {   // files and folders the firmware has open (FatFs keeps 16 at most): reported when it changes
            int o = ff_open_objects();
            if (o != last_open) { printf("OPEN OBJECTS %d (before: %s", o, line); last_open = o; }
        }
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
            // as the Picoface: no NMI while the menu is open (a second one would
            // overwrite the registers it keeps for the way back)
            if (sendNmiRequest(0, 0, 0)) z80_nmi(&cpu);
            else printf("NMI not sent\n");
            run_frames(25);
        } else if (c == "nmi2") {
            // two quick presses of the button: the screen is grabbed with no menu
            if (sendNmiRequest(STARTUP_ACTION_GRAB, 0, 0)) z80_nmi(&cpu);
            else printf("NMI not sent\n");
            run_frames(25);
        } else if (c == "expecttape") {     // expecttape <n>: fail unless the tape trap gave n blocks
            if (tape_requests >= atoi(p1)) printf("PASS expecttape %s (%d)\n", p1, tape_requests);
            else { printf("FAIL expecttape %s (%d)\n", p1, tape_requests); failures++; }
        } else if (c == "expecttapemax") {  // expecttapemax <n>: fail if the tape trap gave more than n blocks
            if (tape_requests <= atoi(p1)) printf("PASS expecttapemax %s (%d)\n", p1, tape_requests);
            else { printf("FAIL expecttapemax %s (%d)\n", p1, tape_requests); failures++; }
        } else if (c == "regs") {           // where the Spectrum is
            printf("PC %04X SP %04X IFF1 %d IM %d I %02X nmi %d rom flags %u halt %d 7FFD %02X\n", Z80_PC(cpu), Z80_SP(cpu), cpu.iff1, cpu.im, cpu.i, nmi, rom_state.flags, cpu.halt_line, p7ffd);
        } else if (c == "hold") {           // hold <key> <frames>: a key held down longer
            // several keys at once: hold 4,1 50
            memset(rows, 0xFF, 8);
            std::string keys = p1;
            for (size_t st = 0; st <= keys.size();) {
                size_t en = keys.find(',', st);
                if (en == std::string::npos) en = keys.size();
                std::string k = keys.substr(st, en - st);
                int row, bit, shift;
                if (!keypos(k, row, bit, shift)) { printf("unknown key %s\n", k.c_str()); return 2; }
                rows[row] &= ~(1 << bit);
                if (shift == 1) rows[0] &= ~1;
                if (shift == 2) rows[7] &= ~2;
                st = en + 1;
            }
            run_frames((uint32_t) atoi(p2));
            memset(rows, 0xFF, 8);
        } else if (c == "key") {
            press(p1);
        } else if (c == "joy") {            // joy <bits> <frames>: the Kempston joystick held (1 right 2 left 4 down 8 up 16 fire)
            kempston = (uint8_t) strtoul(p1, nullptr, 0);
            run_frames((uint32_t) atoi(p2));
            kempston = 0;
        } else if (c == "joyset") {         // joyset <bits>: the joystick stays so until joyset 0
            kempston = (uint8_t) strtoul(p1, nullptr, 0);
        } else if (c == "poke") {           // poke <address> <byte>: (debugging) write the Spectrum's RAM as paged now
            unsigned a = (unsigned) strtoul(p1, nullptr, 0);
            if (a >= 0x4000 && a < 0x10000) slot((uint16_t) a)[a & 0x3FFF] = (uint8_t) strtoul(p2, nullptr, 0);
        } else if (c == "jump") {           // jump <pc> <a>: (debugging) carry on at pc with A set
            Z80_PC(cpu) = (zuint16) strtoul(p1, nullptr, 0);
            Z80_A(cpu) = (zuint8) strtoul(p2, nullptr, 0);
        } else if (c == "tracepaging") {    // tracepaging 1|0: print each write to port 0x7FFD
            trace_paging = atoi(p1) != 0;
        } else if (c == "dumpmem") {        // dumpmem <file>: the 64K the Z80 sees now (ROM as served) to a file
            FILE* o = fopen(p1, "wb");
            for (unsigned a = 0; a < 0x10000; a++) { uint8_t b = rd(nullptr, (uint16_t) a); fputc(b, o); }
            fclose(o);
            for (int b = 0; b < 8; b++) {       // and each 128K bank: <file>.bank0 ...
                std::string n = std::string(p1) + ".bank" + std::to_string(b);
                FILE* f = fopen(n.c_str(), "wb"); fwrite(bank[b], 1, 16384, f); fclose(f);
            }
        } else if (c == "expectmem") {      // expectmem <address> <byte>: the Spectrum's RAM (as paged now)
            unsigned a = (unsigned) strtoul(p1, nullptr, 0), want = (unsigned) strtoul(p2, nullptr, 0);
            unsigned got = a >= 0x4000 && a < 0x10000 ? slot((uint16_t) a)[a & 0x3FFF] : 256;
            if (got == want) printf("PASS expectmem %u %u\n", a, want);
            else { printf("FAIL expectmem %u %u (it is %u)\n", a, want, got); failures++; }
        } else if (c == "double") {         // double: two quick presses of the button (a menu up: a menu grab)
            if (!button_double_press()) printf("double press not taken\n");
            run_frames(25);
        } else if (c == "slowcard") {       // slowcard <us>: each sector read takes that long (simulated time)
            sim_read_us = atol(p1);
        } else if (c == "cardstuck") {      // cardstuck: the SD card stops answering (until initialised again)
            sim_card_stuck = 1;
        } else if (c == "leak") {           // leak <n>: n folders of the SD card opened and never closed (a bug's leftovers)
            static DIR leaked[20];
            for (int i = 0, n = atoi(p1); i < n && i < 20; i++) f_opendir(&leaked[i], "/Games");    // (the root takes no place)
            printf("open objects now %d\n", ff_open_objects());
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
        } else if (c == "ay") {             // ay <register> <value>: as the game wrote it
            ay_reg[atoi(p1) & 15] = (uint8_t) strtoul(p2, nullptr, 0);
        } else if (c == "expectay") {       // expectay <register> <value>
            unsigned r = atoi(p1) & 15, want = (unsigned) strtoul(p2, nullptr, 0);
            if (ay_reg[r] == want) printf("PASS expectay %u %u\n", r, want);
            else { printf("FAIL expectay %u %u (it is %u)\n", r, want, ay_reg[r]); failures++; }
        } else if (c == "expectbyte") {     // expectbyte sd <path>|<offset>|<value>: a byte of a file on the SD card
            std::string rest = line + 14;
            while (!rest.empty() && (rest.back() == '\n' || rest.back() == '\r')) rest.pop_back();
            size_t b1 = rest.find('|'), b2 = rest.rfind('|');
            std::string path = rest.substr(0, b1);
            long off = atol(rest.substr(b1 + 1, b2 - b1 - 1).c_str());
            unsigned want = (unsigned) strtoul(rest.substr(b2 + 1).c_str(), nullptr, 0), got = 256;
            FIL fil; UINT n = 0; uint8_t byte = 0;
            if (f_open(&fil, path.c_str(), FA_READ) == FR_OK) {
                if (f_lseek(&fil, off) == FR_OK && f_read(&fil, &byte, 1, &n) == FR_OK && n == 1) got = byte;
                f_close(&fil);
            }
            if (got == want) printf("PASS expectbyte %s %ld %u\n", path.c_str(), off, want);
            else { printf("FAIL expectbyte %s %ld %u (it is %u)\n", path.c_str(), off, want, got); failures++; }
        } else if (c == "expectfile") {     // expectfile sd <path>|<text>: the SD card's file has that text in it
            std::string rest = line + 14;
            while (!rest.empty() && (rest.back() == '\n' || rest.back() == '\r')) rest.pop_back();
            size_t bar = rest.find('|');
            std::string path = rest.substr(0, bar), want = bar == std::string::npos ? "" : rest.substr(bar + 1);
            std::string got;
            FIL fil; UINT n = 0; char buf[4096];
            if (f_open(&fil, path.c_str(), FA_READ) == FR_OK) { f_read(&fil, buf, sizeof(buf), &n); f_close(&fil); got.assign(buf, n); }
            bool ok = n > 0 && got.find(want) != std::string::npos;
            printf("%s expectfile %s has \"%s\"\n", ok ? "PASS" : "FAIL", path.c_str(), want.c_str());
            if (!ok) { failures++; printf("%s\n", got.c_str()); }
        } else if (c == "nofile") {         // nofile sd|flash <path>: fail if the file is there
            std::string path = p2;
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' ')) path.pop_back();
            bool there;
            if (strcmp(p1, "sd") == 0) { FILINFO fi; there = f_stat(path.c_str(), &fi) == FR_OK; }
            else { int h = pico_open(path.c_str(), LFS_O_RDONLY); there = h >= 0; if (h >= 0) pico_close(h); }
            if (!there) printf("PASS nofile %s\n", path.c_str());
            else { printf("FAIL nofile %s (it is there)\n", path.c_str()); failures++; }
        } else if (c == "clock") {
            sleep_ms((uint32_t) atoi(p1));
        } else if (c == "slice") {
            host_conv_slice_frames = atoi(p1);
        } else if (c == "ls") {
            list(strcmp(p1, "sd") == 0, p2);
        } else {
            printf("unknown command %s\n", cmd);
        }
    }
    if (tape_in_takeovers) printf("tape in: the Pico took over (0x0562 playing, 0x04C2 recording) %d time(s)\n", tape_in_takeovers);
    printf("%d requests answered, %d tape blocks asked for, %llu frames, %d failure(s)\n", requests, tape_requests, (unsigned long long) frames, failures);
    return failures ? 1 : 0;
}
