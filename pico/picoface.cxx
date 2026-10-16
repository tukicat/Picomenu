/*
Copyright (C) 2024 Brian Apps

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

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#ifdef WIFI_ENABLE
#include "pico/cyw43_arch.h"
#endif
#include "hardware/structs/bus_ctrl.h"
#include "usb_command_handler.h"
#include "usb_msc.h"
#include "gameinfo.h"
#include "screengrab.h"
#include "notes.h"
#include "sdsetup.h"
#include <new>
#include "tusb.h"


#include "http_server.h"
#include "littlefs-lib/pico_hal.h"


#include "picoface.pio.h"

#include "f_util.h"
#include "ff.h"
#include "hw_config.h"

#include "nmi.h"
#include "picoface.h"
#include "file_wrapper.h"
#include "snapload.h"
#include "tzxconv_pico.h"
#include "helptext.h"
#include "extras.h"
#include "fsops.h"
#ifndef PICOFACE_HOST
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/structs/psm.h"
#include "hardware/structs/watchdog.h"
#include "pico/bootrom.h"
#endif
// The updater's functions that run while the program is being replaced live in RAM
#define FW_RAMFUNC __attribute__((section(".time_critical.fwupdate")))
#include "fwupdate.h"


bool sendNmiRequest(uint8_t command, uint16_t param1, uint16_t param2);
static void tape_service_stop();

// NMI_ROM / NMI_ROM_SIZE are already declared by nmi.h

extern const uint8_t FGH_ROM[];
extern const uint32_t FGH_ROM_SIZE;

// set high to disable rom.
// When low /CSROM is floating but when high this pulls /CSROM to GND and
// therefore gives the pico control of the data lines when the Z80 is 
// accessing memory in the lower 16KB.
const uint8_t PIN_CSROM = 26; 
const uint8_t PIN_PICOREQ = 22;
const uint8_t PIN_XE = 20;

const uint8_t PIN_USER_SWITCH = 27;
const uint8_t PIN_NMI = 21;

const uint MY_SM = 0;
const uint SM_OUTDATA = 1;

const uint8_t PIN_D0 = 12;
const uint8_t PIN_A0xA8 = 4;


struct MYSTATE {
    // bit 0 is set if we are serving a rom from the pico
    // bit 1 is set if we should serve the nmi rom when we execute an instruction at 0x66
    uint32_t flags;
    // non-zero if the nmi rom is active.
    uint32_t nmi_active;
    // The address in ROM that writes are enabled from. If a ROM is active then
    // we allow writes from this address onwards. For writable ROM swe prevent
    // write below address 16 because the normal Spectrum rom does write to
    // these lower locations in a number of places and relies on this having
    // no effect. Setting this value above 16KB effectively disables writes and
    // means we are serving a ROM
    uint32_t writableStartAddress;
    uint32_t flags_on_nmi_exit;
};

volatile struct MYSTATE rom_state = {
    .flags = 0,  .nmi_active=0, .writableStartAddress=32768, .flags_on_nmi_exit= 0
};
uint8_t rom_data[32768];

// HIGH-SPEED TAPE ENGINE GLOBAL STATE
extern "C" {
    void piohandler();                        // Keep this assembly function here
}

static bool enable_logging = true;

void LOG(const char* format, ...) {
    if (enable_logging) {
        va_list args;
        va_start(args, format);
        vprintf(format, args);
    }
}


extern "C" {
    void piohandler();
}


void __time_critical_func(do_my_pio)() {
    PIO pio = pio0;
    pio_sm_claim(pio, MY_SM);
    pio_sm_claim(pio, SM_OUTDATA);
    
    pio_gpio_init(pio, PIN_XE);
    pio_gpio_init(pio, PIN_CSROM);

    for (uint i = 0; i < 8; ++i) {
        pio_gpio_init(pio, PIN_D0 + i);
    }

    pio_sm_set_consecutive_pindirs(pio, MY_SM, PIN_XE, 1, true);
    pio_sm_set_consecutive_pindirs(pio, SM_OUTDATA, PIN_CSROM, 1, true);
    pio_sm_set_consecutive_pindirs(pio, SM_OUTDATA, PIN_D0, 8, false);

    uint offset = pio_add_program(pio, &fetchaddr_program);
    pio_sm_config sm_config = fetchaddr_program_get_default_config(offset);
    sm_config_set_in_pins(&sm_config, PIN_A0xA8);
    sm_config_set_in_shift(&sm_config, true, true, 32);
    sm_config_set_sideset_pins(&sm_config, PIN_XE);

    pio_sm_init(pio, MY_SM, offset, &sm_config);
    pio_sm_set_enabled(pio, MY_SM, true);

    offset = pio_add_program(pio, &putdata_program);
    sm_config = putdata_program_get_default_config(offset);
    sm_config_set_sideset_pins(&sm_config, PIN_CSROM);
    sm_config_set_out_pins(&sm_config, PIN_D0, 8);
    pio_sm_init(pio, SM_OUTDATA, offset, &sm_config);
    pio_sm_set_enabled(pio, SM_OUTDATA, true);

    piohandler();
}



#define SNA_LOAD_SIZE (SNA_FILE_SIZE)

// Big enough for a 128K snapshot (see snapload.h).  A 48K snapshot, and the 48K
// of memory that the tape loader and the memory transfers work on, only use
// the start of it.
#define SNAP_LOAD_BUFFER_SIZE (snapload::BUFFER128_SIZE + 1024)

static char snap_load_buffer[SNAP_LOAD_BUFFER_SIZE] __attribute__((aligned(4)));
static_assert(SNAP_LOAD_BUFFER_SIZE >= SNA_LOAD_SIZE + 1024, "buffer too small for a 48K snapshot");

// Snapshot files are read through this a piece at a time
static uint8_t snap_io_buffer[512];

// snap_load_buffer holds a 128K snapshot, as built by snapload.h.  Only
// meaningful between loading a snapshot file and sending it to the NMI rom.
static bool snap_is_paged = false;

// ACTION_SNA_BEGIN_WRITE128 was the last way a snapshot was sent for saving: it is
// a 128K one, which is saved as a .z80 file.  save128_info is the header it sent.
static bool save_is_128k = false;
static uint8_t save128_info[snapload::SAVE128_HEADER];

// The paging port value of the last 128K snapshot loaded, and the byte it had where the
// 128K ROM keeps its copy of that value (BANK_M).  The NMI rom can only read BANK_M,
// which is out of date after a load, or wrong for a game that does its own paging or
// uses that address for something else.  finish_save128 prefers this for the ROM and
// screen bits: a game that is showing bank 5 would otherwise go blank on leaving the
// menu, with the shadow screen paged in.  Not valid after a 48K load, a reset (the long
// press, a tape) or a ROM change, when the ROM sets BANK_M again.
static snapload::LoadedPaging loaded_paging;
static_assert(SAVE128_INFO_SIZE == snapload::SAVE128_HEADER, "save header size differs from nmirom.asm");

// The NMI rom and snapload.h have to agree about the layout of a 128K snapshot
static_assert(Z80X_SIZE == snapload::NMI_HEADER128_SIZE, "header size differs from nmirom.asm");
static_assert(Z80X_7FFD == snapload::X_7FFD && Z80X_AYFLAG == snapload::X_AYFLAG &&
    Z80X_AYSEL == snapload::X_AYSEL && Z80X_AYREGS == snapload::X_AYREGS, "header layout differs from nmirom.asm");
static constexpr uint8_t NMI_PAGES[8] = { NMI_PAGE_ORDER };
static constexpr bool same_page_order() {
    for (int i = 0; i < 8; i++) {
        if (NMI_PAGES[i] != snapload::PAGE_ORDER[i]) {
            return false;
        }
    }
    return true;
}
static_assert(same_page_order(), "bank order differs from page_order in nmirom.asm");

const char* current_snap_data = NULL;
uint32_t current_snap_offset =  0;
uint32_t current_snap_size = 0;
uint32_t current_sna_write_offset = 0;
static bool sd_card_mounted = false;

// The directory of the SD card that the snapshot browser is showing.  "" is
// the root, otherwise it is a path with a leading slash and no trailing one,
// e.g. "/GAMES/ARCADE".  It is kept between visits to the NMI menu.  The internal
// flash has folders too and is browsed the same way, with its own directory.
static char sd_cwd[128] = "";
static char flash_cwd[sizeof(sd_cwd)] = "";

// The directory being browsed on a drive (0 internal flash, 1 SD card)
static char* cwd_of(uint8_t drive) {
    return drive == 1 ? sd_cwd : flash_cwd;
}

// The drive the last ACTION_SNAP_LIST was for
static uint8_t list_drive = 0;

// Big enough for sd_cwd, a slash and the longest file name
static const size_t BROWSE_PATH_MAX = sizeof(sd_cwd) + 260;

// The text the snapshot browser is searching for (ACTION_SNAP_FIND), or ""
// when it is simply listing a directory.
static char snap_filter[16] = "";

// The directory that each entry of the last ACTION_SNAP_LIST is in, in the
// same form as sd_cwd.  A search returns entries from many directories, and
// this is how we know where to find the one that the user then picks.
static const int SNAP_LIST_MAX = 10;
static char list_entry_dir[SNAP_LIST_MAX][sizeof(sd_cwd)];
static uint16_t list_entry_offset[SNAP_LIST_MAX];
static int list_entry_count = 0;

// nameoffset is the address in the NMI rom of a name returned by the list
// The full name and extension of each entry.  The NMI rom is sent names cut
// down to fit its 32 column screen; these are what the entries really are.
static char list_entry_name[SNAP_LIST_MAX][260];

// nameoffset is the address in the NMI rom of a name returned by the list.
// Returns the index of the entry, or -1.
static int list_entry_for(uint16_t nameoffset) {
    for (int i = 0; i < list_entry_count; i++) {
        if (list_entry_offset[i] == nameoffset) {
            return i;
        }
    }
    return -1;
}

static const char* list_entry_dir_for(uint16_t nameoffset) {
    int i = list_entry_for(nameoffset);
    return i >= 0 ? list_entry_dir[i] : cwd_of(list_drive);
}

// The real name (and extension after it) of a name the NMI rom sent back
static const char* list_entry_name_for(uint16_t nameoffset, const char* asSent) {
    int i = list_entry_for(nameoffset);
    return i >= 0 ? list_entry_name[i] : asSent;
}


//---------------------------------------------
// TAP -> 48K SNA conversion
//
// A .tap file is a list of tape blocks, not a machine image, so we build a
// 48K snapshot by loading each CODE block (and SCREEN$) to the address in
// its header, then pointing the snapshot's PC at the address used by
// "RANDOMIZE USR nnnn" in the BASIC loader.  This works for ordinary games
// with a standard BASIC loader.  Turbo/custom loaders and multi-load games
// are not supported.

static bool tap_read_exact(FileHandle& f, void* buf, size_t n) {
    return f.read(buf, n) == (int)n;
}

static bool tap_skip(FileHandle& f, size_t n) {
    uint8_t tmp[64];
    while (n > 0) {
        size_t c = n > sizeof(tmp) ? sizeof(tmp) : n;
        if (!tap_read_exact(f, tmp, c)) return false;
        n -= c;
    }
    return true;
}

// ---- .tzx files ----
// A .tzx is read as the same list of blocks as a .tap: tape_next_block leaves the
// file at the flag byte of the next data block and returns its length (flag, data
// and checksum, as the 2 byte length of a .tap block).  Standard speed (0x10),
// turbo speed (0x11) and pure data (0x14) blocks are data blocks; everything else
// (pauses, tones, pulses, direct recording, text, groups, ...) is skipped.  The
// timings are not used: the ROM trap hands over the bytes, and a tape whose
// loader reads the pulses itself cannot work this way (the same as a turbo .tap).
static bool tape_is_tzx(const char* name) {
    size_t n = strlen(name);
    return n >= 4 && strcasecmp(name + n - 4, ".tzx") == 0;
}

// What a block of a .tzx is.  tzx_read_header reads the block's id and fixed header and
// says how many bytes follow it (the body, which a data block is the data of).
struct TzxBlock {
    uint8_t id;
    bool data;          // 0x10, 0x11 or 0x14 (and long enough to be data: see blen)
    uint32_t blen;      // for a data block, its length (flag, data, checksum)
    uint32_t skip;      // for another block, the bytes that follow its header
    uint8_t h[20];      // its fixed header
};

// Where the tape is: the index of the next block (0 is the first one after the 10
// byte header), and the loop and call sequence being played.  Loops, jumps and calls
// are followed.  A loop that has no data block in it is not repeated, at most
// TZX_MAX_IDLE control blocks are followed in a row and a tape gives at most
// TZX_MAX_BLOCKS data blocks, so a jump to itself or a loop of a million does not
// hang the Picoface.
static const uint32_t TZX_FIRST = 10;
static const uint32_t TZX_MAX_IDLE = 2000, TZX_MAX_BLOCKS = 4096;
static const uint8_t TZX_CALLS = 32;
struct TzxState {
    uint32_t index, idle, given;
    bool loop_on, loop_data;
    uint16_t loop_left;
    uint32_t loop_index, loop_pos;
    bool call_on;
    uint32_t call_index;
    uint8_t call_n, call_i;
    int16_t call_off[TZX_CALLS];
};
static TzxState tz;

// Reads and checks the 10 byte "ZXTape!" 0x1A major minor header, and starts the tape
static bool tzx_begin(FileHandle& f) {
    uint8_t h[10];
    memset(&tz, 0, sizeof(tz));
    return tap_read_exact(f, h, sizeof(h)) && memcmp(h, "ZXTape!\x1A", 8) == 0 && h[8] == 1;
}

// Reads the next block's id and header.  False at the end of the tape, at an unknown
// block or at a damaged file.
static bool tzx_read_header(FileHandle& f, TzxBlock& b) {
    uint8_t id;
    if (!tap_read_exact(f, &id, 1)) return false;
    uint8_t* h = b.h;
    size_t head = 0;       // bytes of fixed header after the id
    b.id = id;
    b.data = false;
    b.blen = 0;
    b.skip = 0;
    auto le = [&](size_t at, size_t bytes) {
        uint32_t v = 0;
        for (size_t i = 0; i < bytes; i++) v |= (uint32_t)h[at + i] << (8 * i);
        return v;
    };
    switch (id) {
        case 0x10: head = 4; b.data = true; break;                   // pause, length(2)
        case 0x11: head = 18; b.data = true; break;                  // 15 bytes, length(3)
        case 0x14: head = 10; b.data = true; break;                  // 7 bytes, length(3)
        case 0x12: head = 4; break;                                  // pure tone
        case 0x13: head = 1; break;                                  // pulses (n * 2 bytes)
        case 0x15: head = 5; break;                                  // direct recording (length(3) at 5, below)
        case 0x18: case 0x19: head = 4; break;                       // CSW, generalised (length(4))
        case 0x20: head = 2; break;                                  // pause / stop the tape
        case 0x21: head = 1; break;                                  // group start (name length)
        case 0x22: break;                                            // group end
        case 0x23: head = 2; break;                                  // jump
        case 0x24: head = 2; break;                                  // loop start
        case 0x25: break;                                            // loop end
        case 0x26: head = 2; break;                                  // call sequence (n * 2 bytes)
        case 0x27: break;                                            // return
        case 0x28: head = 2; break;                                  // select (length(2))
        case 0x2A: head = 4; break;                                  // stop the tape in 48K mode
        case 0x2B: head = 4; break;                                  // set signal level
        case 0x30: head = 1; break;                                  // text description
        case 0x31: head = 2; break;                                  // message (time, length)
        case 0x32: head = 2; break;                                  // archive info (length(2))
        case 0x33: head = 1; break;                                  // hardware type (n * 3 bytes)
        case 0x34: head = 8; break;                                  // emulation info
        case 0x35: head = 20; break;                                 // custom info (id(16), length(4))
        case 0x40: head = 4; break;                                  // snapshot (type, length(3))
        case 0x5A: head = 9; break;                                  // "glue" block
        default: return false;                                       // unknown: stop here
    }
    if (head > sizeof(b.h) || (head > 0 && !tap_read_exact(f, h, head))) return false;
    if (b.data) {
        b.blen = id == 0x10 ? le(2, 2) : id == 0x11 ? le(15, 3) : le(7, 3);
        return true;
    }
    switch (id) {
        case 0x13: b.skip = le(0, 1) * 2; break;
        case 0x15: { uint8_t b3[3]; if (!tap_read_exact(f, b3, 3)) return false;
                     b.skip = b3[0] | (b3[1] << 8) | ((uint32_t)b3[2] << 16); break; }
        case 0x18: case 0x19: case 0x2B: b.skip = le(0, 4); break;
        case 0x21: case 0x30: b.skip = le(0, 1); break;
        case 0x26: b.skip = le(0, 2) * 2; break;
        case 0x28: case 0x32: b.skip = le(0, 2); break;
        case 0x31: b.skip = le(1, 1); break;
        case 0x33: b.skip = le(0, 1) * 3; break;
        case 0x35: b.skip = le(16, 4); break;
        case 0x40: b.skip = le(1, 3); break;
        default: break;
    }
    return true;
}

// Goes to block number 'target' (from the start of the tape).  False if there is no such block.
static bool tzx_goto(FileHandle& f, uint32_t target) {
    if (target < tz.index) {
        if (!f.seek(TZX_FIRST)) return false;
        tz.index = 0;
    }
    while (tz.index < target) {
        TzxBlock b;
        if (!tzx_read_header(f, b)) return false;
        tz.index++;
        if (!tap_skip(f, b.data ? b.blen : b.skip)) return false;
    }
    return true;
}

// Positions the file at the flag byte of the next block and sets blen.  False at
// the end of the tape (or of a damaged file).  'tzx' selects the file type.
static bool tape_next_block(FileHandle& f, bool tzx, uint32_t& blen) {
    if (!tzx) {
        uint8_t lb[2];
        if (!tap_read_exact(f, lb, 2)) return false;
        blen = lb[0] | (lb[1] << 8);
        return true;
    }
    while (true) {
        if (++tz.idle > TZX_MAX_IDLE) return false;
        TzxBlock b;
        const uint32_t here = tz.index;
        if (!tzx_read_header(f, b)) return false;
        tz.index++;
        if (b.data) {
            if (b.blen >= 2) {
                if (++tz.given > TZX_MAX_BLOCKS) return false;
                tz.idle = 0;
                tz.loop_data = true;
                blen = b.blen;
                return true;
            }
            if (!tap_skip(f, b.blen)) return false;                  // nothing usable in it
            continue;
        }
        // where to go next: -1 for the next block
        int32_t target = -1;
        switch (b.id) {
            case 0x24: {                                             // loop start: the number of times
                uint32_t n = b.h[0] | (b.h[1] << 8);
                tz.loop_on = n > 1;
                tz.loop_left = (uint16_t) n;
                tz.loop_data = false;
                tz.loop_index = tz.index;
                tz.loop_pos = f.tell();
                break;
            }
            case 0x25:                                               // loop end: again, if there is more to do
                if (tz.loop_on) {
                    if (tz.loop_left > 1 && tz.loop_data) {
                        tz.loop_left--;
                        if (!f.seek(tz.loop_pos)) return false;
                        tz.index = tz.loop_index;
                        tz.loop_data = false;
                    } else {
                        tz.loop_on = false;
                    }
                }
                break;
            case 0x23: {                                             // jump, relative to this block
                int32_t off = (int16_t)(b.h[0] | (b.h[1] << 8));
                if (off != 0 && off != 1) {                          // 0 would loop for ever
                    int32_t t = (int32_t) here + off;
                    if (t < 0) return false;
                    target = t;
                }
                break;
            }
            case 0x26: {                                             // call sequence: n relative jumps
                uint32_t n = b.h[0] | (b.h[1] << 8);
                uint32_t keep = n < TZX_CALLS ? n : TZX_CALLS;
                for (uint32_t i = 0; i < keep; i++) {
                    uint8_t o[2];
                    if (!tap_read_exact(f, o, 2)) return false;
                    tz.call_off[i] = (int16_t)(o[0] | (o[1] << 8));
                }
                if (!tap_skip(f, (n - keep) * 2)) return false;
                if (!tz.call_on && keep > 0) {                       // one level of calls only
                    tz.call_on = true;
                    tz.call_index = here;
                    tz.call_n = (uint8_t) keep;
                    tz.call_i = 1;
                    int32_t t = (int32_t) here + tz.call_off[0];
                    if (t < 0) return false;
                    target = t;
                }
                b.skip = 0;
                break;
            }
            case 0x27:                                               // return: the next call, or on after the sequence
                if (tz.call_on) {
                    int32_t t;
                    if (tz.call_i < tz.call_n) {
                        t = (int32_t) tz.call_index + tz.call_off[tz.call_i++];
                    } else {
                        tz.call_on = false;
                        t = (int32_t) tz.call_index + 1;
                    }
                    if (t < 0) return false;
                    target = t;
                }
                break;
            default: break;                                          // pauses, tones, text, ... are skipped
        }
        if (b.id != 0x26 && !tap_skip(f, b.skip)) return false;
        if (target >= 0) {
            // the jump is not made in place: the file is now after this block (index is here + 1)
            if (!tzx_goto(f, (uint32_t) target)) return false;
        }
    }
}

// Scan a tokenised BASIC program for "USR <number>" and return the number
// (the last one found that is in RAM), or -1.
static int tap_find_usr_address(const uint8_t* p, size_t len) {
    int result = -1;
    size_t i = 0;
    while (i + 4 <= len) {
        size_t linelen = p[i + 2] | (p[i + 3] << 8);
        size_t start = i + 4;
        size_t end = start + linelen;
        if (end > len) end = len;
        if (start < end && p[start] != 0xEA) {          // skip REM lines
            for (size_t j = start; j < end; j++) {
                if (p[j] == 0x0E) { j += 5; continue; }  // hidden 5 byte number
                if (p[j] == 0xC0) {                      // USR token
                    size_t k = j + 1;
                    while (k < end && p[k] == ' ') k++;
                    if (k < end && p[k] >= '0' && p[k] <= '9') {
                        uint32_t v = 0;
                        while (k < end && p[k] >= '0' && p[k] <= '9' && v <= 65535) {
                            v = v * 10 + (p[k] - '0');
                            k++;
                        }
                        if (v >= 16384 && v <= 65535) result = (int)v;
                    }
                }
            }
        }
        i = end;
    }
    return result;
}

// Fills in the system variables and SNA header so that the snapshot starts
// running at 'entry' (0 resets the Spectrum).
static void tap_write_snapshot_header(int entry) {
    const uint16_t SNAP_SP = 0x5BFE;     // stack in the (unused) printer buffer
    uint8_t* header = reinterpret_cast<uint8_t*>(snap_load_buffer);
    uint8_t* ram = header + SNA_HEADER_SIZE;   // ram[0] is Spectrum address 16384
    // minimal system variables so the ROM interrupt routine behaves
    auto put16 = [](uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; };
    ram[0x5C00 - 16384] = 0xFF;                   // KSTATE free
    ram[0x5C04 - 16384] = 0xFF;
    ram[0x5C09 - 16384] = 35;                     // REPDEL
    ram[0x5C0A - 16384] = 5;                      // REPPER
    put16(ram + 0x5C36 - 16384, 0x3C00);          // CHARS
    put16(ram + 0x5C7B - 16384, 0xFF58);          // UDG
    ram[0x5C8D - 16384] = 0x38;                   // ATTR_P
    ram[0x5C8F - 16384] = 0x38;                   // ATTR_T
    put16(ram + 0x5CB2 - 16384, 0xFFFF);          // RAMTOP
    put16(ram + 0x5CB4 - 16384, 0xFFFF);          // P_RAMT

    // SNA header: PC lives on the stack
    header[0] = 0x3F;                             // I
    put16(header + 1, 0x2758);                    // HL'
    put16(header + 13, (uint16_t)entry);          // BC (as left by USR)
    put16(header + 15, 0x5C3A);                   // IY
    put16(header + 17, (uint16_t)entry);          // IX
    header[19] = 0x04;                            // IFF2: interrupts on
    put16(header + 23, SNAP_SP);                  // SP
    header[25] = 1;                               // interrupt mode 1
    header[26] = 7;                               // border
    put16(ram + SNAP_SP - 16384, (uint16_t)entry);

    current_snap_size = SNA_FILE_SIZE;
    current_snap_data = snap_load_buffer;
}

// A snapshot that simply resets the Spectrum
static void tap_make_reset_snapshot() {
    memset(snap_load_buffer, 0, SNA_FILE_SIZE);
    tap_write_snapshot_header(0);
}

// Builds a 48K SNA in snap_load_buffer.  Always leaves a valid snapshot in
// the buffer; if the tape could not be converted the snapshot simply jumps
// to address 0, which resets the Spectrum.  Returns true only if the tape was
// converted cleanly: every block was loaded and the entry point came from a
// "RANDOMIZE USR" in the BASIC loader.  Otherwise the result is a best guess.
static bool build_snapshot_from_tap(uint8_t drive, const char* filename) {
    const uint32_t PROG_ADDR = 0x5CCB;   // where BASIC normally stores a program
    uint8_t* header = reinterpret_cast<uint8_t*>(snap_load_buffer);
    uint8_t* ram = header + SNA_HEADER_SIZE;   // ram[0] is Spectrum address 16384
    memset(snap_load_buffer, 0, SNA_FILE_SIZE);

    int entry = -1;
    int first_code = -1;
    bool clean = true;

    FileHandle file;
    const bool tzx = tape_is_tzx(filename);
    if (file.open(drive == 1 ? Drive::EXTERNAL_SD_CARD : Drive::INTERNAL_FLASH, filename, true) &&
        (!tzx || tzx_begin(file))) {
        bool have_header = false;
        uint8_t htype = 0;
        uint16_t hlen = 0, hp1 = 0;

        while (true) {
            uint32_t blen;
            if (!tape_next_block(file, tzx, blen)) break;
            if (blen < 2) break;                  // need at least flag + checksum
            uint8_t flag;
            if (!tap_read_exact(file, &flag, 1)) break;
            uint32_t payload = blen - 2;          // excluding flag and checksum

            if (flag == 0x00 && payload == 17) {  // standard header block
                uint8_t h[17];
                if (!tap_read_exact(file, h, 17)) break;
                htype = h[0];
                hlen = h[11] | (h[12] << 8);
                hp1 = h[13] | (h[14] << 8);
                have_header = true;
                if (!tap_skip(file, 1)) break;
                continue;
            }

            uint32_t toread = 0;
            uint8_t* dest = nullptr;
            if (have_header && flag == 0xFF) {
                uint32_t n = payload < hlen ? payload : hlen;
                if (htype == 3 && hp1 >= 16384) {            // CODE / SCREEN$
                    if (n > 65536u - hp1) n = 65536u - hp1;
                    dest = ram + (hp1 - 16384);
                    toread = n;
                    if (first_code < 0) first_code = hp1;
                } else if (htype == 0) {                      // BASIC program
                    if (n > 65536u - PROG_ADDR) n = 65536u - PROG_ADDR;
                    dest = ram + (PROG_ADDR - 16384);
                    toread = n;
                }
            }
            // anything else (headerless/turbo blocks, arrays) is skipped
            bool is_basic = have_header && htype == 0;
            have_header = false;

            if (dest != nullptr) {
                if (!tap_read_exact(file, dest, toread)) break;
                if (is_basic) {
                    int a = tap_find_usr_address(dest, toread);
                    if (a >= 0) entry = a;
                }
                if (!tap_skip(file, payload - toread + 1)) break;
            } else {
                clean = false;                    // a block we could not place
                if (!tap_skip(file, payload + 1)) break;
            }
        }
        file.close();
    } else {
        LOG("Tape Loader: could not open %s\n", filename);
    }

    if (entry < 0) {
        clean = false;
        entry = first_code;
    }
    if (entry < 0) {
        LOG("Tape Loader: nothing loadable found in %s, resetting\n", filename);
        entry = 0;
    }
    LOG("Tape Loader: entry point %d, %s\n", entry, clean ? "clean conversion" : "not a clean conversion");

    tap_write_snapshot_header(entry);
    return clean;
}


static bool tape_service_start(uint8_t drive, const char* filename);

// What a Picoface's own updater looks for in a firmware file: the Rev, then (for firmware up to Rev 1.0, which
// looks for "tapeloader v" and a number) the build number. Every build keeps both in this one text.
static const char FIRMWARE_ID[] = "Rev 1.9 (tapeloader v109)";
// "Rev 1.0": shown on the Diagnostics page of Help and the Update screen. Made from the id so the id is used
// (an unused text is left out of the image by the linker).
static const char* firmware_version() {
    static char version[16];
    if (version[0] == '\0') {
        const char* end = strstr(FIRMWARE_ID, " (");
        size_t n = end != nullptr ? (size_t) (end - FIRMWARE_ID) : strlen(FIRMWARE_ID);
        if (n >= sizeof(version)) n = sizeof(version) - 1;
        memcpy(version, FIRMWARE_ID, n);
        version[n] = '\0';
    }
    return version;
}
#define FIRMWARE_VERSION (firmware_version())

// The last thing that went wrong (until power off), for the Diagnostics page
static char last_error[40] = "";
static void set_last_error(const char* what, const char* name) {
    snprintf(last_error, sizeof(last_error), "%s %s", what, name != nullptr ? name : "");
}

// The file last loaded from the menu (loaded, not saved): L-Reload last game loads
// it again and the Poke list looks for its .pok file.  Kept until power off.
static bool reload_known = false;
// A game has been loaded since the Spectrum was switched on or last reset: Q (quick save)
// needs one, or it would save the empty machine under the name of the game of the last session
static bool game_running = false;
static uint8_t reload_drive = 0;                        // 0 internal flash, 1 SD card
static char reload_dir[sizeof(sd_cwd)] = "";            // same form as sd_cwd
static char reload_name[260];                           // name and extension, as two strings
// The kind of file the last load or save was (ACTION_SNA_SAVE_TYPE_*), which a
// quick-save makes
static uint8_t last_format = ACTION_SNA_SAVE_TYPE_AUTO;

// The game last loaded from the menu, or the snapshot last saved: Save snapshot
// starts with its name and can save over it (ACTION_SNA_SAVE_DIR_OP_LAST).  Kept
// until power off.
static bool last_game_known = false;
static uint8_t last_game_drive = 0;                     // 0 internal flash, 1 SD card
static char last_game_dir[sizeof(sd_cwd)] = "";         // same form as sd_cwd
static char last_game_name[64] = "";                    // without the extension
// the longest name the save screen takes (nmirom.asm)
static const size_t SAVE_NAME_MAX = ACTION_SNA_SAVE_NAME_MAX;
static char grab_path[BROWSE_PATH_MAX];                 // one path buffer for the screen grabs, the recent list and the numbered saves: it is not small
static_assert(sizeof(sd_cwd) == extras::LG_DIR_MAX && sizeof(reload_name) == extras::LG_RELOAD_MAX &&
    sizeof(last_game_name) == extras::LG_NAME_MAX, "sizes differ from extras.h (the last game file)");

static void remember_last_game(uint8_t drive, const char* dir, const char* name) {
    last_game_drive = drive == 1 ? 1 : 0;
    snprintf(last_game_dir, sizeof(last_game_dir), "%s", dir);
    snprintf(last_game_name, sizeof(last_game_name), "%s", name);
    last_game_known = last_game_name[0] != '\0' && strlen(name) < sizeof(last_game_name) &&
        strlen(dir) < sizeof(last_game_dir);
}

// True if the last game's folder can be saved in now
static bool last_game_usable() {
    if (!last_game_known) {
        return false;
    }
    if (last_game_drive == 1 && !sd_card_mounted) {
        return false;
    }
    if (last_game_dir[0] == '\0') {
        return true;
    }
    return fsops::kind(fsops::drive_of(last_game_drive), last_game_dir) == fsops::KIND_DIR;
}

// The last game is kept in LASTGAME_FILE in the internal flash, so L-Reload and the
// Save screen's name are still there after power off.  It is only written when
// something changed.
static const char LASTGAME_FILE[] = "lastgame.cfg";
static uint8_t lastgame_stored[extras::LG_FILE_MAX];
static size_t lastgame_stored_len = 0;

static void persist_last_game() {
    static extras::LastGame g;
    g = extras::LastGame();
    g.reload_known = reload_known;
    g.reload_drive = reload_drive;
    snprintf(g.reload_dir, sizeof(g.reload_dir), "%s", reload_dir);
    memcpy(g.reload_name, reload_name, sizeof(g.reload_name));
    g.reload_name[sizeof(g.reload_name) - 1] = '\0';
    if (!reload_known) {
        g.reload_name[0] = g.reload_name[1] = '\0';
    }
    g.last_known = last_game_known;
    g.last_drive = last_game_drive;
    snprintf(g.last_dir, sizeof(g.last_dir), "%s", last_game_dir);
    snprintf(g.last_name, sizeof(g.last_name), "%s", last_game_name);
    g.format = last_format;
    static uint8_t buf[extras::LG_FILE_MAX];
    size_t len = extras::lastgame_encode(g, buf);
    if (len == lastgame_stored_len && memcmp(buf, lastgame_stored, len) == 0) {
        return;
    }
    int file = pico_open(LASTGAME_FILE, LFS_O_CREAT | LFS_O_WRONLY | LFS_O_TRUNC);
    if (file >= 0) {
        int written = pico_write(file, buf, len);
        pico_close(file);
        if (written == (int) len) {
            memcpy(lastgame_stored, buf, len);
            lastgame_stored_len = len;
        }
    }
}

static void load_last_game() {
    static uint8_t buf[extras::LG_FILE_MAX];
    int file = pico_open(LASTGAME_FILE, LFS_O_RDONLY);
    if (file < 0) {
        return;
    }
    int n = pico_read(file, buf, sizeof(buf));
    pico_close(file);
    static extras::LastGame g;
    if (n > 0 && extras::lastgame_decode(buf, (size_t) n, g)) {
        reload_known = g.reload_known;
        reload_drive = g.reload_drive;
        snprintf(reload_dir, sizeof(reload_dir), "%s", g.reload_dir);
        memcpy(reload_name, g.reload_name, sizeof(g.reload_name) < sizeof(reload_name) ? sizeof(g.reload_name) : sizeof(reload_name));
        last_game_known = g.last_known;
        last_game_drive = g.last_drive;
        snprintf(last_game_dir, sizeof(last_game_dir), "%s", g.last_dir);
        snprintf(last_game_name, sizeof(last_game_name), "%s", g.last_name);
        last_format = g.format;
        memcpy(lastgame_stored, buf, (size_t) n);
        lastgame_stored_len = (size_t) n;
        LOG("Last game restored: %s\n", last_game_name);
    }
}

// The games played recently (E on the menu), kept in RECENT_FILE in the internal flash.
// Each load reads the file, puts the game first and writes it again if that changed
// anything. The list (about 2 KB) and the file's bytes are made in snap_load_buffer, which
// nothing is using when a game is about to be loaded (the load fills it afterwards).
static const char RECENT_FILE[] = "recent.cfg";
static const uint32_t RECENT_LIST_OFFSET = 65536;
static const uint32_t RECENT_FILE_OFFSET = 65536 + 2304;
static_assert(sizeof(extras::RecentList) <= 2304, "the list does not fit before the file's bytes");
static_assert(RECENT_FILE_OFFSET + extras::RECENT_FILE_MAX <= SNAP_LOAD_BUFFER_SIZE, "no room for the recent games");

static extras::RecentList* recent_read() {
    extras::RecentList* list = new (snap_load_buffer + RECENT_LIST_OFFSET) extras::RecentList();
    uint8_t* bytes = reinterpret_cast<uint8_t*>(snap_load_buffer) + RECENT_FILE_OFFSET;
    int file = pico_open(RECENT_FILE, LFS_O_RDONLY);
    if (file >= 0) {
        int n = pico_read(file, bytes, extras::RECENT_FILE_MAX);
        pico_close(file);
        if (n > 0) {
            list->decode(bytes, (size_t) n);
        }
    }
    return list;
}

static void recent_note(uint8_t drive, const char* dir, const char* name, const char* ext) {
    extras::RecentList* list = recent_read();
    if (!list->add(drive, dir, name, ext)) {
        return;
    }
    uint8_t* bytes = reinterpret_cast<uint8_t*>(snap_load_buffer) + RECENT_FILE_OFFSET;
    size_t len = list->encode(bytes);
    int file = pico_open(RECENT_FILE, LFS_O_CREAT | LFS_O_WRONLY | LFS_O_TRUNC);
    if (file >= 0) {
        pico_write(file, bytes, len);
        pico_close(file);
    }
}

void load_snapshot_file(uint8_t drive, const char* dir, const char* fileAndExt) {
    game_running = true;
    // a snapshot expects the internal ROM; stop any earlier tape trap
    tape_service_stop();
    snap_is_paged = false;

    // fileAndExt is the name and the extension as two strings, one after the
    // other.  On the SD card the file is in directory dir (same form as sd_cwd).
    static char filename[BROWSE_PATH_MAX];
    const char* ext = fileAndExt + strlen(fileAndExt) + 1;
    snprintf(filename, sizeof(filename), "%s/%s.%s", dir, fileAndExt, ext);
    remember_last_game(drive, dir, fileAndExt);
    {
        size_t len = strlen(fileAndExt) + 1;
        len += strlen(ext) + 1;
        if (len <= sizeof(reload_name) && strlen(dir) < sizeof(reload_dir)) {
            memmove(reload_name, fileAndExt, len);
            memmove(reload_dir, dir, strlen(dir) + 1);
            reload_drive = drive == 1 ? 1 : 0;
            reload_known = true;
        }
        last_format = strcasecmp(ext, "sna") == 0 ? ACTION_SNA_SAVE_TYPE_SNA :
            strcasecmp(ext, "z80") == 0 ? ACTION_SNA_SAVE_TYPE_Z80 :
            strcasecmp(ext, "szx") == 0 ? ACTION_SNA_SAVE_TYPE_SZX : ACTION_SNA_SAVE_TYPE_AUTO;
    }
    persist_last_game();
    recent_note(drive, dir, fileAndExt, ext);

    if (strcasecmp(ext, "tap") == 0 || strcasecmp(ext, "tzx") == 0) {
        // First try to convert the tape into a 48K snapshot in snap_load_buffer;
        // the normal snapshot loading path then sends it to the Spectrum.
        bool clean = build_snapshot_from_tap(drive, filename);
        if (!clean && tape_service_start(drive, filename)) {
            // Needs the real loader: reset the Spectrum into a ROM that gets
            // its tape blocks from the Pico.  The user then types LOAD "".
            LOG("Tape: using ROM trap, LOAD \"\" is typed after the reset\n");
            tap_make_reset_snapshot();
        }
        return;
    }

    LOG("File name and extension: %s\n", filename);

    FileHandle file;
    if (!file.open(drive == 1 ? Drive::EXTERNAL_SD_CARD : Drive::INTERNAL_FLASH, filename, true)) {
        LOG("Could not open %s\n", filename);
        set_last_error("Cannot open", fileAndExt);
        tap_make_reset_snapshot();      // nothing to load, just reset the Spectrum
        return;
    }

    // 48K snapshots are read exactly as they are in the file.  A .z80 file is
    // decoded, and a .sna file as big as a 128K one is read as one (snapload.h
    // describes what ends up in the buffer).
    uint8_t* buf = reinterpret_cast<uint8_t*>(snap_load_buffer);
    snapload::Kind kind;
    int32_t fileSize = file.size();
    if (strcasecmp(ext, "z80") == 0) {
        kind = snapload::load_z80(file, fileSize, buf, snap_io_buffer, sizeof(snap_io_buffer));
    }
    else if (strcasecmp(ext, "szx") == 0) {
        kind = snapload::load_szx(file, buf, snap_io_buffer, sizeof(snap_io_buffer));
    }
    else if (fileSize >= 0 && snapload::is_sna128((uint32_t) fileSize)) {
        kind = snapload::load_sna128(file, buf, snap_io_buffer, sizeof(snap_io_buffer));
    }
    else {
        file.read(snap_load_buffer, SNA_FILE_SIZE);
        kind = snapload::KIND_SNA48;
    }
    file.close();

    if (kind == snapload::KIND_ERROR) {
        LOG("%s is not a snapshot that can be loaded\n", filename);
        set_last_error("Cannot load", fileAndExt);
        tap_make_reset_snapshot();
        return;
    }
    if (kind == snapload::KIND_PAGED) {
        LOG("128K snapshot, PC = %02X%02X, port 7FFD = %02X, %s\n",
            buf[7], buf[6], buf[snapload::X_7FFD],
            buf[snapload::X_AYFLAG] ? "with sound chip registers" : "no sound chip registers");
        snap_is_paged = true;
    }
    current_snap_size = kind == snapload::KIND_Z80V1 ? Z80_FILE_SIZE :
        kind == snapload::KIND_PAGED ? snapload::IMAGE128_SIZE : SNA_FILE_SIZE;
    current_snap_data = snap_load_buffer;
}

bool stringendswith(const char* str, const char* suffix) {
    int suffixlen = strlen(suffix);
    int len = strlen(str);
    if (suffixlen > len) {
        return false;
    }
    return strcasecmp(str + len - suffixlen, suffix) == 0;
}


inline bool is_snapshot_ext(const char* extension) {
    return (strcasecmp(extension, "sna") == 0) ||
        (strcasecmp(extension, "z80") == 0) ||
        (strcasecmp(extension, "szx") == 0) ||
        (strcasecmp(extension, "tap") == 0) || // Natively allow raw .TAP files to be listed
        (strcasecmp(extension, "tzx") == 0);   // and .TZX
}

// The files the browser lists: snapshots, tapes, ROM images and poke lists (which are copied,
// moved and so on like the rest; a ROM is used with Change ROM, a poke list with Poke)
// L-All in Browse Games: hidden folders, the Poke List and Screen Grabs folders and .scr files
// are listed too (the ROM asks with bit 2 of the list request's flags)
static bool list_all = false;

inline bool is_listed_ext(const char* extension) {
    return is_snapshot_ext(extension) || strcasecmp(extension, "rom") == 0 || strcasecmp(extension, "pok") == 0 ||
        (list_all && strcasecmp(extension, "scr") == 0);
}

const char* split_filename_and_get_ext(char* name) {
    char* ext = nullptr;
    char* pch = name;
    while (*pch != '\0') {
        if (*pch == '.')
            ext = pch;
        pch++;
    }
    if (ext == nullptr)
        return pch;
    *ext = '\0';
    return ext + 1;
}

bool add_file_to_list(const char* name, const uint16_t destoffset, uint16_t& stringoffset) {
    uint8_t* nmi_rom_data = rom_data + 16384;
    int index = nmi_rom_data[destoffset];
    if (index >= 10) {
        nmi_rom_data[destoffset + 1] = 1;
        return false;
    }
    nmi_rom_data[destoffset] = index + 1;
    nmi_rom_data[destoffset + 2 + 2 * index] = stringoffset & 0xFF;
    nmi_rom_data[destoffset + 2 + 2 * index + 1] = (stringoffset >> 8) & 0xFF;
    int filenamelen = strlen(name) + 1;
    filenamelen += strlen(name + filenamelen) + 1;
    memcpy(nmi_rom_data + stringoffset, name, filenamelen);
    stringoffset += filenamelen;
    return true;
}



// Fills in the line that tells the user which directory is being listed.
// dest is SNAPLIST_PATH_SIZE bytes; nothing is shown for the internal flash.
static void format_path(uint8_t drive, const char* dir, uint8_t* dest);

static void write_browse_path(uint8_t* dest, uint8_t drive) {
    format_path(drive, cwd_of(drive), dest);
}

// Writes a directory of a drive (in the form of sd_cwd) as a string that fits a line of
// the NMI menu: "SD:/GAMES" or "Flash:/GAMES".  dest is SNAPLIST_PATH_SIZE bytes.
static void format_path(uint8_t drive, const char* dir, uint8_t* dest) {
    memset(dest, 0, SNAPLIST_PATH_SIZE);
    const int maxlen = 30;      // what fits on a line of the menu
    const char* label = drive == 1 ? "SD:" : "Flash:";
    const int labellen = (int) strlen(label);
    char full[sizeof(sd_cwd) + 8];
    int len = snprintf(full, sizeof(full), "%s%s", label, dir[0] != '\0' ? dir : "/");
    const char* src = full;
    if (len > maxlen) {
        // Too long: keep the end of the path, starting at a whole name if we can
        const int prefixlen = labellen + 2;
        src = full + len - (maxlen - prefixlen);
        const char* slash = strchr(src, '/');
        if (slash != nullptr) {
            src = slash;
        }
        memcpy(dest, label, labellen);
        memcpy(dest + labellen, "..", 2);
        dest += prefixlen;
    }
    for (; *src != '\0'; src++) {
        // the NMI rom's font only has the printable ASCII characters
        *dest++ = (*src >= 32 && *src <= 126) ? *src : '?';
    }
}


// True if the name contains filter, ignoring case.  Everything matches an
// empty filter.
static bool matches_snap_filter(const char* name, const char* filter) {
    if (filter[0] == '\0') {
        return true;
    }
    for (; *name != '\0'; name++) {
        const char* a = name;
        const char* b = filter;
        while (*b != '\0' && tolower((unsigned char) *a) == tolower((unsigned char) *b)) {
            a++;
            b++;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

// Sub directories that are shown.  Hidden ones (and names starting with '.') are not, unless
// L-All is on; "." and ".." never are.
static bool is_listable_dir(DirHandle& dir) {
    const char* n = dir.fileName();
    if (!dir.isDirectory() || strcmp(n, ".") == 0 || strcmp(n, "..") == 0) return false;
    return list_all || (!dir.isHidden() && n[0] != '.');
}

// A directory is returned with a '/' on the end of its name and an empty
// extension
static void make_dir_entry(char* fname) {
    size_t len = strlen(fname);
    fname[len] = '/';
    fname[len + 1] = '\0';
    fname[len + 2] = '\0';
}

// Room for a name, a '/' and two terminators
static char list_fname[260];


//---------------------------------------------
// Snapshot browser listing
//
// A listing (one directory, or a search) is produced by a walker that reads
// the card a bit at a time and can stop and later carry on where it left
// off, so each page only reads the entries the page needs.  The results it
// has produced are also kept in list_pool, so going back a page reads
// nothing at all.  Altogether the card is read once per listing, not once
// per page.  The walker is thrown away when the listing changes (another
// directory, search or drive) and when the NMI menu is entered.

// Results cache.  A record is a 2 byte pool offset of the result's directory
// followed by its name and extension.  Directory strings are stored in the
// pool too, once each time the directory changes.
static const size_t LIST_POOL_SIZE = 32 * 1024;
static const int LIST_CACHE_MAX = 1500;
static char list_pool[LIST_POOL_SIZE];
static size_t list_pool_used = 0;
static uint16_t list_cache[LIST_CACHE_MAX];    // pool offset of each record
static int list_cached = 0;                    // results 0..list_cached-1 are cached
static bool list_cache_full = false;
static int list_last_dir = -1;                 // pool offset of the newest directory string

static void list_cache_reset() {
    list_cached = 0;
    list_pool_used = 0;
    list_cache_full = false;
    list_last_dir = -1;
}

// Caches the next result.  Once the pool is full the rest are not cached and
// pages beyond what is cached are found by reading the card again.
static void list_cache_add(const char* nameAndExt, const char* dir) {
    if (list_cache_full) {
        return;
    }
    size_t namelen = strlen(nameAndExt) + 1;
    namelen += strlen(nameAndExt + namelen) + 1;
    size_t dirlen = strlen(dir) + 1;
    bool newdir = list_last_dir < 0 || strcmp(list_pool + list_last_dir, dir) != 0;
    size_t need = (newdir ? dirlen : 0) + 2 + namelen;
    if (list_cached >= LIST_CACHE_MAX || list_pool_used + need > LIST_POOL_SIZE) {
        list_cache_full = true;
        return;
    }
    if (newdir) {
        memcpy(list_pool + list_pool_used, dir, dirlen);
        list_last_dir = (int) list_pool_used;
        list_pool_used += dirlen;
    }
    uint16_t diroffset = (uint16_t) list_last_dir;
    memcpy(list_pool + list_pool_used, &diroffset, 2);
    memcpy(list_pool + list_pool_used + 2, nameAndExt, namelen);
    list_cache[list_cached++] = (uint16_t) list_pool_used;
    list_pool_used += 2 + namelen;
}

static const char* list_cache_name(int i) {
    return list_pool + list_cache[i] + 2;
}

static const char* list_cache_dir(int i) {
    uint16_t diroffset;
    memcpy(&diroffset, list_pool + list_cache[i], 2);
    return list_pool + diroffset;
}


// How many levels of directory a search goes down
static const int FIND_MAX_DEPTH = 8;
static DirHandle find_dirs[FIND_MAX_DEPTH];

// How the card is read: one directory (or the flash) in card order, or a search
enum WalkMode {
    WALK_FLASH,             // the root of the internal flash
    WALK_DIRS_THEN_FILES,   // (not used) an SD card directory, sub directories first
    WALK_MIXED,             // an SD card directory, in card order
    WALK_FIND               // a search of an SD card directory and those below it
};

// The order the listing is shown in.  A directory (or the flash) is shown in
// alphabetical order, folders first.  If all of it fits in the results cache it
// is read once and sorted (ORDER_SORTED).  If not, it is a big one: each time the
// pages shown go past what is in memory, the whole directory is read again for
// the next BIG_WINDOW entries in order (ORDER_BIG).  A search shows its results
// as they are found (ORDER_FOUND) and they are sorted when it finishes, if they
// all fit in the cache.
enum ListOrder { ORDER_FOUND, ORDER_SORTED, ORDER_BIG };

enum WalkResult { WALK_FOUND, WALK_END, WALK_OUT_OF_TIME };

static struct {
    bool active = false;
    // what is being listed
    uint8_t drive = 0;
    char cwd[sizeof(sd_cwd)] = "";
    char filter[sizeof(snap_filter)] = "";
    WalkMode mode = WALK_FLASH;
    // where the walk has got to.  find_dirs[0..depth] are open, and depth
    // is -1 once the walk is over.  path is the directory find_dirs[depth]
    // is reading, starting "/sd" on the SD card and "/fl" on the internal flash, so
    // path + 3 is in the same form as sd_cwd.
    int depth = -1;
    ListOrder order = ORDER_FOUND;
    bool sorted = false;    // ORDER_FOUND: the finished search's results have been sorted
    int phase = 0;          // WALK_DIRS_THEN_FILES: 0 for the directories, 1 the snapshots
    bool advance = false;   // move on from the current entry of find_dirs[depth]
    bool descend = false;   // go into the directory that is the current entry
    int produced = 0;       // results produced since the walk started
    bool stopped = false;   // the user stopped the search: read no more
    char path[sizeof(sd_cwd) + 4] = "";
} walk;

// How long one ACTION_SNAP_LIST may spend reading the card.  If the page is
// not full by then, what has been found so far is returned with the busy
// flag set and the NMI rom asks again, so a long search shows its results
// as it goes and can be stopped.
static const uint64_t LIST_TIME_LIMIT_US = 300 * 1000;

// The last result the walk produced, kept because finding out whether there
// is a next page means reading the first result of that page.  Without it,
// paging past the end of the cache would start the walk again every time.
static char walk_last_name[sizeof(list_fname)];
static char walk_last_dir[sizeof(sd_cwd)];
static int walk_last_index = -1;

static void walk_close_dirs() {
    while (walk.depth >= 0) {
        find_dirs[walk.depth--].close();
    }
}

// Goes back to the start of the listing.  The cache is kept.
static void walk_restart() {
    walk_close_dirs();
    walk.phase = 0;
    walk.advance = false;
    walk.descend = false;
    walk.produced = 0;
    walk_last_index = -1;
    snprintf(walk.path, sizeof(walk.path), "%s%s", walk.drive == 1 ? "/sd" : "/fl", walk.cwd);
    if (!walk.stopped && find_dirs[0].open(walk.path)) {
        walk.depth = 0;
    }
}

static void walk_stop() {
    walk_close_dirs();
    walk.active = false;
    walk.stopped = false;
    list_cache_reset();
}

// Counts the entries of a directory, giving up after limit + 1.  Returns -1
// if the directory cannot be read.
static int count_dir_entries(const char* path, int limit) {
    DirHandle& dir = find_dirs[0];
    if (!dir.open(path)) {
        return -1;
    }
    int count = 0;
    while (!dir.atEnd() && count <= limit) {
        count++;
        if (!dir.next())
            break;
    }
    dir.close();
    return count;
}

// The order names are listed in: folders (names ending '/') first, then
// snapshots, each from A to Z not minding case.  Names that differ only in case
// are then in ASCII order, and then by extension, so no two entries of a
// directory are equal.  nameAndExt is the name and the extension as two strings.
static bool is_dir_entry_name(const char* name) {
    size_t len = strlen(name);
    return len > 0 && name[len - 1] == '/';
}

static int compare_entry_names(const char* a, const char* b) {
    bool da = is_dir_entry_name(a), db = is_dir_entry_name(b);
    if (da != db) {
        return da ? -1 : 1;
    }
    int c = strcasecmp(a, b);
    if (c == 0) {
        c = strcmp(a, b);
    }
    if (c == 0) {
        const char* ea = a + strlen(a) + 1;
        const char* eb = b + strlen(b) + 1;
        c = strcasecmp(ea, eb);
        if (c == 0) {
            c = strcmp(ea, eb);
        }
    }
    return c;
}

static int compare_cached(const void* x, const void* y) {
    uint16_t ox, oy;
    memcpy(&ox, x, 2);
    memcpy(&oy, y, 2);
    const char* a = list_pool + ox + 2;
    const char* b = list_pool + oy + 2;
    int c = compare_entry_names(a, b);
    if (c == 0) {
        // the same name in two directories (a search): by directory
        uint16_t da, db;
        memcpy(&da, list_pool + ox, 2);
        memcpy(&db, list_pool + oy, 2);
        c = strcasecmp(list_pool + da, list_pool + db);
    }
    return c;
}

static void list_cache_sort() {
    qsort(list_cache, list_cached, sizeof(list_cache[0]), compare_cached);
}

static WalkResult walk_next(char* fname, const char** dir, uint64_t deadline);

// Reads the whole of the directory being listed into the cache and sorts it.
// Returns false if it does not all fit.
static bool walk_load_all() {
    walk_restart();
    const char* dir;
    while (walk_next(list_fname, &dir, UINT64_MAX) == WALK_FOUND) {
        list_cache_add(list_fname, dir);
        if (list_cache_full) {
            walk_close_dirs();
            return false;
        }
    }
    list_cache_sort();
    walk.produced = list_cached;    // the walk is over and everything is cached
    return true;
}

// ORDER_BIG.  The window holds entries big_start .. big_start + big_count - 1 of
// the listing, in order, each in a slot of list_pool (which the cache does not
// use for a big directory).
static const int BIG_WINDOW = 100;
static const size_t BIG_SLOT = sizeof(list_fname) + 4;
static_assert(BIG_WINDOW * BIG_SLOT <= LIST_POOL_SIZE, "the window fits in list_pool");
static uint8_t big_order[BIG_WINDOW];      // the slot of each entry of the window, in order
static int big_count = 0;
static int big_start = 0;
static bool big_last = false;             // the window ends at the end of the listing
static bool big_valid = false;
static char big_bound[sizeof(list_fname)];

static char* big_slot(int slot) {
    return list_pool + slot * BIG_SLOT;
}

static const char* big_entry(int i) {      // i counts from the start of the window
    return big_slot(big_order[i]);
}

static void copy_entry_name(char* to, const char* nameAndExt) {
    size_t len = strlen(nameAndExt) + 1;
    len += strlen(nameAndExt + len) + 1;
    memcpy(to, nameAndExt, len);
}

// Puts name in the window at its place, which must be < big_count when full
static void big_insert(const char* name, int slot) {
    int lo = 0, hi = big_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (compare_entry_names(name, big_entry(mid)) < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    memmove(big_order + lo + 1, big_order + lo, big_count - lo);
    big_order[lo] = (uint8_t) slot;
    copy_entry_name(big_slot(slot), name);
    big_count++;
}

// Reads the directory and keeps the want entries that come first after
// big_bound (or from the start if after is false).
static void big_fill_after(bool after, int want) {
    big_count = 0;
    walk_restart();
    const char* dir;
    while (walk_next(list_fname, &dir, UINT64_MAX) == WALK_FOUND) {
        if (after && compare_entry_names(list_fname, big_bound) <= 0) {
            continue;
        }
        int slot = big_count;
        if (big_count == want) {
            if (compare_entry_names(list_fname, big_entry(big_count - 1)) >= 0) {
                continue;
            }
            slot = big_order[--big_count];      // the last one goes
        }
        big_insert(list_fname, slot);
    }
    big_last = big_count < want;
}

// Reads the directory and keeps the want entries that come last before big_bound
static void big_fill_before(int want) {
    big_count = 0;
    walk_restart();
    const char* dir;
    while (walk_next(list_fname, &dir, UINT64_MAX) == WALK_FOUND) {
        if (compare_entry_names(list_fname, big_bound) >= 0) {
            continue;
        }
        int slot = big_count;
        if (big_count == want) {
            if (compare_entry_names(list_fname, big_entry(0)) <= 0) {
                continue;
            }
            slot = big_order[0];                // the first one goes
            memmove(big_order, big_order + 1, --big_count);
        }
        big_insert(list_fname, slot);
    }
}

// Makes the window hold entries start .. start + SNAP_LIST_MAX (a page and the
// one after it), or as many of them as there are
static void big_cover(int start) {
    if (!big_valid || (start == 0 && big_start != 0)) {
        big_fill_after(false, BIG_WINDOW);
        big_start = 0;
        big_valid = true;
    }
    while (start < big_start) {
        // back: the entries before the window, then the window's first entry
        copy_entry_name(big_bound, big_entry(0));
        int old_start = big_start;
        bool only_last = big_last && big_count == 1;
        big_fill_before(BIG_WINDOW - 1);
        big_insert(big_bound, big_count);
        big_start = old_start - (big_count - 1);
        if (big_start < 0) {
            big_start = 0;          // the directory changed under us
        }
        big_last = only_last;
    }
    while (start + SNAP_LIST_MAX >= big_start + big_count && !big_last) {
        // on: from start, or from the end of the window if start is past it
        int from = start >= big_start && start < big_start + big_count ? start : big_start + big_count;
        if (from == big_start) {
            big_fill_after(false, BIG_WINDOW);   // can only be the start of the listing
            big_start = 0;
            continue;
        }
        copy_entry_name(big_bound, big_entry(from - 1 - big_start));
        big_fill_after(true, BIG_WINDOW);
        big_start = from;
    }
}

// Starts a new listing of the drive, the directory being browsed and the
// search in force
static void walk_begin(uint8_t drive) {
    walk_stop();
    walk.active = true;
    walk.drive = drive;
    snprintf(walk.filter, sizeof(walk.filter), "%s", snap_filter);
    walk.cwd[0] = '\0';
    walk.mode = WALK_FLASH;
    walk.order = snap_filter[0] != '\0' ? ORDER_FOUND : ORDER_SORTED;
    walk.sorted = false;
    big_valid = false;

    {
        char* cwd = cwd_of(drive);
        char path[sizeof(sd_cwd) + 4];
        snprintf(path, sizeof(path), "%s%s", drive == 1 ? "/sd" : "/fl", cwd);
        int entries = count_dir_entries(path, 0);
        if (entries < 0 && cwd[0] != '\0') {
            // The directory has gone (a different card?) so return to the root
            LOG("Cannot open %s, returning to the root\n", path);
            cwd[0] = '\0';
        }
        snprintf(walk.cwd, sizeof(walk.cwd), "%s", cwd);
        walk.mode = snap_filter[0] != '\0' ? WALK_FIND : WALK_MIXED;
    }
    if (walk.order == ORDER_SORTED) {
        if (!walk_load_all()) {
            LOG("%s/ is too big to sort in memory: it is read again every %d entries\n", walk.cwd, BIG_WINDOW);
            list_cache_reset();
            walk.order = ORDER_BIG;
        }
        return;
    }
    walk_restart();
}

static bool walk_is_for(uint8_t drive) {
    return walk.active && walk.drive == drive &&
        strcmp(walk.cwd, cwd_of(drive)) == 0 &&
        strcmp(walk.filter, snap_filter) == 0;
}

// Goes into the directory that is the current entry of find_dirs[depth],
// unless that is too deep or would make too long a path
static void walk_descend() {
    const char* name = find_dirs[walk.depth].fileName();
    size_t len = strlen(name);
    size_t pathlen = strlen(walk.path);
    if (walk.depth + 1 < FIND_MAX_DEPTH && (pathlen - 3) + 1 + len < sizeof(sd_cwd)) {
        walk.path[pathlen] = '/';
        memcpy(walk.path + pathlen + 1, name, len + 1);
        bool opened = walk.drive == 1 ? find_dirs[walk.depth + 1].openSubdir(find_dirs[walk.depth]) :
            find_dirs[walk.depth + 1].open(walk.path);
        if (opened) {
            walk.depth++;
            walk.advance = false;   // open() has read the first entry
        }
        else {
            walk.path[pathlen] = '\0';
        }
    }
}

// Finds the next result of the listing.  fname is set to the name and
// extension, as two strings, and *dir to the directory it is in (in the form
// of sd_cwd), which is only good until the next call.  Gives up at the time
// deadline, leaving the walk ready to carry on.
static WalkResult walk_next(char* fname, const char** dir, uint64_t deadline) {
    while (walk.depth >= 0) {
        if (time_us_64() >= deadline) {
            return WALK_OUT_OF_TIME;
        }
        DirHandle& d = find_dirs[walk.depth];
        if (walk.descend) {
            walk.descend = false;
            walk_descend();
            continue;
        }
        if ((walk.advance && !d.next()) || d.atEnd()) {
            d.close();
            if (walk.mode == WALK_DIRS_THEN_FILES && walk.phase == 0) {
                // the directories are done, go through again for the snapshots
                walk.phase = 1;
                walk.advance = false;
                if (!d.open(walk.path)) {
                    walk.depth = -1;
                }
                continue;
            }
            // finished with this directory, carry on with the one above
            walk.depth--;
            char* slash = strrchr(walk.path + 3, '/');
            if (slash != nullptr) {
                *slash = '\0';
            }
            walk.advance = true;
            continue;
        }
        walk.advance = true;

        strcpy(fname, d.fileName());
        *dir = walk.path + 3;
        if (!d.isDirectory()) {
            if (walk.mode == WALK_DIRS_THEN_FILES && walk.phase == 0) {
                continue;
            }
            if (is_listed_ext(split_filename_and_get_ext(fname)) &&
                matches_snap_filter(fname, walk.filter)) {
                return WALK_FOUND;
            }
            continue;
        }
        if ((walk.mode == WALK_DIRS_THEN_FILES && walk.phase == 1) || !is_listable_dir(d)) {
            continue;
        }
        if (walk.drive == 1 && (*dir)[0] == '\0' && !list_all &&
                (strcasecmp(fname, "Poke List") == 0 || strcasecmp(fname, "Screen Grabs") == 0)) {
            continue;           // Poke List: only a PC puts files there; Screen Grabs: V shows them. Browse Games does not list either
        }
        if (walk.mode == WALK_FIND) {
            walk.descend = true;    // look inside it before moving on
            if (!matches_snap_filter(fname, walk.filter)) {
                continue;
            }
        }
        make_dir_entry(fname);
        return WALK_FOUND;
    }
    return WALK_END;
}

// Adds a result to the list being returned to the NMI rom.  Returns false if
// the list already holds a full page, in which case the "more" flag is set.
// The longest name sent to the NMI rom, which shows it a window's width at
// a time and scrolls it
static const size_t SHOWN_NAME_MAX = SNAPLIST_NAME_MAX;

// Makes the name the NMI rom shows: a long name keeps its start and its end
// (where TOSEC style names differ, "[a]", "(Side B)") with ".." between, and
// a directory keeps the '/' on the end that marks it.  Characters the NMI
// rom's font does not have become '?'.  The extension is copied as it is.
// Copies name to d cut down to max characters, keeping its start and end
// with ".." between.  Returns the end of what was written (not terminated).
static char* shorten_name(const char* name, char* d, size_t max) {
    size_t len = strlen(name);
    char* start = d;
    if (len <= max) {
        memcpy(d, name, len);
        d += len;
    }
    else {
        const size_t tail = max / 3;
        const size_t head = max - tail - 2;
        memcpy(d, name, head);
        d += head;
        *d++ = '.';
        *d++ = '.';
        memcpy(d, name + len - tail, tail);
        d += tail;
    }
    for (char* c = start; c < d; c++) {
        if (!(*c >= 32 && *c <= 126)) {
            *c = '?';
        }
    }
    return d;
}

static void make_shown_name(const char* nameAndExt, char* shown) {
    const char* ext = nameAndExt + strlen(nameAndExt) + 1;
    char* d = shorten_name(nameAndExt, shown, SHOWN_NAME_MAX);
    *d++ = '\0';
    strcpy(d, ext);
}

// ACTION_SNAP_LIST flag bit 1: send each file's size and whether it needs a 128K
// machine too, as a third string after the name and extension.
static bool list_details = false;

// The size and 128K flag of a file, as extras::describe_file makes them
static void read_file_details(uint8_t drive, const char* dir, const char* nameAndExt, char out[9]) {
    const char* ext = nameAndExt + strlen(nameAndExt) + 1;
    static char path[BROWSE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.%s", dir, nameAndExt, ext);
    FileHandle f;
    if (!f.open(drive == 1 ? Drive::EXTERNAL_SD_CARD : Drive::INTERNAL_FLASH, path, true)) {
        snprintf(out, 9, "%s", "   ?    ");
        return;
    }
    int32_t size = f.size();
    uint8_t head[64];
    int got = f.read(head, sizeof(head));
    f.close();
    extras::Needs needs = extras::snapshot_needs(ext, size < 0 ? 0 : (uint32_t) size, head, got < 0 ? 0 : (size_t) got);
    extras::describe_file(needs, size < 0 ? 0 : (uint32_t) size, out);
}

static bool list_out(const char* nameAndExt, const char* dir, uint16_t destoffset, uint16_t& stringoffset) {
    char shown[SHOWN_NAME_MAX + 1 + 260];
    make_shown_name(nameAndExt, shown);
    uint16_t offset = stringoffset;
    if (!add_file_to_list(shown, destoffset, stringoffset)) {
        return false;
    }
    if (list_details && nameAndExt[strlen(nameAndExt) + 1] != '\0') {
        char info[9];
        read_file_details(list_drive, dir, nameAndExt, info);
        memcpy(reinterpret_cast<char*>(rom_data + 16384) + stringoffset, info, strlen(info) + 1);
        stringoffset += strlen(info) + 1;
    }
    if (list_entry_count < SNAP_LIST_MAX) {
        list_entry_offset[list_entry_count] = offset;
        snprintf(list_entry_dir[list_entry_count], sizeof(list_entry_dir[0]), "%s", dir);
        size_t len = strlen(nameAndExt) + 1;
        len += strlen(nameAndExt + len) + 1;
        if (len <= sizeof(list_entry_name[0])) {
            memcpy(list_entry_name[list_entry_count], nameAndExt, len);
        }
        list_entry_count++;
    }
    return true;
}


void nmi_action_list_sna() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t start =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
    uint8_t drive = nmi_rom_data[4];
    uint16_t destoffset =  (nmi_rom_data[6] << 8) |  nmi_rom_data[5];
    bool stop = (nmi_rom_data[7] & 1) != 0;
    list_details = (nmi_rom_data[7] & 2) != 0;
    bool all = (nmi_rom_data[7] & 4) != 0;
    if (all != list_all) {
        list_all = all;
        walk_stop();                // what is listed has changed: read the folder again
    }
    list_drive = drive == 1 ? 1 : 0;
    uint16_t stringoffset = destoffset + SNAPLIST_STRINGS;
    uint64_t deadline = time_us_64() + LIST_TIME_LIMIT_US;

    LOG("List snaps for drive %d in %s/ matching \"%s\", starting at %d and storing at %X%s\n",
        drive, cwd_of(drive), snap_filter, start, destoffset, stop ? ", stop" : "");

    nmi_rom_data[destoffset] = 0;
    nmi_rom_data[destoffset + 1] = 0;
    list_entry_count = 0;

    if (!walk_is_for(drive)) {
        walk_begin(drive);
    }
    if (stop) {
        // the user has seen enough: keep what has been found and read no more
        walk_close_dirs();
        walk.stopped = true;
    }
    bool busy = false;

    // A page is SNAP_LIST_MAX results, and one more tells us there is a next page
    int need = start + SNAP_LIST_MAX + 1;
    int out = start;

    if (walk.order == ORDER_BIG) {
        big_cover(start);
        const char* dir = walk.cwd;
        for (; out < need && out < big_start + big_count; out++) {
            list_out(big_entry(out - big_start), dir, destoffset, stringoffset);
        }
        nmi_rom_data[destoffset + SNAPLIST_HASPARENT] = cwd_of(drive)[0] != '\0' ? 1 : 0;
        nmi_rom_data[destoffset + SNAPLIST_BUSY] = 0;
        write_browse_path(nmi_rom_data + destoffset + SNAPLIST_PATH, drive);
        return;
    }

    // First the results that are cached...
    while (out < need && out < list_cached) {
        list_out(list_cache_name(out), list_cache_dir(out), destoffset, stringoffset);
        out++;
    }

    // ... then the rest from the walk, which carries on from where it got
    // to.  It only has to go back to the start if it has already passed
    // results that we need and that did not fit in the cache.
    if (out < need && walk.produced == out + 1 && walk_last_index == out) {
        list_out(walk_last_name, walk_last_dir, destoffset, stringoffset);
        out++;
    }
    if (out < need && walk.produced > out) {
        walk_restart();
    }
    // A search is read to its end (as time allows, the NMI rom asking again while
    // it is busy) so its results can be sorted, unless there are too many to keep
    bool to_the_end = walk.order == ORDER_FOUND && !walk.sorted;
    while (out < need || (to_the_end && !list_cache_full && walk.depth >= 0)) {
        const char* dir;
        WalkResult r = walk_next(list_fname, &dir, deadline);
        if (r == WALK_OUT_OF_TIME) {
            busy = true;
            break;
        }
        if (r == WALK_END) {
            break;
        }
        int index = walk.produced++;
        if (index == list_cached) {
            list_cache_add(list_fname, dir);
        }
        if (index >= list_cached) {
            size_t len = strlen(list_fname) + 1;
            len += strlen(list_fname + len) + 1;
            memcpy(walk_last_name, list_fname, len);
            snprintf(walk_last_dir, sizeof(walk_last_dir), "%s", dir);
            walk_last_index = index;
        }
        if (index == out && out < need) {
            list_out(list_fname, dir, destoffset, stringoffset);
            out++;
        }
    }

    // A search that has finished (or been stopped) with all its results in the
    // cache: sort them, and send this page again in that order
    if (!busy && walk.order == ORDER_FOUND && walk.depth < 0 && !walk.sorted &&
        !list_cache_full && walk.produced == list_cached) {
        list_cache_sort();
        walk.sorted = true;
        nmi_rom_data[destoffset] = 0;
        nmi_rom_data[destoffset + 1] = 0;
        list_entry_count = 0;
        stringoffset = destoffset + SNAPLIST_STRINGS;
        for (out = start; out < need && out < list_cached; out++) {
            list_out(list_cache_name(out), list_cache_dir(out), destoffset, stringoffset);
        }
    }

    nmi_rom_data[destoffset + SNAPLIST_HASPARENT] = cwd_of(drive)[0] != '\0' ? 1 : 0;
    nmi_rom_data[destoffset + SNAPLIST_BUSY] = busy ? 1 : 0;
    write_browse_path(nmi_rom_data + destoffset + SNAPLIST_PATH, drive);
}


// Moves the browser of a drive into a sub directory, given a name as returned
// by ACTION_SNAP_LIST (with its trailing '/'), or back to the parent
// directory for "..".  This also ends any search.
void nmi_action_snap_chdir() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t nameoffset =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
    uint8_t drive = nmi_rom_data[4];
    const char* name = list_entry_name_for(nameoffset,
        reinterpret_cast<const char*>(nmi_rom_data) + nameoffset);

    snap_filter[0] = '\0';
    char* cwd = cwd_of(drive);

    size_t namelen = strlen(name);
    while (namelen > 0 && name[namelen - 1] == '/') {
        namelen--;
    }
    // a directory found by a search can be anywhere below the current one
    const char* base = list_entry_dir_for(nameoffset);
    size_t baselen = strlen(base);

    if (namelen == 2 && name[0] == '.' && name[1] == '.') {
        char* slash = strrchr(cwd, '/');     // "/A/B" -> "/A" and "/A" -> ""
        if (slash != nullptr) {
            *slash = '\0';
        }
    }
    else if (namelen == 0 || memchr(name, '/', namelen) != nullptr) {
        LOG("Not a directory name: %s\n", name);
    }
    else if (baselen + 1 + namelen >= sizeof(sd_cwd)) {
        LOG("Path is too long to enter %s\n", name);
    }
    else {
        memmove(cwd, base, baselen);
        cwd[baselen] = '/';
        memcpy(cwd + baselen + 1, name, namelen);
        cwd[baselen + 1 + namelen] = '\0';
    }
    LOG("Directory of drive %d is now %s/\n", drive, cwd);
}


//---------------------------------------------
// Moving and copying files, between directories and between the SD card and the
// internal flash, erasing, renaming and making directories.  The NMI rom picks a
// file from the list, the user goes to another directory or drive and asks for it to
// be moved or copied there.

static bool move_picked = false;
static uint8_t move_from_drive = 0;
static char move_from_dir[sizeof(sd_cwd)];
static char move_name[260];     // name and extension, as two strings

// Status returned by ACTION_SNAP_MOVE and ACTION_SNAP_MKDIR (the first three are
// the values in fsops.h)
static const uint8_t FILEOP_OK = fsops::OK;
static const uint8_t FILEOP_NAME_IN_USE = fsops::NAME_IN_USE;
static const uint8_t FILEOP_FAILED = fsops::FAILED;
static const uint8_t FILEOP_SAME_DIRECTORY = 3;
// ACTION_SNAP_ERASE: the entry is a directory, which is erased only if the user types "ok"
static const uint8_t FILEOP_IS_DIRECTORY = 4;
static const uint8_t FILEOP_NOT_CONFIRMED = 5;
static const uint8_t FILEOP_IS_ROM = 6;
static const uint8_t FILEOP_IS_TESTROM = 8;   // testrom.rom of the internal flash, which T-Test ROM needs
static const uint8_t FILEOP_IS_DIAGROM = 9;   // DiagROM.rom of the internal flash, which T-Test ROM needs too
static const uint8_t FILEOP_IS_PROTECTED = 10; // /Favourites or /Save Games: the texts to show follow the name and the folder
static const uint8_t FILEOP_CANT_MOVE_FOLDER = 11; // the picked folder can't go here: the reason follows as text in the description
static const uint8_t FILEOP_CANCELLED = fsops::CANCELLED;   // the user stopped a folder copy with BREAK
static const uint8_t FILEOP_IS_48ROM = 7;     // 48.rom of the internal flash, which the tapes need

// Copies a file, between drives if need be.  Never overwrites: the copy must be a new
// file.  A copy that fails part way is removed.  The data goes through list_pool,
// which is free once walk_stop() has emptied the list cache.
static uint8_t copy_file(uint8_t from_drive, const char* from, uint8_t to_drive, const char* to) {
    return fsops::copy_file(fsops::drive_of(from_drive), from, fsops::drive_of(to_drive), to,
        reinterpret_cast<uint8_t*>(list_pool), sizeof(list_pool));
}

// Where A (favourite) copies files to, in the same form as sd_cwd, on the file's drive
static const char FAVOURITES_DIR[] = "/Favourites";
static const char SAVE_GAMES_DIR[] = "/Save Games";

static void move_full_path(const char* dir, char* path, size_t size) {
    const char* ext = move_name + strlen(move_name) + 1;
    snprintf(path, size, "%s/%s%s%s", dir, move_name, ext[0] != '\0' ? "." : "", ext);
}

static int file_entry_for(uint8_t drive, uint16_t nameoffset);

// ---- Moving a folder (M, then the folder's number, browse to where it goes, M).  On one drive
// that is a rename of the folder, which is quick whatever is in it, so there is nothing to wait
// for.  A folder does not change drive (that would be a copy of everything in it).

// The picked folder's path now and where it would be here, without a '/' on the end.
// False if either path is too long.
static bool move_folder_paths(const char* here, char* from, size_t from_size, char* to, size_t to_size) {
    size_t len = strlen(move_name);
    while (len > 0 && move_name[len - 1] == '/') {
        len--;
    }
    int nf = snprintf(from, from_size, "%s/%.*s", move_from_dir, (int) len, move_name);
    int nt = snprintf(to, to_size, "%s/%.*s", here, (int) len, move_name);
    return nf > 0 && nf < (int) from_size && nt > 0 && nt < (int) to_size;
}

// The folders the Picoface itself looks for in the root of the card (and the flash)
static bool is_picoface_folder(const char* path) {
    for (int i = 0; i < sdsetup::FOLDER_COUNT; i++) {
        if (strcasecmp(path, sdsetup::FOLDERS[i].path) == 0) {
            return true;
        }
    }
    return false;
}

// Why the picked folder can't be moved into `here`, or nullptr if it can.  from is its path.
static const char* folder_move_problem(uint8_t drive, const char* here, const char* from) {
    if (move_from_drive != drive) {
        return "Folders can't change drive";
    }
    if (strcmp(move_from_dir, here) == 0) {
        return "It is already in this folder";
    }
    size_t n = strlen(from);
    if (strncasecmp(here, from, n) == 0 && (here[n] == '\0' || here[n] == '/')) {
        return "Not into the folder itself";
    }
    if (move_from_dir[0] == '\0' && is_picoface_folder(from)) {
        return "The PicoMenu needs that folder";
    }
    return nullptr;
}

// The paths of the picked folder and the reason it can't go into `here`, if there is one
static const char* move_folder_move_check(uint8_t drive, const char* here, char* from, size_t from_size, char* to, size_t to_size) {
    if (!move_folder_paths(here, from, from_size, to, to_size)) {
        return "That path is too long";
    }
    return folder_move_problem(drive, here, from);
}

// ---- Copying a folder (C, then the folder's number, browse to where it goes, C).  It stays on
// one drive (a copy from the SD card to the flash is not done), asks for "ok" and shows a bar
// while it works.

// Why the picked folder can't be copied into `here`, or nullptr if it can.  from and to are the
// paths of the folder and of its copy.
static const char* folder_copy_problem(uint8_t drive, const char* here, const char* from, const char* to) {
    if (move_from_drive != drive) {
        return "Folders can't change drive";
    }
    if (strcmp(move_from_dir, here) == 0) {
        return "It is already in this folder";
    }
    size_t n = strlen(from);
    if (strncasecmp(here, from, n) == 0 && (here[n] == '\0' || here[n] == '/')) {
        return "Not into the folder itself";
    }
    if (fsops::kind(fsops::drive_of(drive), to) != fsops::KIND_NONE) {
        return "That name is already in use";
    }
    return nullptr;
}

// The pico tells the Spectrum how far it has got in byte 3 (0 to COPY_BAR_CELLS), and the Spectrum
// puts 1 in byte 8 when BREAK is pressed
static const int COPY_BAR_CELLS = 28;
static bool copy_folder_report(void*, uint64_t done, uint64_t total) {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint64_t cells = total == 0 ? 0 : done * COPY_BAR_CELLS / total;
    nmi_rom_data[3] = (uint8_t) (cells > COPY_BAR_CELLS ? COPY_BAR_CELLS : cells);
    return nmi_rom_data[8] == 0;
}

// FatFs clears a new folder's whole cluster in one go and needs memory for it: it is lent the
// list's memory just for that call (the copy's own buffer is the same memory, free between files)
extern "C" { extern unsigned char* ff_pool; extern size_t ff_pool_size; }
static uint8_t copy_folder_make(Drive drive, const char* path) {
    if (drive != Drive::EXTERNAL_SD_CARD) {
        return fsops::make_dir(drive, path);
    }
    ff_pool = reinterpret_cast<unsigned char*>(list_pool);
    ff_pool_size = sizeof(list_pool);
    uint8_t status = fsops::make_dir(drive, path);
    ff_pool = nullptr;
    ff_pool_size = 0;
    return status;
}

static uint8_t copy_folder(uint8_t drive, char* from, size_t from_size, char* to, size_t to_size) {
    static char scratch[BROWSE_PATH_MAX + 8];
    static fsops::TreeCopy job;
    job.drive = fsops::drive_of(drive);
    job.dirs = find_dirs;
    job.max_depth = FIND_MAX_DEPTH;
    job.from = from;
    job.from_size = from_size;
    job.to = to;
    job.to_size = to_size;
    job.scratch = scratch;
    job.scratch_size = sizeof(scratch);
    job.buffer = reinterpret_cast<uint8_t*>(list_pool);
    job.buffer_size = sizeof(list_pool);
    job.make_folder = copy_folder_make;
    job.report = copy_folder_report;
    job.context = nullptr;
    rom_data[16384 + 3] = 0;
    uint8_t status = fsops::copy_tree(job);
    LOG("Copy folder %s to %s (drive %d): %d (%u files, %u folders, %llu bytes)\n", from, to, drive, status,
        (unsigned) job.files, (unsigned) job.folders, (unsigned long long) job.total);
    return status;
}

// "From: <path>" (or "To: ..."), the path cut down to fit a line of the menu
static void put_labelled_path(const char* label, uint8_t drive, const char* dir, uint8_t* dest) {
    uint8_t path[SNAPLIST_PATH_SIZE];
    format_path(drive, dir, path);
    size_t room = SNAPLIST_PATH_SIZE - 2 - strlen(label);
    char shown[SNAPLIST_PATH_SIZE];
    *shorten_name(reinterpret_cast<const char*>(path), shown, room) = '\0';
    snprintf(reinterpret_cast<char*>(dest), SNAPLIST_PATH_SIZE, "%s%s", label, shown);
}

// WORD param1 = for MOVE_OP_PICK the name of a list entry, for
//               MOVE_OP_DESCRIBE where to put the description
// BYTE param2 = the drive being browsed (where a file is moved or copied to)
// BYTE param3 = what to do, one of ACTION_SNAP_MOVE_OP_*
// Status in byte 0, one of FILEOP_*
void nmi_action_snap_move() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t offset =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
    uint8_t drive = nmi_rom_data[4] == 1 ? 1 : 0;
    uint8_t op = nmi_rom_data[5];
    uint8_t status = FILEOP_OK;
    static char from[BROWSE_PATH_MAX];
    static char to[BROWSE_PATH_MAX];
    const char* here = cwd_of(drive);

    if (op == ACTION_SNAP_MOVE_OP_PICK) {
        int i = list_entry_for(offset);
        size_t len = 0;
        if (i >= 0) {
            len = strlen(list_entry_name[i]) + 1;
            len += strlen(list_entry_name[i] + len) + 1;
        }
        if (i < 0 || len > sizeof(move_name)) {
            status = FILEOP_FAILED;
        }
        else {
            memcpy(move_name, list_entry_name[i], len);
            snprintf(move_from_dir, sizeof(move_from_dir), "%s", list_entry_dir[i]);
            move_from_drive = drive;
            move_picked = true;
            LOG("Picked %s in %s/ of drive %d to move\n", move_name, move_from_dir, drive);
        }
    }
    else if (op == ACTION_SNAP_MOVE_OP_DESCRIBE) {
        // three lines for the NMI rom to show: the file, from and to
        uint8_t* dest = nmi_rom_data + offset;
        memset(dest, 0, 7 * SNAPLIST_PATH_SIZE);
        if (move_picked && is_dir_entry_name(move_name)) {
            // a folder: either the reason it can't go here (status FILEOP_CANT_MOVE_FOLDER, the
            // text in the first line) or, for the NMI rom's "type ok" screen (as for erasing a
            // folder), the folder's name and five texts: title, two lines, two more
            const bool copying = nmi_rom_data[6] == 2;      // the Spectrum says which (1 move, 2 copy)
            const char* problem = move_folder_paths(here, from, sizeof(from), to, sizeof(to)) ?
                (copying ? folder_copy_problem(drive, here, from, to) : folder_move_problem(drive, here, from)) :
                "That path is too long";
            if (problem != nullptr) {
                snprintf(reinterpret_cast<char*>(dest), SNAPLIST_PATH_SIZE, "%s", problem);
                status = FILEOP_CANT_MOVE_FOLDER;
            }
            else {
                char shown[SNAPLIST_PATH_SIZE];
                size_t len = strlen(move_name) - 1;
                char bare[sizeof(move_name)];
                snprintf(bare, sizeof(bare), "%.*s", (int) len, move_name);
                *shorten_name(bare, shown, SNAPLIST_PATH_SIZE - 6) = '\0';
                snprintf(reinterpret_cast<char*>(dest), SNAPLIST_PATH_SIZE, "\"%s\"", shown);
                snprintf(reinterpret_cast<char*>(dest) + 2 * SNAPLIST_PATH_SIZE, SNAPLIST_PATH_SIZE, copying ? "Are you sure you want to copy" : "Are you sure you want to move");
                snprintf(reinterpret_cast<char*>(dest) + 3 * SNAPLIST_PATH_SIZE, SNAPLIST_PATH_SIZE, "If so, type ok");
                put_labelled_path("From: ", move_from_drive, move_from_dir, dest + 4 * SNAPLIST_PATH_SIZE);
                put_labelled_path("To: ", drive, here, dest + 5 * SNAPLIST_PATH_SIZE);
                status = FILEOP_IS_DIRECTORY;
            }
        }
        else if (move_picked) {
            char whole[sizeof(move_name) + 1];   // name, '.' and extension
            const char* ext = move_name + strlen(move_name) + 1;
            snprintf(whole, sizeof(whole), "%s%s%s", move_name, ext[0] != '\0' ? "." : "", ext);
            shorten_name(whole, reinterpret_cast<char*>(dest), SNAPLIST_PATH_SIZE - 2);
            format_path(move_from_drive, move_from_dir, dest + SNAPLIST_PATH_SIZE);
            format_path(drive, here, dest + 2 * SNAPLIST_PATH_SIZE);
        }
    }
    else if (op == ACTION_SNAP_MOVE_OP_DO || op == ACTION_SNAP_MOVE_OP_COPY) {
        if (!move_picked) {
            status = FILEOP_FAILED;
        }
        else if (is_dir_entry_name(move_name)) {
            // a folder: moved or copied (on its drive), only after "ok" was typed (the text is where byte 6, 7 point)
            uint16_t typed_at = (nmi_rom_data[7] << 8) | nmi_rom_data[6];
            const char* typed = reinterpret_cast<const char*>(nmi_rom_data) + typed_at;
            const bool copying = op == ACTION_SNAP_MOVE_OP_COPY;
            const char* problem = move_folder_paths(here, from, sizeof(from), to, sizeof(to)) ?
                (copying ? folder_copy_problem(drive, here, from, to) : folder_move_problem(drive, here, from)) :
                "That path is too long";
            if (problem != nullptr) {
                status = strcmp(move_from_dir, here) == 0 && move_from_drive == drive ? FILEOP_SAME_DIRECTORY :
                    strcmp(problem, "That name is already in use") == 0 ? FILEOP_NAME_IN_USE : FILEOP_FAILED;
            }
            else if (typed_at > 16384 - 4 || strncasecmp(typed, "ok", 3) != 0) {
                status = FILEOP_NOT_CONFIRMED;
            }
            else if (copying) {
                walk_stop();        // directories change, and none may be open; the list's memory is the buffer
                status = copy_folder(drive, from, sizeof(from), to, sizeof(to));
                if (status == FILEOP_OK) {
                    move_picked = false;
                }
                else if (status == FILEOP_FAILED) {
                    set_last_error("Cannot copy", move_name);
                }
            }
            else {
                walk_stop();        // directories change, and none may be open
                status = fsops::rename_path(fsops::drive_of(drive), from, to);
                LOG("Move folder %s to %s (drive %d): %d\n", from, to, drive, status);
                if (status == FILEOP_OK) {
                    move_picked = false;
                }
                else if (status == FILEOP_FAILED) {
                    set_last_error("Cannot move", move_name);
                }
            }
        }
        else if (move_from_drive == drive && strcmp(move_from_dir, here) == 0) {
            status = FILEOP_SAME_DIRECTORY;
        }
        else {
            move_full_path(move_from_dir, from, sizeof(from));
            move_full_path(here, to, sizeof(to));
            walk_stop();        // directories change, and none may be open; the cache's memory is used for the copy
            if (op == ACTION_SNAP_MOVE_OP_DO) {
                status = fsops::move_file(fsops::drive_of(move_from_drive), from, fsops::drive_of(drive), to,
                    reinterpret_cast<uint8_t*>(list_pool), sizeof(list_pool));
                LOG("Move %s (drive %d) to %s (drive %d): %d\n", from, move_from_drive, to, drive, status);
            }
            else {
                status = copy_file(move_from_drive, from, drive, to);
                LOG("Copy %s (drive %d) to %s (drive %d): %d\n", from, move_from_drive, to, drive, status);
            }
            if (status == FILEOP_OK) {
                move_picked = false;
            }
            else if (status == FILEOP_FAILED) {
                set_last_error(op == ACTION_SNAP_MOVE_OP_DO ? "Cannot move" : "Cannot copy", move_name);
            }
        }
    }
    else if (op == ACTION_SNAP_MOVE_OP_FAVOURITE) {
        // a copy of the list entry at offset in FAVOURITES_DIR of the same drive, which is
        // made if it is not there
        int i = file_entry_for(drive, offset);
        if (i < 0) {
            status = FILEOP_FAILED;
        }
        else if (strcasecmp(list_entry_dir[i], FAVOURITES_DIR) == 0) {
            status = FILEOP_SAME_DIRECTORY;
        }
        else {
            const char* name = list_entry_name[i];
            const char* ext = name + strlen(name) + 1;
            const char* dot = ext[0] != '\0' ? "." : "";
            int nf = snprintf(from, sizeof(from), "%s/%s%s%s", list_entry_dir[i], name, dot, ext);
            int nt = snprintf(to, sizeof(to), "%s/%s%s%s", FAVOURITES_DIR, name, dot, ext);
            if (nf < 0 || nf >= (int) sizeof(from) || nt < 0 || nt >= (int) sizeof(to)) {
                status = FILEOP_FAILED;     // too long a path
            }
            else {
                walk_stop();        // the list cache's memory is used for the copy
                if (fsops::kind(fsops::drive_of(drive), FAVOURITES_DIR) == fsops::KIND_NONE) {
                    uint8_t made = fsops::make_dir(fsops::drive_of(drive), FAVOURITES_DIR);
                    LOG("Made %s: %d\n", FAVOURITES_DIR, made);
                }
                status = copy_file(drive, from, drive, to);
                LOG("Copy %s to %s: %d\n", from, to, status);
            }
        }
    }
    else {
        move_picked = false;
    }
    nmi_rom_data[0] = status;
}

// Checks a name typed for a new folder or file.  FAT drops spaces and dots
// from the end of a name so they are dropped here too.  Returns false for
// names FAT does not allow, and for names starting '.' which would not be
// listed.
static bool tidy_new_name(char* name) {
    size_t len = strlen(name);
    while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '.')) {
        name[--len] = '\0';
    }
    if (len == 0 || name[0] == '.' || strpbrk(name, "\\/:*?\"<>|") != nullptr) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (name[i] < 32) {
            return false;
        }
    }
    return true;
}

// The list entry the NMI rom asked about, if it is a file.  Directories end with '/'.
// Returns the index or -1.
static int file_entry_for(uint8_t drive, uint16_t nameoffset) {
    int i = list_entry_for(nameoffset);
    if (i < 0) {
        return -1;
    }
    size_t len = strlen(list_entry_name[i]);
    if (len == 0 || list_entry_name[i][len - 1] == '/') {
        return -1;
    }
    return i;
}

// The list entry the NMI rom asked about, whether it is a file or a directory
static int any_entry_for(uint16_t nameoffset) {
    int i = list_entry_for(nameoffset);
    return i >= 0 && list_entry_name[i][0] != '\0' ? i : -1;
}

// The full path of list entry i, with a different name if newname is given
static void entry_path(int i, const char* newname, char* path, size_t size) {
    const char* name = list_entry_name[i];
    const char* ext = name + strlen(name) + 1;
    snprintf(path, size, "%s/%s%s%s", list_entry_dir[i], newname != nullptr ? newname : name,
        ext[0] != '\0' ? "." : "", ext);
}

// The path of list entry i, a directory (its name ends with '/'), without the '/'
static void dir_entry_path(int i, char* path, size_t size) {
    const char* name = list_entry_name[i];
    size_t len = strlen(name);
    while (len > 0 && name[len - 1] == '/') {
        len--;
    }
    snprintf(path, size, "%s/%.*s", list_entry_dir[i], (int) len, name);
}

// "name.ext" of list entry i, cut down to fit max characters, at dest
static void entry_shown_name(int i, char* dest, size_t max) {
    char whole[sizeof(list_entry_name[0]) + 1];
    const char* name = list_entry_name[i];
    const char* ext = name + strlen(name) + 1;
    snprintf(whole, sizeof(whole), "%s%s%s", name, ext[0] != '\0' ? "." : "", ext);
    *shorten_name(whole, dest, max) = '\0';
}

// Renames a file, keeping its extension, or a folder (which has none).  A folder of the Picoface
// itself (in the top of the card or the flash) can't be renamed: it looks for it by its name.
// WORD param1 = the name of a list entry
// BYTE param2 = drive
// WORD param3 = DESCRIBE: where to put two SNAPLIST_PATH_SIZE strings, the
//               name to edit (empty if too long to edit) and "Was: <name>".
//               DO: the new name.
// BYTE param4 = ACTION_SNAP_RENAME_OP_*
// Status in byte 0, one of FILEOP_*; FILEOP_CANT_MOVE_FOLDER (11) with the reason as text at
// param3 (DESCRIBE) when a folder can't be renamed
void nmi_action_snap_rename() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t nameoffset =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
    uint8_t drive = nmi_rom_data[4] == 1 ? 1 : 0;
    uint16_t param =  (nmi_rom_data[6] << 8) |  nmi_rom_data[5];
    uint8_t op = nmi_rom_data[7];
    uint8_t status = FILEOP_FAILED;
    int i = any_entry_for(nameoffset);
    const bool is_dir = i >= 0 && is_dir_entry_name(list_entry_name[i]);
    // the folder's name without its '/'
    char bare[sizeof(list_entry_name[0])];
    if (i >= 0) {
        snprintf(bare, sizeof(bare), "%s", list_entry_name[i]);
        if (is_dir) {
            bare[strlen(bare) - 1] = '\0';
        }
    }
    bool needed = false;
    if (is_dir && list_entry_dir[i][0] == '\0') {
        char top[sizeof(bare) + 1];
        snprintf(top, sizeof(top), "/%s", bare);
        needed = is_picoface_folder(top);
    }

    if (i >= 0 && op == ACTION_SNAP_RENAME_OP_DESCRIBE) {
        char* dest = reinterpret_cast<char*>(nmi_rom_data + param);
        memset(dest, 0, 2 * SNAPLIST_PATH_SIZE);
        if (needed) {
            snprintf(dest, SNAPLIST_PATH_SIZE, "The PicoMenu needs that folder");
            status = FILEOP_CANT_MOVE_FOLDER;
        }
        else {
            bool editable = strlen(bare) <= 28;     // FOLDER_MAXLEN in the NMI rom
            for (const char* c = bare; editable && *c != '\0'; c++) {
                editable = *c >= 32 && *c <= 126;
            }
            if (editable) {
                strcpy(dest, bare);
            }
            memcpy(dest + SNAPLIST_PATH_SIZE, "Was: ", 5);
            entry_shown_name(i, dest + SNAPLIST_PATH_SIZE + 5, SNAPLIST_PATH_SIZE - 2 - 5);
            status = FILEOP_OK;
        }
    }
    else if (i >= 0 && op == ACTION_SNAP_RENAME_OP_DO && !needed) {
        char newname[64];
        snprintf(newname, sizeof(newname), "%s", reinterpret_cast<const char*>(nmi_rom_data) + param);
        static char from[BROWSE_PATH_MAX];
        static char to[BROWSE_PATH_MAX + 64];
        if (tidy_new_name(newname)) {
            if (strcmp(newname, bare) == 0) {
                status = FILEOP_OK;         // nothing to do
            }
            else if (is_dir) {
                dir_entry_path(i, from, sizeof(from));
                int nt = snprintf(to, sizeof(to), "%s/%s", list_entry_dir[i], newname);
                if (nt >= (int) sizeof(sd_cwd)) {
                    status = FILEOP_FAILED;     // too long a path to browse into
                }
                else {
                    walk_stop();
                    status = fsops::rename_path(fsops::drive_of(drive), from, to);
                    LOG("Rename folder %s to %s: %d\n", from, to, status);
                    if (status == FILEOP_FAILED) {
                        set_last_error("Cannot rename", bare);
                    }
                }
            }
            else {
                entry_path(i, nullptr, from, sizeof(from));
                entry_path(i, newname, to, sizeof(to));
                walk_stop();
                status = fsops::rename_path(fsops::drive_of(drive), from, to);
                LOG("Rename %s to %s: %d\n", from, to, status);
                if (status == FILEOP_FAILED) {
                    set_last_error("Cannot rename", list_entry_name[i]);
                }
            }
        }
    }
    nmi_rom_data[0] = status;
}

// How deep a directory tree that is erased can be
static const int ERASE_MAX_DEPTH = FIND_MAX_DEPTH;

// Erases a file, or a directory with everything in it.  A directory is only erased when
// the DO request comes with the text "ok" (typed by the user).
// WORD param1 = the name of a list entry
// BYTE param2 = drive
// WORD param3 = DESCRIBE: where to put two SNAPLIST_PATH_SIZE strings, the
//               file (or directory) and the directory it is in.
//               DO: a directory needs the address of the text the user typed
// BYTE param4 = ACTION_SNAP_ERASE_OP_*
// Status in byte 0, one of FILEOP_*: DESCRIBE gives FILEOP_IS_DIRECTORY for a directory,
// DO gives FILEOP_NOT_CONFIRMED for a directory if the text is not "ok"
void nmi_action_snap_erase() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t nameoffset =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
    uint8_t drive = nmi_rom_data[4] == 1 ? 1 : 0;
    uint16_t param =  (nmi_rom_data[6] << 8) |  nmi_rom_data[5];
    uint8_t op = nmi_rom_data[7];
    uint8_t status = FILEOP_FAILED;
    int i = any_entry_for(nameoffset);
    bool is_dir = i >= 0 && is_dir_entry_name(list_entry_name[i]);
    bool is_rom = i >= 0 && !is_dir && strcasecmp(list_entry_name[i] + strlen(list_entry_name[i]) + 1, "rom") == 0;
    bool is_test = is_rom && drive == 0 && strcasecmp(list_entry_name[i], "testrom") == 0 && list_entry_dir[i][0] == '\0';
    bool is_diag = is_rom && drive == 0 && strcasecmp(list_entry_name[i], "diagrom") == 0 && list_entry_dir[i][0] == '\0';
    bool is_48 = is_rom && drive == 0 && strcasecmp(list_entry_name[i], "48") == 0 && list_entry_dir[i][0] == '\0';

    if (i >= 0 && op == ACTION_SNAP_ERASE_OP_DESCRIBE) {
        char* dest = reinterpret_cast<char*>(nmi_rom_data + param);
        memset(dest, 0, 2 * SNAPLIST_PATH_SIZE);
        entry_shown_name(i, dest, SNAPLIST_PATH_SIZE - 2);
        format_path(drive, list_entry_dir[i], nmi_rom_data + param + SNAPLIST_PATH_SIZE);
        status = is_dir ? FILEOP_IS_DIRECTORY : is_test ? FILEOP_IS_TESTROM : is_diag ? FILEOP_IS_DIAGROM : is_48 ? FILEOP_IS_48ROM : is_rom ? FILEOP_IS_ROM : FILEOP_OK;
        // /Favourites (on either drive) and /Save Games (on the SD card) are used by the menu
        bool favourites = false, saves = false;
        if (is_dir) {
            static char folder[BROWSE_PATH_MAX];
            dir_entry_path(i, folder, sizeof(folder));
            favourites = strcasecmp(folder, FAVOURITES_DIR) == 0;
            saves = drive == 1 && strcasecmp(folder, SAVE_GAMES_DIR) == 0;
            if (favourites || saves) {
                status = FILEOP_IS_PROTECTED;
            }
        }
        // a folder, a ROM file and those two need "ok" typed: the ROM shows these five texts
        // (title, two lines, two more) which follow the name and the folder
        if (status != FILEOP_OK) {
            static const char* const TEXTS[][5] = {
                { "Erase folder", "Directories can contain other", "directories and files, if you", "want to proceed type \"ok\"", "" },
                { "Erase ROM file", "A ROM file is hard to replace,", "if you want to erase it", "type \"ok\"", "" },
                { "Erase 48.rom", "48.rom is required for .tap", "functionality, are you sure you", "want to delete it? if so,", "type ok" },
                { "Erase testrom.rom", "testrom.rom is required for", "Test ROM functionality, are you", "sure you want to delete it?", "if so, type ok" },
                { "Erase DiagROM.rom", "DiagROM.rom is required for", "Test ROM functionality, are you", "sure you want to delete it?", "if so, type ok" },
                { "Erase Save Games", "If you delete this folder you", "will lose all your quick saves,", "type ok to proceed and delete", "" },
                { "Erase Favourites", "If you delete this folder you", "will lose all your favourites,", "type ok to proceed and delete", "" },
            };
            int which = status == FILEOP_IS_DIRECTORY ? 0 : status == FILEOP_IS_ROM ? 1 : status == FILEOP_IS_48ROM ? 2 :
                status == FILEOP_IS_TESTROM ? 3 : status == FILEOP_IS_DIAGROM ? 4 : saves ? 5 : 6;
            char* texts = dest + 2 * SNAPLIST_PATH_SIZE;
            memset(texts, 0, 5 * SNAPLIST_PATH_SIZE);
            for (int k = 0; k < 5; k++) {
                snprintf(texts + k * SNAPLIST_PATH_SIZE, SNAPLIST_PATH_SIZE, "%s", TEXTS[which][k]);
            }
        }
    }
    else if (i >= 0 && op == ACTION_SNAP_ERASE_OP_DO) {
        static char path[BROWSE_PATH_MAX];
        const char* typed = reinterpret_cast<const char*>(nmi_rom_data) + param;
        if (is_rom && strcasecmp(typed, "ok") != 0) {
            status = FILEOP_NOT_CONFIRMED;      // a ROM file needs "ok" as a folder does
        }
        else if (!is_dir) {
            entry_path(i, nullptr, path, sizeof(path));
            walk_stop();
            status = fsops::remove_path(fsops::drive_of(drive), path) ? FILEOP_OK : FILEOP_FAILED;
            LOG("Erase %s: %d\n", path, status);
            if (status != FILEOP_OK) {
                set_last_error("Cannot erase", list_entry_name[i]);
            }
        }
        else {
            // the user must have typed "ok"
            if (strcasecmp(typed, "ok") != 0) {
                status = FILEOP_NOT_CONFIRMED;
            }
            else {
                dir_entry_path(i, path, sizeof(path));
                walk_stop();        // none of the directories may be open, and the list's memory is free
                static char scratch[BROWSE_PATH_MAX + 8];
                bool ok = fsops::remove_tree(fsops::drive_of(drive), path, sizeof(path), find_dirs, 0, ERASE_MAX_DEPTH,
                    scratch, sizeof(scratch));
                LOG("Erase directory %s: %d\n", path, ok);
                status = ok ? FILEOP_OK : FILEOP_FAILED;
                if (!ok) {
                    set_last_error("Cannot erase", list_entry_name[i]);
                }
            }
        }
    }
    nmi_rom_data[0] = status;
}

// A note kept in the internal flash while a New folder is made on the SD card: if the
// Pico stops part way (the menu freezes) the note says how far it got, and the
// next start-up puts it in Help > Diagnostics under "Last error".
// 'A' = about to make it (so FatFs did not return), 'Z' = FatFs returned (then its status).
static const char TRACE_FILE[] = "trace.cfg";
extern "C" { extern unsigned char* ff_pool; extern size_t ff_pool_size; }

// Makes a folder on the SD card. FatFs clears the new folder's whole cluster, which needs a big
// buffer: the small heap can't give one, so it is lent the list's memory (free once walk_stop()
// has emptied the list cache and closed any open directory). Every folder the Picoface makes on
// its own (set up, the first screen grab) goes through here.
static uint8_t make_sd_folder(const char* path) {
    walk_stop();
    ff_pool = reinterpret_cast<unsigned char*>(list_pool);
    ff_pool_size = sizeof(list_pool);
    uint8_t status = fsops::make_dir(Drive::EXTERNAL_SD_CARD, path);
    ff_pool = nullptr;
    ff_pool_size = 0;
    return status;
}
static void trace_step(char stage, int value) {
    char note[3] = { stage, (char) ('0' + value % 10), '\0' };
    int file = pico_open(TRACE_FILE, LFS_O_CREAT | LFS_O_WRONLY | LFS_O_TRUNC);
    if (file >= 0) {
        pico_write(file, note, 2);
        pico_close(file);
    }
}

static void check_trace() {
    char note[2] = { 0, 0 };
    int file = pico_open(TRACE_FILE, LFS_O_RDONLY);
    if (file < 0) {
        return;
    }
    int n = pico_read(file, note, 2);
    pico_close(file);
    if (n == 2 && note[0] == 'A') {
        snprintf(last_error, sizeof(last_error), "New folder froze in FatFs (SD)");
    }
    if (n == 2 && note[0] == 'S') {
        snprintf(last_error, sizeof(last_error), "Set up SD froze at step %c", note[1]);
    }
    if (n == 2 && note[0] != 'Z') {
        trace_step('Z', 0);
    }
}

// Makes a directory in the directory being browsed.
// WORD param1 = pointer to the name
// BYTE param2 = drive
// Status in byte 0, one of FILEOP_*
void nmi_action_snap_mkdir() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t nameoffset =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
    uint8_t drive = nmi_rom_data[4] == 1 ? 1 : 0;
    const char* cwd = cwd_of(drive);
    char name[64];
    snprintf(name, sizeof(name), "%s", reinterpret_cast<const char*>(nmi_rom_data) + nameoffset);
    bool ok = tidy_new_name(name) &&
        strlen(cwd) + 1 + strlen(name) < sizeof(sd_cwd);    // so it can be entered

    uint8_t status = FILEOP_FAILED;
    if (ok) {
        static char path[sizeof(sd_cwd) + 1 + sizeof(name)];   // the length was checked above
        snprintf(path, sizeof(path), "%s/%s", cwd, name);
        walk_stop();
        if (drive == 1) {
            trace_step('A', 0);
        }
        // lend FatFs the list's memory (free after walk_stop): it clears the new folder's whole
        // cluster, in much bigger writes than the small heap allows
        ff_pool = reinterpret_cast<unsigned char*>(list_pool);
        ff_pool_size = sizeof(list_pool);
        status = fsops::make_dir(fsops::drive_of(drive), path);
        ff_pool = nullptr;
        ff_pool_size = 0;
        if (drive == 1) {
            trace_step('Z', status);
        }
        LOG("Make directory %s: %d\n", path, status);
    }
    nmi_rom_data[0] = status;
}


//---------------------------------------------
// Where Save snapshot puts the file: a directory of the internal flash or of the SD card.  The user chooses it by browsing with the Load
// snapshot screen (ACTION_SNA_SAVE_DIR) and it is kept in SAVE_DIR_FILE on
// the internal flash, so it is remembered after power off.

static const char SAVE_DIR_FILE[] = "saveto.cfg";
static uint8_t save_drive = 0;                  // 0 internal flash, 1 SD card
static char save_dir[sizeof(sd_cwd)] = "";      // same form as sd_cwd
static char save_browse_backup[2][sizeof(sd_cwd)]; // flash_cwd and sd_cwd while choosing

static void load_save_dir() {
    char buf[1 + sizeof(save_dir)];
    memset(buf, 0, sizeof(buf));
    int file = pico_open(SAVE_DIR_FILE, LFS_O_RDONLY);
    if (file >= 0) {
        int n = pico_read(file, buf, sizeof(buf) - 1);
        pico_close(file);
        if (n >= 1 && (buf[0] == 0 || buf[0] == 1) && (buf[1] == '\0' || buf[1] == '/')) {
            save_drive = buf[0];
            snprintf(save_dir, sizeof(save_dir), "%s", buf + 1);
        }
    }
    LOG("Snapshots are saved to %s%s/\n", save_drive == 1 ? "SD card " : "internal flash", save_dir);
}

static void store_save_dir() {
    int file = pico_open(SAVE_DIR_FILE, LFS_O_CREAT | LFS_O_WRONLY | LFS_O_TRUNC);
    if (file >= 0) {
        pico_write(file, &save_drive, 1);
        pico_write(file, save_dir, strlen(save_dir) + 1);
        pico_close(file);
    }
}

// The drive to save to now, and the directory in *dir: the root of the internal flash
// if the SD card was chosen but is not there.  A directory that has gone (e.g.
// another card) falls back to the root of its drive; what was chosen is still
// remembered.
static uint8_t save_drive_now(const char** dir) {
    *dir = "";
    uint8_t drive = save_drive == 1 && sd_card_mounted ? 1 : 0;
    if (drive == save_drive && save_dir[0] != '\0' &&
            fsops::kind(fsops::drive_of(drive), save_dir) == fsops::KIND_DIR) {
        *dir = save_dir;
    }
    return drive;
}

// Describes a place snapshots are saved to for the Save screen: "Internal Flash" for the
// root of the flash, otherwise "Flash:/dir" or "SD:/dir"
static void describe_save_place(uint8_t drive, const char* dir, uint8_t* dest) {
    if (drive == 0 && dir[0] == '\0') {
        memset(dest, 0, SNAPLIST_PATH_SIZE);
        strcpy(reinterpret_cast<char*>(dest), "Internal Flash");
    }
    else {
        format_path(drive, dir, dest);
    }
}

// Where a quick save goes: the Save Games folder in the root of the SD card, or, with no card,
// the folder of the game last loaded
static uint8_t quick_save_place(const char** dir) {
    if (sd_card_mounted) {
        *dir = SAVE_GAMES_DIR;
        return 1;
    }
    *dir = last_game_dir;
    return last_game_drive;
}

// WORD param1 = for DESCRIBE where to write a SNAPLIST_PATH_SIZE string
//               saying where snapshots will be saved; for LAST where to write
//               the last game's name
// BYTE param2 = for SET the drive being browsed; for DESCRIBE non-zero to
//               describe the last game's folder instead
// BYTE param3 = one of ACTION_SNA_SAVE_DIR_OP_*:
//   DESCRIBE  describe where snapshots go
//   LAST      the name of the game last loaded or saved, if it can be saved
//             over now and is at most SAVE_NAME_MAX characters.  Returns its
//             length in byte 0, 0 if there is none.
//   BROWSE    the user is about to choose: browse from the save directory.
//             Returns the drive to browse in byte 0.
//   SET       save to the directory being browsed, and stop browsing it
//   CANCEL    stop browsing without changing anything
void nmi_action_sna_save_dir() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t destoffset =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
    uint8_t drive = nmi_rom_data[4];
    uint8_t op = nmi_rom_data[5];
    const char* dir;
    uint8_t status = 0;

    if (op == ACTION_SNA_SAVE_DIR_OP_LAST) {
        // A name longer than the Save screen takes is cut to fit; saving it then makes
        // a new file in the game's folder rather than writing over the game
        size_t len = strlen(last_game_name);
        if (len > SAVE_NAME_MAX) {
            len = SAVE_NAME_MAX;
            while (len > 0 && last_game_name[len - 1] == ' ') {
                len--;
            }
        }
        if (last_game_usable() && len > 0) {
            memcpy(nmi_rom_data + destoffset, last_game_name, len);
            nmi_rom_data[destoffset + len] = '\0';
            status = (uint8_t) len;
        }
    }
    else if (op == ACTION_SNA_SAVE_DIR_OP_QUICK) {
        // "<game> Save Game", in the game's folder, if the game is known and its folder is there
        char quick[SAVE_NAME_MAX + 1];
        // no game loaded since the Spectrum started or was reset: status 254, the ROM says so
        size_t len = !game_running ? 0 : last_game_usable() ? extras::quick_save_name(last_game_name, SAVE_NAME_MAX, quick) : 0;
        if (len > 0) {
            memcpy(nmi_rom_data + destoffset, quick, len + 1);
            status = (uint8_t) len;
        }
        else if (!game_running) {
            status = 254;
        }
    }
    else if (op == ACTION_SNA_SAVE_DIR_OP_DESCRIBE && drive == 2) {
        uint8_t where = quick_save_place(&dir);         // a quick save
        describe_save_place(where, dir, nmi_rom_data + destoffset);
    }
    else if (op == ACTION_SNA_SAVE_DIR_OP_DESCRIBE && drive != 0) {
        describe_save_place(last_game_drive, last_game_dir, nmi_rom_data + destoffset);
    }
    else if (op == ACTION_SNA_SAVE_DIR_OP_DESCRIBE) {
        uint8_t where = save_drive_now(&dir);
        describe_save_place(where, dir, nmi_rom_data + destoffset);
    }
    else if (op == ACTION_SNA_SAVE_DIR_OP_BROWSE) {
        snprintf(save_browse_backup[0], sizeof(save_browse_backup[0]), "%s", flash_cwd);
        snprintf(save_browse_backup[1], sizeof(save_browse_backup[1]), "%s", sd_cwd);
        snap_filter[0] = '\0';
        status = save_drive_now(&dir);
        snprintf(cwd_of(status), sizeof(sd_cwd), "%s", dir);
    }
    else if (op == ACTION_SNA_SAVE_DIR_OP_SET || op == ACTION_SNA_SAVE_DIR_OP_CANCEL) {
        if (op == ACTION_SNA_SAVE_DIR_OP_SET) {
            save_drive = drive == 1 ? 1 : 0;
            snprintf(save_dir, sizeof(save_dir), "%s", cwd_of(save_drive));
            store_save_dir();
            LOG("Snapshots will be saved to %s%s/\n", save_drive == 1 ? "SD card " : "internal flash", save_dir);
        }
        snprintf(flash_cwd, sizeof(flash_cwd), "%s", save_browse_backup[0]);
        snprintf(sd_cwd, sizeof(sd_cwd), "%s", save_browse_backup[1]);
        snap_filter[0] = '\0';
    }
    nmi_rom_data[0] = status;
}

// Where write_z80_128 puts the file
struct FlashSink {
    int file;
    bool write(const uint8_t* data, uint32_t size) {
        return pico_write(file, data, size) == (lfs_ssize_t) size;
    }
};

struct SdSink {
    FIL* file;
    bool write(const uint8_t* data, uint32_t size) {
        UINT written = 0;
        return f_write(file, data, size, &written) == FR_OK && written == size;
    }
};

// Writes the snapshot in snap_load_buffer to sink, as the file type asked for:
// a 48K snapshot as a .sna (as it is) or a .z80, a 128K one as a .z80 or a .sna
// (which has no place for the sound chip registers)
template <class Sink>
static bool write_snapshot_file(Sink& sink, uint8_t format) {
    uint8_t* buf = reinterpret_cast<uint8_t*>(snap_load_buffer);
    bool as_z80 = format == ACTION_SNA_SAVE_TYPE_Z80;
    if (format == ACTION_SNA_SAVE_TYPE_SZX) {
        if (save_is_128k) {
            return snapload::write_szx_128(sink, buf, save128_info, buf[snapload::X_7FFD],
                snap_io_buffer, sizeof(snap_io_buffer));
        }
        return snapload::write_szx_48(sink, buf, snap_io_buffer, sizeof(snap_io_buffer));
    }
    if (save_is_128k) {
        if (as_z80) {
            return snapload::write_z80_128(sink, buf, snap_io_buffer, sizeof(snap_io_buffer));
        }
        return snapload::write_sna_128(sink, buf, save128_info, buf[snapload::X_7FFD],
            snap_io_buffer, sizeof(snap_io_buffer));
    }
    if (as_z80) {
        return snapload::write_z80_48(sink, buf, snap_io_buffer, sizeof(snap_io_buffer));
    }
    return sink.write(buf, SNA_FILE_SIZE);
}

// Saves the snapshot sent by the NMI rom (in snap_load_buffer) in the place
// chosen with ACTION_SNA_SAVE_DIR, as a .sna file or a .z80 file.  Left to
// itself it is a .sna file for a 48K machine and a .z80 file for a 128K one.
// WORD param1 = pointer to file name to save, without its extension
// BYTE param2 = overwrite flag, force overwrite is 1; 2 (a quick save only) is to save
//               under the next number that is free: "<name> 2", "<name> 3" ...
// BYTE param3 = 1 to save in the last game's folder (see
//               ACTION_SNA_SAVE_DIR_OP_LAST) instead of the chosen one; 2 for a quick save,
//               which goes in the Save Games folder of the SD card (made if it is not there)
// BYTE param4 = the file type: ACTION_SNA_SAVE_TYPE_AUTO, _SNA, _Z80 or _SZX
// status is 0 for success, 1 if the file exists, 2 for any other error
void nmi_action_sna_save() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t nameoffset =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
    uint8_t overwrite = nmi_rom_data[4];
    bool over_last = nmi_rom_data[5] != 0;
    bool quick = nmi_rom_data[5] == 2;
    uint8_t type = nmi_rom_data[6];
    const char* dir;
    uint8_t drive;
    if (quick) {
        drive = quick_save_place(&dir);
        if (drive == 1) {
            walk_stop();
            if (fsops::kind(Drive::EXTERNAL_SD_CARD, dir) != fsops::KIND_DIR) {
                ff_pool = reinterpret_cast<unsigned char*>(list_pool);
                ff_pool_size = sizeof(list_pool);
                uint8_t made = fsops::make_dir(Drive::EXTERNAL_SD_CARD, dir);
                ff_pool = nullptr;
                ff_pool_size = 0;
                LOG("Made %s: %d\n", dir, made);
                if (made != FILEOP_OK) {
                    set_last_error("Cannot make", "Save Games");
                    nmi_rom_data[0] = FILEOP_FAILED;
                    return;
                }
            }
        }
        else if (!last_game_usable()) {
            nmi_rom_data[0] = FILEOP_FAILED;
            return;
        }
    }
    else if (over_last) {
        if (!last_game_usable()) {
            LOG("Save: the last game's folder is not there\n");
            nmi_rom_data[0] = FILEOP_FAILED;
            return;
        }
        drive = last_game_drive;
        dir = last_game_dir;
    }
    else {
        drive = save_drive_now(&dir);
    }

    // "same" is the kind of file last loaded or saved
    if (type == ACTION_SNA_SAVE_TYPE_SAME) {
        type = last_format;
    }
    // what the file is: the type asked for, or for "auto" .sna (48K) or .z80 (128K)
    uint8_t format = type == ACTION_SNA_SAVE_TYPE_SZX ? ACTION_SNA_SAVE_TYPE_SZX :
        (type == ACTION_SNA_SAVE_TYPE_Z80 || (type != ACTION_SNA_SAVE_TYPE_SNA && save_is_128k)) ?
        ACTION_SNA_SAVE_TYPE_Z80 : ACTION_SNA_SAVE_TYPE_SNA;
    const char* extension = format == ACTION_SNA_SAVE_TYPE_SZX ? "szx" :
        format == ACTION_SNA_SAVE_TYPE_Z80 ? "z80" : "sna";
    LOG("Saving %s with name: %s, overwrite=%d, drive %d, %s/\n",
        extension, nmi_rom_data + nameoffset, overwrite, drive, dir);
    char name[64];
    snprintf(name, sizeof(name), "%s", reinterpret_cast<const char*>(nmi_rom_data) + nameoffset);

    if (overwrite == 2) {
        // keep the earlier save: use the first number from 2 that is not taken
        overwrite = 0;
        bool found = false;
        for (int number = 2; number < 100 && !found; number++) {
            char numbered[sizeof(name)];
            if (extras::numbered_name(name, number, sizeof(numbered) - 1, numbered) == 0) {
                break;
            }
            char probe[sizeof(name)];
            snprintf(probe, sizeof(probe), "%s", numbered);
            if (!tidy_new_name(probe)) {
                break;
            }
            snprintf(grab_path, sizeof(grab_path), "%s/%s.%s", dir, probe, extension);
            if (fsops::kind(fsops::drive_of(drive), grab_path) == fsops::KIND_NONE) {
                snprintf(name, sizeof(name), "%s", numbered);
                found = true;
            }
        }
        if (!found) {
            set_last_error("Too many saves", name);
            nmi_rom_data[0] = FILEOP_FAILED;
            return;
        }
    }

    if (drive == 0) {
        static char filename[BROWSE_PATH_MAX];
        if (!tidy_new_name(name) || snprintf(filename, sizeof(filename), "%s/%s.%s", dir, name, extension) >= (int) sizeof(filename)) {
            nmi_rom_data[0] = FILEOP_FAILED;
            return;
        }

        int file = pico_open(filename, (LFS_O_CREAT | LFS_O_WRONLY | LFS_O_TRUNC)
        | (overwrite == 0 ? LFS_O_EXCL : 0));

        if (file == LFS_ERR_EXIST) {
            LOG("File exists\n");
            nmi_rom_data[0] = FILEOP_NAME_IN_USE;
            return;
        }

        bool ok = file >= 0;

        if (ok) {
            FlashSink sink = { file };
            ok = write_snapshot_file(sink, format);
            pico_close(file);
            if (!ok) {
                pico_remove(filename);      // not half a snapshot
            }
        }

        if (ok) {
            static char saved_dir[sizeof(last_game_dir)];
            snprintf(saved_dir, sizeof(saved_dir), "%s", dir);     // dir may be last_game_dir
            remember_last_game(0, saved_dir, name);
            last_format = format;
            persist_last_game();
        }
        else {
            set_last_error("Cannot save", name);
        }
        nmi_rom_data[0] = ok ? FILEOP_OK : FILEOP_FAILED;
        return;
    }

    uint8_t status = FILEOP_FAILED;
    if (tidy_new_name(name) && strlen(dir) + 1 + strlen(name) + 5 < BROWSE_PATH_MAX) {
        static char path[BROWSE_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.%s", dir, name, extension);
        walk_stop();        // the directory changes, and none may be open
        static FIL file;
        FRESULT fr = f_open(&file, path, FA_WRITE | (overwrite == 0 ? FA_CREATE_NEW : FA_CREATE_ALWAYS));
        if (fr == FR_OK) {
            SdSink sink = { &file };
            fr = write_snapshot_file(sink, format) ? FR_OK : FR_DENIED;    // a full card is a short write
            FRESULT fc = f_close(&file);
            if (fr == FR_OK) {
                fr = fc;
            }
            if (fr != FR_OK) {
                f_unlink(path);     // don't leave half a snapshot behind
            }
        }
        LOG("Save %s: %d\n", path, fr);
        status = fr == FR_OK ? FILEOP_OK : fr == FR_EXIST ? FILEOP_NAME_IN_USE : FILEOP_FAILED;
        if (fr == FR_OK) {
            static char saved_dir[sizeof(last_game_dir)];
            snprintf(saved_dir, sizeof(saved_dir), "%s", dir);     // dir may be last_game_dir
            remember_last_game(1, saved_dir, name);
            last_format = format;
            persist_last_game();
        }
        else if (status == FILEOP_FAILED) {
            set_last_error("Cannot save", name);
        }
    }
    nmi_rom_data[0] = status;
}


// Sets the text that ACTION_SNAP_LIST searches for.  An empty string ends
// the search.
void nmi_action_snap_find() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t nameoffset =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
    snprintf(snap_filter, sizeof(snap_filter), "%s",
        reinterpret_cast<const char*>(nmi_rom_data) + nameoffset);
    LOG("Searching for \"%s\"\n", snap_filter);
}



void nmi_action_list_rom() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t start =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
    uint16_t destoffset =  (nmi_rom_data[5] << 8) |  nmi_rom_data[4];
    uint16_t stringoffset = destoffset + 22;

    LOG("List roms starting at %d and storing at %X\n", start, destoffset);

    int dir = pico_dir_open("/");
    struct lfs_info info;

    nmi_rom_data[destoffset] = 0;
    nmi_rom_data[destoffset + 1] = 0;
    int16_t foundsofar = 1;

    if (start == 0) {
        add_file_to_list("Internal Rom\0int", destoffset, stringoffset);
    }

    while (pico_dir_read(dir, &info) > 0) {
        if (info.type == LFS_TYPE_REG) {
            const char* ext = split_filename_and_get_ext(info.name);
            if (strcasecmp(ext, "rom") == 0) {
                if (foundsofar >= start) {
                    if (!add_file_to_list(info.name, destoffset, stringoffset))
                        break;
                }
                foundsofar++;
            }
        }
    }
    pico_dir_close(dir);
}

void nmi_action_change_rom() {
    tape_service_stop();
    loaded_paging.valid = false;
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint32_t nameoffset = (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
    bool makeWritable = nmi_rom_data[4] != 0;

    const char* fileAndExt = reinterpret_cast<const char*>(nmi_rom_data + nameoffset);
    char filename[128];
    int filePartLen = strlen(fileAndExt);
    memcpy(filename, fileAndExt, filePartLen);
    filename[filePartLen] = '.';
    const char* ext = fileAndExt + filePartLen + 1;
    strcpy(filename + filePartLen + 1, ext);
    LOG("Rom File name and extension: %s\n", filename);
    LOG("Writable: %d\n", makeWritable);

    if (strcmp(filename, "Internal Rom.int") == 0) {
         // disable PICO from supplying its own rom when the nmi routine exits
         // and thereby return to the Spectrum internal ROM
        rom_state.flags_on_nmi_exit = 0;
        rom_state.writableStartAddress = 32768;
        nmi_rom_data[0] = 0;
    }
    else {
        int file = pico_open(filename, LFS_O_RDONLY);
        if (file < 0) {
            nmi_rom_data[0] = 1;
            return;
        }

        pico_read(file, rom_data, 16384);

        pico_close(file);
        rom_state.flags_on_nmi_exit = 1;
        rom_state.writableStartAddress = makeWritable ? 64 : 32768;
        nmi_rom_data[0] = 0;
    }
}

void nmi_action_read_data_from_pico() {
    // WORD param1 pointer to destination location (in spectrum rom)
    // WORD param2 offset address to read from must be >= 16384
    // WORD param3 length of source data

    uint8_t* nmi_rom_data = rom_data + 16384;
    uint32_t destoffset = (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
    uint32_t srcoffset = (nmi_rom_data[5] << 8) |  nmi_rom_data[4];
    uint32_t length = (nmi_rom_data[7] << 8) |  nmi_rom_data[6];

    if (srcoffset >= 16384) {
        memcpy(nmi_rom_data + destoffset, 
            snap_load_buffer + SNA_HEADER_SIZE + srcoffset - 16384, length);
    }
}


void nmi_action_write_data_to_pico() {
    // WORD param1 pointer to source location of data (in spectrum rom)
    // WORD param2 offset address to write data must be >= 16384
    // WORD param3 length of source data

    uint8_t* nmi_rom_data = rom_data + 16384;
    uint32_t srcoffset = (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
    uint32_t destoffset = (nmi_rom_data[5] << 8) |  nmi_rom_data[4];
    uint32_t length = (nmi_rom_data[7] << 8) |  nmi_rom_data[6];

    if (destoffset >= 16384) {
        memcpy(snap_load_buffer + SNA_HEADER_SIZE + destoffset - 16384,
            nmi_rom_data + srcoffset , length);
    }
}

static bool tape_trap_running();

// A long press of the button resets the Spectrum into its own ROM (see the main loop).
// The reset snapshot then pages in ROM 0, which is the 128K start up.
static bool reset_to_128k_pending = false;

// True when the program running on the Spectrum is a 48K one that the pico started (a
// 48K snapshot, or a tape, which runs as one or boots the 48K ROM).  The NMI rom then
// saves a 48K snapshot even on a 128K machine and leaves the paging alone.  False after
// a 128K snapshot, the long press reset and at power on, when the NMI rom works it out
// itself.  The pico can't see the Spectrum's reset button, so after a reset by hand
// this is out of date until the next load.
static bool running_48k = false;

// The value the NMI rom writes to the paging port (0x7FFD) before it loads a 48K
// snapshot, so the 48K RAM is bank 0 (which is not contended, unlike banks 1, 3, 5
// and 7) and the 48K ROM is the one paged in.  The tape ROM trap boots the 48K ROM
// and also locks the paging port, the same as choosing 48 BASIC does.
static const uint8_t PORT_7FFD_48K = 0x10;              // bank 0, 48K ROM
static const uint8_t PORT_7FFD_48K_LOCKED = 0x30;

// Lock the paging port for a tape loaded through the ROM trap, as "48 BASIC" does?  A
// locked port ignores all writes until the Spectrum is really reset, so a long press of
// the button could not then get back to the 128K start up (ROM 0).  Off: the tape runs
// with bank 0 and the 48K ROM paged in but the port unlocked.  Turn it on if a 48K
// game that writes to ports it should not misbehaves on a 128K machine.
static const bool TAPE_LOCK_PAGING = false;
static const uint8_t PORT_7FFD_128K_ROM0 = 0x00;         // bank 0, the 128K editor ROM, not locked

// Set by ACTION_PREPARE_LOAD: the snapshot is already in the buffer, so the next
// ACTION_BEGIN_SNAP_READ with no name is for it
static bool snap_prepared = false;
// ... and a reset into the 128K start up that the NMI menu asked for, until it is sent
static bool menu_reset_128k = false;

void nmi_action_begin_snap_read() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint32_t nameoffset = (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
    uint8_t drive = nmi_rom_data[4];
    uint32_t headeroffset =  (nmi_rom_data[6] << 8) |  nmi_rom_data[5];

    if (nameoffset != 0) {
        const char* selected_file = list_entry_name_for(nameoffset,
            reinterpret_cast<const char*>(nmi_rom_data + nameoffset));
        LOG("Loading %s\n", selected_file);

        load_snapshot_file(drive, list_entry_dir_for(nameoffset), selected_file);
        LOG("Start snapshot, size = %d, head destination: %X\n", current_snap_size, headeroffset);
    } else if (!snap_prepared) {
        snap_is_paged = false;      // a snapshot sent from a PC is a 48K one
        current_snap_data = snap_load_buffer;
    }
    snap_prepared = false;

    running_48k = !snap_is_paged && !reset_to_128k_pending;
    loaded_paging.valid = snap_is_paged;
    if (snap_is_paged) {
        uint8_t* image = reinterpret_cast<uint8_t*>(snap_load_buffer);
        loaded_paging.port = image[snapload::X_7FFD];
        loaded_paging.bankm = snapload::bank_memory(image, 5)[snapload::BANK_M_ADDRESS - 0x4000];
        LOG("Loaded 128K snapshot: port %02X, BANK_M byte %02X\n", loaded_paging.port, loaded_paging.bankm);
    }
    if (snap_is_paged) {
        // The header, then the eight banks one after the other in the order of
        // snapload::PAGE_ORDER, are what the NMI rom reads
        LOG("Sending 128K snapshot header\n");
        nmi_rom_data[2] = snapload::KIND_PAGED;
        memcpy(nmi_rom_data + headeroffset, snap_load_buffer, snapload::NMI_HEADER128_SIZE);
        current_snap_data = snap_load_buffer + snapload::HEADER_AREA;
        current_snap_size = snapload::IMAGE128_SIZE;
        current_snap_offset = 0;
    }
    else if (current_snap_size == Z80_FILE_SIZE) {
        LOG("Sending Z80 file header\n");
        nmi_rom_data[2] = 1;
        nmi_rom_data[3] = reset_to_128k_pending ? PORT_7FFD_128K_ROM0 :
            (TAPE_LOCK_PAGING && tape_trap_running()) ? PORT_7FFD_48K_LOCKED : PORT_7FFD_48K;
        memcpy(nmi_rom_data + headeroffset, current_snap_data, Z80_HEADER_SIZE);
        current_snap_offset = Z80_HEADER_SIZE;
    }
    else {
        LOG("Sending SNA file header\n");
        nmi_rom_data[2] = 0;
        nmi_rom_data[3] = reset_to_128k_pending ? PORT_7FFD_128K_ROM0 :
            (TAPE_LOCK_PAGING && tape_trap_running()) ? PORT_7FFD_48K_LOCKED : PORT_7FFD_48K;
        memcpy(nmi_rom_data + headeroffset, current_snap_data, SNA_HEADER_SIZE);
        current_snap_offset = SNA_HEADER_SIZE;
    }
    if (menu_reset_128k) {
        menu_reset_128k = false;
        reset_to_128k_pending = false;
    }
}

// ACTION_PREPARE_LOAD: gets a snapshot ready in the buffer without a list entry:
//   BYTE param1 = PREPARE_LOAD_RELOAD, _RESET_48K, _RESET_128K or _DIAGNOSTIC (then BYTE param2:
//   0 testrom.rom, 1 DiagROM.rom)
// The NMI rom then loads it the same way as a file, with a name of 0.  Status 0, or
// 1 if there is no game to reload.
static const uint8_t PREPARE_LOAD_RELOAD = 0;
static const uint8_t PREPARE_LOAD_RESET_48K = 1;
static const uint8_t PREPARE_LOAD_RESET_128K = 2;
static const uint8_t PREPARE_LOAD_DIAGNOSTIC = 3;
static const uint8_t PREPARE_LOAD_RECENT = 4;           // BYTE param2 = the game from 0 (ACTION_RECENT_LIST)

// The diagnostic ROMs, kept in the top folder of the internal flash so that Browse Games can
// copy them there: Brendan Alford's zx-diagnostics testrom.bin as testrom.rom (which = 0)
// and Phil Ruston's DiagROM as DiagROM.rom (which = 1).  The .bin name, and for DiagROM a
// name with no extension, are found too, in any case.  The first 16384 bytes are put in place
// of the Spectrum's ROM.
// Returns 0, 1 if there is no such file, 2 if it is too short.
static uint8_t use_diagnostic_rom(uint8_t which) {
    static const char* const names[2][3] = {
        { "testrom.rom", "testrom.bin", nullptr },
        { "diagrom.rom", "diagrom.bin", "diagrom" },
    };
    const char* const* wanted = names[which == 1 ? 1 : 0];
    static char found[64];
    found[0] = '\0';
    bool best = false;
    int dir = pico_dir_open("/");
    if (dir >= 0) {
        struct lfs_info info;
        while (pico_dir_read(dir, &info) > 0) {
            if (info.type != LFS_TYPE_REG || strlen(info.name) >= sizeof(found)) {
                continue;
            }
            for (int n = 0; n < 3; n++) {
                if (wanted[n] != nullptr && strcasecmp(info.name, wanted[n]) == 0) {
                    if (found[0] == '\0' || n == 0) {
                        strcpy(found, info.name);     // the .rom name is preferred
                    }
                    best = n == 0;
                    break;
                }
            }
            if (best) {
                break;
            }
        }
        pico_dir_close(dir);
    }
    if (found[0] == '\0') {
        return 1;
    }
    int file = pico_open(found, LFS_O_RDONLY);
    if (file < 0) {
        return 1;
    }
    bool whole = pico_size(file) >= 16384 && pico_read(file, rom_data, 16384) == 16384;   // size first: rom_data is in use
    pico_close(file);
    if (!whole) {
        return 2;
    }
    LOG("Diagnostic ROM %s\n", found);
    tape_service_stop();
    loaded_paging.valid = false;
    rom_state.flags_on_nmi_exit = 1;
    rom_state.writableStartAddress = 32768;
    snap_is_paged = false;
    tap_make_reset_snapshot();
    game_running = false;
    menu_reset_128k = false;
    reset_to_128k_pending = false;
    snap_prepared = true;
    return 0;
}

static void nmi_action_prepare_load() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t what = nmi_rom_data[2];
    uint8_t status = 0;
    // load_snapshot_file keeps what it is given, so it is given copies
    static char name[sizeof(reload_name)];
    static char dir[sizeof(reload_dir)];
    if (what == PREPARE_LOAD_RECENT) {
        extras::RecentList* list = recent_read();
        int which = nmi_rom_data[3];
        if (which >= list->count) {
            status = 1;
        }
        else {
            const extras::RecentEntry& e = list->entry[which];
            uint8_t drive = e.drive;
            size_t namelen = strlen(e.name);
            memcpy(name, e.name, namelen + 1);
            memcpy(name + namelen + 1, e.ext, strlen(e.ext) + 1);
            snprintf(dir, sizeof(dir), "%s", e.dir);
            static char path[BROWSE_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s.%s", dir, name, name + namelen + 1);
            if (drive == 1 && !sd_card_mounted) {
                status = 1;
            }
            else if (fsops::kind(fsops::drive_of(drive), path) != fsops::KIND_FILE) {
                status = 2;
            }
            else {
                LOG("Recent %s\n", path);
                load_snapshot_file(drive, dir, name);
                snap_prepared = true;
            }
        }
    }
    else if (what == PREPARE_LOAD_RELOAD) {
        if (!reload_known || (reload_drive == 1 && !sd_card_mounted)) {
            status = 1;
        }
        else {
            memcpy(name, reload_name, sizeof(name));
            memcpy(dir, reload_dir, sizeof(dir));
            LOG("Reload %s\n", name);
            load_snapshot_file(reload_drive, dir, name);
            snap_prepared = true;
        }
    }
    else if (what == PREPARE_LOAD_DIAGNOSTIC) {
        status = use_diagnostic_rom(nmi_rom_data[3]);
    }
    else {
        LOG("Reset into %s\n", what == PREPARE_LOAD_RESET_128K ? "128K" : "48K");
        tape_service_stop();
        snap_is_paged = false;
        tap_make_reset_snapshot();
        game_running = false;
        menu_reset_128k = what == PREPARE_LOAD_RESET_128K;
        reset_to_128k_pending = menu_reset_128k;
        snap_prepared = true;
    }
    nmi_rom_data[0] = status;
}


// The Spectrum's screen while the NMI menu is open. The NMI ROM sends it here
// on entry (ACTION_SCREEN_STASH) and takes it back on exit
// (ACTION_SCREEN_RESTORE), so it needs no room in the NMI ROM. Snapshots
// saved from the menu take the screen from here (ACTION_SNA_WRITE_SCREEN).
static uint8_t screen_stash[6912];
static bool screen_stashed = false;

// The screen ACTION_SNAP_PREVIEW found: 6912 bytes in snap_load_buffer, or null.
static const uint8_t* preview_screen = nullptr;

// The game ACTION_SNAP_PREVIEW was asked about (its name without the extension) and the
// screen it found, which ACTION_GAME_INFO shows a small copy of
static char info_name[gameinfo::GAME_MAX + 1] = "";
// The file of the game information (mode 0): its drive and full path, as find_preview opened
// it. C on its information converts it if it is a tape.
static char info_path[BROWSE_PATH_MAX] = "";
static uint8_t info_drive = 0;

// While a tape is converted (C), the screen sent to the Spectrum is the emulated one with
// the top and bottom rows drawn over it (conv_overlay)
static bool conv_overlay_on = false;
static void conv_overlay(uint8_t* dest, uint32_t at, uint32_t length);
static const uint8_t* info_source = nullptr;

enum ScreenOp { SCREEN_STASH, SCREEN_RESTORE, SCREEN_PREVIEW };

// ACTION_SCREEN_STASH, ACTION_SCREEN_RESTORE and ACTION_PREVIEW_READ
// WORD param1 = where the piece is in the NMI ROM
// WORD param2 = the screen address of the piece (16384 on)
// WORD param3 = length
static void nmi_action_screen(ScreenOp op) {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t buffer = nmi_rom_data[2] | (nmi_rom_data[3] << 8);
    uint16_t address = nmi_rom_data[4] | (nmi_rom_data[5] << 8);
    uint16_t length = nmi_rom_data[6] | (nmi_rom_data[7] << 8);
    if (address < 16384 || address - 16384 + length > sizeof(screen_stash) ||
        buffer + length > 16384) {
        return;
    }
    uint32_t at = address - 16384;
    if (op == SCREEN_STASH) {
        memcpy(screen_stash + at, nmi_rom_data + buffer, length);
        screen_stashed = true;
    } else if (op == SCREEN_RESTORE) {
        if (screen_stashed) {
            memcpy(nmi_rom_data + buffer, screen_stash + at, length);
        }
    } else if (preview_screen != nullptr) {
        memcpy(nmi_rom_data + buffer, preview_screen + at, length);
        if (conv_overlay_on) {
            conv_overlay(nmi_rom_data + buffer, at, length);
        }
    }
}

// What a .tap or .tzx is made of, counted by tape_scan when the tape is previewed, and shown
// on the tape page of the game information (tape_text).  The blocks are counted as they are
// in the file: loops, jumps and calls are not followed.
struct TapeSummary {
    uint32_t standard, turbo, pure;     // data blocks: 0x10, 0x11, 0x14 (a .tap's are all standard)
    uint32_t pulse;                     // pulse and tone blocks, direct recording, CSW, generalised data:
                                        // the loader needs the signal itself
    uint32_t loops, jumps, calls;
    bool tzx, bad;                      // a .tzx; unreadable (damaged or a block that is not known)
};
static TapeSummary tape_sum;
static bool tape_sum_valid = false;

// Moves on n bytes without reading them. False if that is past the end of the file.
static bool tape_hop(FileHandle& f, uint32_t n, uint32_t size) {
    uint32_t at = f.tell();
    return at + n <= size && f.seek(at + n);
}

// Counts the blocks of the tape in f, which is at its start
__attribute__((noinline)) static void tape_scan(FileHandle& f, bool tzx) {
    memset(&tape_sum, 0, sizeof(tape_sum));
    tape_sum.tzx = tzx;
    tape_sum_valid = true;
    const uint32_t size = (uint32_t) f.size();
    if (tzx) {
        if (!tzx_begin(f)) {
            tape_sum.bad = true;
            return;
        }
        while (f.tell() < size) {
            TzxBlock b;
            if (!tzx_read_header(f, b)) {
                tape_sum.bad = true;
                return;
            }
            switch (b.id) {
                case 0x10: tape_sum.standard++; break;
                case 0x11: tape_sum.turbo++; break;
                case 0x14: tape_sum.pure++; break;
                case 0x12: case 0x13: case 0x15: case 0x18: case 0x19: tape_sum.pulse++; break;
                case 0x24: tape_sum.loops++; break;
                case 0x23: tape_sum.jumps++; break;
                case 0x26: tape_sum.calls++; break;
                default: break;
            }
            if (!tape_hop(f, b.data ? b.blen : b.skip, size)) {
                tape_sum.bad = true;
                return;
            }
        }
    } else {
        while (f.tell() + 2 <= size) {
            uint8_t lb[2];
            if (!tap_read_exact(f, lb, 2)) {
                break;
            }
            tape_sum.standard++;
            if (!tape_hop(f, lb[0] | (lb[1] << 8), size)) {
                tape_sum.bad = true;
                return;
            }
        }
    }
}

// What the Picoface can say about loading the tape: the verdict, and the reason
static const char* tape_verdict(const char** reason) {
    const TapeSummary& s = tape_sum;
    if (s.bad) {
        *reason = "Damaged, or a block this PicoMenu does not know.";
        return "No";
    }
    if (s.standard + s.turbo + s.pure == 0) {
        *reason = "No data blocks in it.";
        return "No";
    }
    if (s.pulse > 0) {
        *reason = "It has pulse or direct recording blocks: a loader that times the signal cannot work.";
        return "Probably not";
    }
    if (s.turbo + s.pure > 0) {
        *reason = "Turbo or pure data: works only if the game loads it with the ROM's loader.";
        return "Maybe";
    }
    *reason = s.tzx ? "Standard blocks only." : "Ordinary blocks (a turbo loader cannot be seen in a .tap).";
    return "Yes";
}

// The tape page of the game information. Lines end in \n.
__attribute__((noinline)) static void tape_text(char* out, size_t max, const char** say) {
    const TapeSummary& s = tape_sum;
    const char* reason;
    *say = tape_verdict(&reason);
    size_t n = 0;
    auto add = [&](const char* fmt, uint32_t a, uint32_t b, uint32_t c) {
        if (n < max) n += snprintf(out + n, max - n, fmt, (unsigned) a, (unsigned) b, (unsigned) c);
    };
    if (s.tzx) {
        add("Data blocks: %u  Standard: %u\n", s.standard + s.turbo + s.pure, s.standard, 0);
        add("Turbo blocks: %u\n", s.turbo, 0, 0);
        add("Pure data blocks: %u\n", s.pure, 0, 0);
        add("Pulse/direct recording: %u\n", s.pulse, 0, 0);
        add("Loops: %u  Jumps: %u  Calls: %u\n", s.loops, s.jumps, s.calls);
    } else {
        add("Data blocks: %u\n", s.standard, 0, 0);
    }
    if (n < max) n += snprintf(out + n, max - n, "PicoMenu can load: %s\n%s\n", *say, reason);
    if (n < max) n += snprintf(out + n, max - n, "\n\x01Press C to convert this .tzx or\n\x01.tap into a .z80\n");
    if (n >= max) {
        n = max - 1;
    }
    out[n] = '\0';
}

// Finds the screen of a snapshot or tape for a preview and points preview_screen
// at it.  The file is read into snap_load_buffer, which is free while the menu is
// browsing (a load or save fills it again from the start).
// Returns 0 when there is a screen, 1 if the file has none, 2 if it could not be read.
static uint8_t find_preview(uint8_t drive, const char* filename, const char* ext) {
    uint8_t* buf = reinterpret_cast<uint8_t*>(snap_load_buffer);
    FileHandle file;
    tape_sum_valid = false;
    info_drive = drive;
    snprintf(info_path, sizeof(info_path), "%s", filename);
    if (!file.open(drive == 1 ? Drive::EXTERNAL_SD_CARD : Drive::INTERNAL_FLASH, filename, true)) {
        LOG("Preview: could not open %s\n", filename);
        return 2;
    }
    uint8_t status = 1;
    const bool tzx = strcasecmp(ext, "tzx") == 0;
    if (tzx && !tzx_begin(file)) {
        status = 2;
    } else if (tzx || strcasecmp(ext, "tap") == 0) {
        // the data block after a CODE header for 16384 with at least 6912 bytes,
        // as LOAD "" SCREEN$ loads
        bool screen_next = false;
        while (status == 1) {
            uint32_t blen;
            uint8_t flag;
            if (!tape_next_block(file, tzx, blen)) break;
            if (blen < 2 || !tap_read_exact(file, &flag, 1)) break;
            uint32_t payload = blen - 2;
            if (flag == 0x00 && payload == 17) {
                uint8_t h[17];
                if (!tap_read_exact(file, h, 17) || !tap_skip(file, 1)) break;
                uint16_t hlen = h[11] | (h[12] << 8);
                uint16_t hstart = h[13] | (h[14] << 8);
                screen_next = h[0] == 3 && hstart == 16384 && hlen >= 6912;
                continue;
            }
            if (screen_next && flag == 0xFF && payload >= 6912) {
                status = tap_read_exact(file, buf, 6912) ? 0 : 2;
                preview_screen = buf;
                break;
            }
            screen_next = false;
            if (!tap_skip(file, payload + 1)) break;
        }
    } else if (strcasecmp(ext, "scr") == 0) {
        // a screen (L-All lists them): V shows it
        status = file.read(buf, 6912) == 6912 ? 0 : 2;
        preview_screen = buf;
    } else {
        snapload::Kind kind;
        int32_t fileSize = file.size();
        if (strcasecmp(ext, "z80") == 0) {
            kind = snapload::load_z80(file, fileSize, buf, snap_io_buffer, sizeof(snap_io_buffer));
        } else if (strcasecmp(ext, "szx") == 0) {
            kind = snapload::load_szx(file, buf, snap_io_buffer, sizeof(snap_io_buffer));
        } else if (fileSize >= 0 && snapload::is_sna128((uint32_t) fileSize)) {
            kind = snapload::load_sna128(file, buf, snap_io_buffer, sizeof(snap_io_buffer));
        } else {
            kind = file.read(buf, SNA_HEADER_SIZE + 6912) == SNA_HEADER_SIZE + 6912 ?
                snapload::KIND_SNA48 : snapload::KIND_ERROR;
        }
        if (kind == snapload::KIND_SNA48) {
            preview_screen = buf + SNA_HEADER_SIZE;
        } else if (kind == snapload::KIND_Z80V1) {
            preview_screen = buf + Z80_HEADER_SIZE;
        } else if (kind == snapload::KIND_PAGED) {
            static_assert(snapload::PAGE_ORDER[0] == 5, "bank 5, the screen, comes first");
            preview_screen = buf + snapload::HEADER_AREA;
        }
        status = kind == snapload::KIND_ERROR ? 2 : 0;
    }
    if (tzx || strcasecmp(ext, "tap") == 0) {
        if (file.seek(0)) tape_scan(file, tzx);
    }
    file.close();
    if (status != 0) {
        preview_screen = nullptr;
    }
    LOG("Preview of %s: %s\n", filename, status == 0 ? "found" : status == 1 ? "no screen" : "could not read it");
    return status;
}

// ACTION_SNAP_PREVIEW
// WORD param1 = the name of an entry in the list
// BYTE param2 = drive
static void nmi_action_snap_preview() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t nameoffset = nmi_rom_data[2] | (nmi_rom_data[3] << 8);
    uint8_t drive = nmi_rom_data[4];
    preview_screen = nullptr;
    const char* fileAndExt = list_entry_name_for(nameoffset,
        reinterpret_cast<const char*>(nmi_rom_data + nameoffset));
    const char* ext = fileAndExt + strlen(fileAndExt) + 1;
    static char filename[BROWSE_PATH_MAX];
    snprintf(filename, sizeof(filename), "%s/%s.%s", list_entry_dir_for(nameoffset), fileAndExt, ext);
    nmi_rom_data[0] = find_preview(drive, filename, ext);
    snprintf(info_name, sizeof(info_name), "%s", fileAndExt);
    // a screen of nothing but zeros is no picture: only the screen grabs are cycled then
    info_source = (preview_screen != nullptr && !gameinfo::screen_is_blank(preview_screen)) ? preview_screen : nullptr;
}

// ACTION_HELP_PAGE: draws a page of the help text (Docs/Help.txt, helptext.h) as a
// screen in snap_load_buffer and points preview_screen at it, so ACTION_PREVIEW_READ
// fetches it like a game's screen.
// BYTE param1 = the page, counting from 0 (a page past the last gives the last)
// Status: the number of pages (never 0)
// Layout: row 0 the title and the page number (white on blue), rows 1-22 the text
// (white on black; a line starting "# " is a heading in yellow), row 23 the keys.
static void help_put_text(uint8_t* screen, int row, int col, const char* text, int len, uint8_t attr) {
    for (int i = 0; i < len && col + i < 32; i++) {
        unsigned char ch = (unsigned char) text[i];
        const uint8_t* glyph = help_font[(ch >= 32 && ch < 128 ? ch : '?') - 32];
        for (int line = 0; line < 8; line++) {
            screen[((row >> 3) << 11) | (line << 8) | ((row & 7) << 5) | (col + i)] = glyph[line];
        }
        screen[6144 + row * 32 + col + i] = attr;
    }
}

// The values behind the {tokens} of Docs/Help.txt, which the Diagnostics page uses
static void help_token(const char* token, size_t len, char* out, size_t outsize) {
    out[0] = '\0';
    auto is = [&](const char* name) { return strlen(name) == len && strncmp(token, name, len) == 0; };
    if (is("version")) {
        snprintf(out, outsize, "%s %s", FIRMWARE_VERSION, __DATE__);
    }
    else if (is("flash")) {
        pico_fsstat_t stats;
        if (pico_fsstat(&stats) < 0) {
            snprintf(out, outsize, "unknown");
        }
        else {
            char free_text[16], total_text[16];
            extras::format_big((uint64_t) (stats.block_count - stats.blocks_used) * stats.block_size, free_text, sizeof(free_text));
            extras::format_big((uint64_t) stats.block_count * stats.block_size, total_text, sizeof(total_text));
            snprintf(out, outsize, "%s free of %s", free_text, total_text);
        }
    }
    else if (is("sd")) {
        if (usb_msc_pc_holds_card()) {
            snprintf(out, outsize, "In use by the PC");
        }
        else if (!sd_card_mounted) {
            snprintf(out, outsize, "No card");
        }
        else {
            sd_card_t* pSD = sd_get_by_num(0);
            DWORD clusters = 0;
            FATFS* fs = nullptr;
            if (f_getfree(pSD->pcName, &clusters, &fs) != FR_OK || fs == nullptr) {
                snprintf(out, outsize, "Cannot read it");
            }
            else {
                char free_text[16], total_text[16];
                uint64_t cluster_bytes = (uint64_t) fs->csize * 512;
                extras::format_big(clusters * cluster_bytes, free_text, sizeof(free_text));
                extras::format_big((uint64_t) (fs->n_fatent - 2) * cluster_bytes, total_text, sizeof(total_text));
                snprintf(out, outsize, "%s free of %s", free_text, total_text);
            }
        }
    }
    else if (is("error1") || is("error2")) {
        // the last error over two lines of up to 30 characters
        size_t n = strlen(last_error);
        if (n == 0) {
            snprintf(out, outsize, "%s", is("error1") ? "None" : "");
        }
        else if (is("error1")) {
            snprintf(out, outsize, "%.30s", last_error);
        }
        else if (n > 30) {
            snprintf(out, outsize, "%s", last_error + 30);
        }
    }
}

// Copies a line of help text to out with its {tokens} replaced
static int help_expand(const char* line, int len, char* out, size_t outsize) {
    size_t n = 0;
    for (int i = 0; i < len && n + 1 < outsize; ) {
        if (line[i] == '{') {
            const char* close = (const char*) memchr(line + i, '}', len - i);
            if (close != nullptr) {
                char value[48];
                help_token(line + i + 1, close - (line + i + 1), value, sizeof(value));
                for (const char* v = value; *v != '\0' && n + 1 < outsize; v++) {
                    out[n++] = *v;
                }
                i = (int) (close - line) + 1;
                continue;
            }
        }
        out[n++] = line[i++];
    }
    out[n] = '\0';
    return (int) n;
}

static void nmi_action_help_page() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    int page = nmi_rom_data[2];
    if (page >= HELP_PAGE_COUNT) page = HELP_PAGE_COUNT - 1;
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer);
    memset(screen, 0, 6144);
    memset(screen + 6144, 0x07, 768);                 // white ink, black paper, as the menu
    const uint8_t BANNER = 0x4F, TEXT = 0x47, HEADING = 0x46, KEYS = 0x45;   // bright
    // title row
    memset(screen + 6144, BANNER, 32);
    const HelpPage& hp = help_pages[page];
    help_put_text(screen, 0, 1, hp.title, strlen(hp.title), BANNER);
    char number[12];
    int n = snprintf(number, sizeof(number), "%d/%d", page + 1, HELP_PAGE_COUNT);
    help_put_text(screen, 0, 31 - n, number, n, BANNER);
    // the text
    int row = 1;
    for (const char* p = hp.text; *p && row <= 22; row++) {
        const char* end = strchr(p, '\n');
        int len = end ? (int)(end - p) : (int)strlen(p);
        char expanded[96];
        if (memchr(p, '{', len) != nullptr) {
            len = help_expand(p, len, expanded, sizeof(expanded));
            if (len >= 2 && expanded[0] == '#' && expanded[1] == ' ') {
                help_put_text(screen, row, 0, expanded + 2, len - 2, HEADING);
            } else {
                help_put_text(screen, row, 0, expanded, len, TEXT);
            }
            p += strcspn(p, "\n");
            p += (*p == '\n') ? 1 : 0;
            continue;
        }
        if (len >= 2 && p[0] == '#' && p[1] == ' ') {
            help_put_text(screen, row, 0, p + 2, len - 2, HEADING);
        } else {
            help_put_text(screen, row, 0, p, len, TEXT);
        }
        p += len + (end ? 1 : 0);
    }
    const char* keys = page == 0 ? "N-Next page      X-Exit" :
        page == HELP_PAGE_COUNT - 1 ? "P-Previous page  X-Exit" : "N-Next  P-Previous  X-Exit";
    help_put_text(screen, 23, 1, keys, strlen(keys), KEYS);
    preview_screen = screen;
    nmi_rom_data[0] = HELP_PAGE_COUNT;
}

// The poke lists: the .pok file in the Poke List folder of the SD card, named like the game
// last loaded (or like its name without " Save Game"), read when the list is first asked for
// Also the converter's working space while a tape is converted (C): see conv_state. It is
// read again whenever the poke list is opened.
alignas(8) static extras::PokFile pok_file;

static const char POKE_LIST_DIR[] = "/Poke List";     // in the root of the SD card

// Looks for <name>.pok (.POK too) in the Poke List folder. Returns the number of cheats.

static int pok_load_for_last_game() {
    pok_file.clear();
    if (!reload_known || !sd_card_mounted) {
        return 0;
    }
    static char base[sizeof(reload_name)];
    static char path[BROWSE_PATH_MAX];
    size_t full = strlen(reload_name);
    size_t shorter = extras::base_name_length(reload_name);
    for (int attempt = 0; attempt < 2; attempt++) {
        size_t n = attempt == 0 ? full : shorter;
        if (attempt == 1 && shorter == full) {
            break;
        }
        memcpy(base, reload_name, n);
        base[n] = '\0';
        for (int upper = 0; upper < 2; upper++) {
            const char* ext = upper ? "POK" : "pok";
            snprintf(path, sizeof(path), "%s/%s.%s", POKE_LIST_DIR, base, ext);
            FileHandle f;
            if (f.open(Drive::EXTERNAL_SD_CARD, path, true)) {
                int cheats = pok_file.load(f);
                f.close();
                LOG("%s: %d cheats\n", path, cheats);
                return cheats;
            }
        }
    }
    return 0;
}

// ACTION_POKE_LIST: WORD param1 = the first cheat, WORD param2 = where to put the
// page (a count, a "more" flag, ten name pointers and the names, as for ACTION_SNAP_LIST).
// Asking for cheat 0 reads the file again.  Status 1 if there are no cheats.
static void nmi_action_poke_list() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t start = nmi_rom_data[2] | (nmi_rom_data[3] << 8);
    uint16_t dest = nmi_rom_data[4] | (nmi_rom_data[5] << 8);
    if (start == 0) {
        pok_load_for_last_game();
    }
    if (pok_file.ncheats == 0) {
        nmi_rom_data[0] = 1;
        return;
    }
    extras::poke_list_page(pok_file, start, nmi_rom_data, dest);
    nmi_rom_data[0] = 0;
}

// ACTION_POKE_GET: WORD param1 = the cheat, WORD param2 = where to put its pokes
// (see extras::poke_get).  Status 1 if there is no such cheat.
static void nmi_action_poke_get() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t cheat = nmi_rom_data[2] | (nmi_rom_data[3] << 8);
    uint16_t dest = nmi_rom_data[4] | (nmi_rom_data[5] << 8);
    nmi_rom_data[0] = extras::poke_get(pok_file, cheat, nmi_rom_data, dest) ? 0 : 1;
}

// ACTION_GAME_INFO: the controls and instructions of a game (the file Instructions.txt in the
// folder Scraper of the SD card, or in its root, see gameinfo.h) with a small copy of its screen, as a screen in
// snap_load_buffer which ACTION_PREVIEW_READ fetches.
// BYTE param1 = 0 the game ACTION_SNAP_PREVIEW was last asked about, 1 the game last loaded
// BYTE param2 = the page from 0
// BYTE param3 = the picture from 0 (mode 0 only: the screen of the game's file, if it has
//               one, and then the screen grabs of the game, see ACTION_SCREEN_GRAB)
// Status: the number of pages (never 0) in the low four bits, the number of pictures in
// the high four bits (the menu's page, mode 1, shows the running game's screen and has none)
// Layout: row 0 the name (white on blue), the picture and page numbers, then the picture
// (left) and the controls (right), the instructions below, the keys on row 23.
// The screen is built at INFO_SCREEN_OFFSET and the entry kept at INFO_DATA_OFFSET of
// snap_load_buffer: above where a preview puts the game's screen, which is still wanted
// when going back to the first page. A screen grab is read into GRAB_SCR_OFFSET.
static const uint32_t INFO_SCREEN_OFFSET = 16384;
static const uint32_t INFO_DATA_OFFSET = 24576;
static const uint32_t GRAB_SCR_OFFSET = 32768;
static_assert(INFO_DATA_OFFSET + sizeof(gameinfo::Info) <= GRAB_SCR_OFFSET, "no room for the game information");
static_assert(GRAB_SCR_OFFSET + screengrab::SCR_SIZE <= SNAP_LOAD_BUFFER_SIZE, "no room for a screen grab");
static bool info_loaded = false;
static bool info_full = false;      // View: the picture on the whole screen (F toggles; kept until power off)

__attribute__((noinline)) static bool info_read_file(gameinfo::Info* info, const char* game) {
    // the folder Scraper first (where the scraper program puts it), then the root of the card
    static const char* const paths[] = { "/Scraper/Instructions.txt", "/Scraper/Instructions", "/Instructions.txt", "/Instructions" };
    for (const char* path : paths) {
        FileHandle f;
        if (f.open(Drive::EXTERNAL_SD_CARD, path, true)) {
            bool found = info->load(f, game);
            f.close();
            LOG("%s: %s for %s\n", path, found ? "found" : "no entry", game);
            return found;
        }
    }
    LOG("No Instructions file\n");
    return false;
}

// The screen grabs of the game ACTION_SNAP_PREVIEW was asked about: the numbers (0 for the
// name alone, 1 for name1 ...) of the .scr files in the Screen Grabs folder of the scraper
// (/Scraper/Screen Grabs, the loading screens it downloads), then in /Screen Grabs (the grabs
// made here). grab_from says which folder a picture is in.
static int grab_count = 0;
static uint8_t grab_number[screengrab::MAX_PICTURES];
static uint8_t grab_from[screengrab::MAX_PICTURES];             // 0 /Scraper/Screen Grabs, 1 /Screen Grabs
static const char* grab_folder(int k) { return grab_from[k] == 0 ? screengrab::SCRAPER_FOLDER : screengrab::FOLDER; }

__attribute__((noinline)) static void grabs_find(const char* game) {
    grab_count = 0;
    char base[screengrab::NAME_MAX + 1];
    if (!sd_card_mounted || screengrab::base_name(game, base) == 0) {
        return;
    }
    for (int from = 0; from < 2; from++) {
        const char* folder = from == 0 ? screengrab::SCRAPER_FOLDER : screengrab::FOLDER;
        if (fsops::kind(Drive::EXTERNAL_SD_CARD, folder) != fsops::KIND_DIR) {
            continue;                   // no folder: no 40 lookups
        }
        for (int i = 0; i < screengrab::MAX_SCAN && grab_count < screengrab::MAX_PICTURES; i++) {
            screengrab::path_in(folder, base, i, ".scr", grab_path, sizeof(grab_path));
            if (fsops::kind(Drive::EXTERNAL_SD_CARD, grab_path) == fsops::KIND_FILE) {
                grab_from[grab_count] = (uint8_t) from;
                grab_number[grab_count++] = (uint8_t) i;
            }
        }
    }
    LOG("%d screen grabs of %s\n", grab_count, game);
}

__attribute__((noinline)) static bool grab_load(const char* game, int k, uint8_t* dest) {
    char base[screengrab::NAME_MAX + 1];
    if (k < 0 || k >= grab_count || screengrab::base_name(game, base) == 0) {
        return false;
    }
    screengrab::path_in(grab_folder(k), base, grab_number[k], ".scr", grab_path, sizeof(grab_path));
    FileHandle f;
    if (!f.open(Drive::EXTERNAL_SD_CARD, grab_path, true)) {
        return false;
    }
    int got = f.read(dest, screengrab::SCR_SIZE);
    f.close();
    return got == (int) screengrab::SCR_SIZE;
}

// ---- My notes: <game>.txt in the hidden Game Notes folder (notes.h)
// The editor and the bytes of a file are kept in snap_load_buffer above the screen grab
static const uint32_t NOTES_EDITOR_OFFSET = 40960;
static const uint32_t NOTES_FILE_OFFSET = NOTES_EDITOR_OFFSET + 1024;
static_assert(sizeof(notes::Editor) <= 1024, "the notes editor does not fit");
static_assert(NOTES_FILE_OFFSET + notes::FILE_MAX + 2 <= NOTES_EDITOR_OFFSET + 2048, "the notes file does not fit");
static_assert(gameinfo::TAPE_MAX + 2 <= 1024, "the tape summary is made in the notes file's place");
// The SD card's file handle is big and core 0's stack is only 2K, so the notes' handle is
// kept in the buffer too (never on the stack: the first save hung the Spectrum that way)
static const uint32_t NOTES_HANDLE_OFFSET = NOTES_EDITOR_OFFSET + 2048;
static_assert(NOTES_HANDLE_OFFSET + sizeof(FileHandle) <= 65536, "the notes use the recent games' room");
#define NOTES_FUNC __attribute__((noinline))
static FileHandle* notes_handle() {
    return new (snap_load_buffer + NOTES_HANDLE_OFFSET) FileHandle();
}
static bool notes_editing = false;                      // the notes screen is up: every key goes to notes_step
static bool notes_edit = false;                         // ...and the notes can be changed (E was pressed)
static bool notes_saved = false;                        // a save was made, so the information shows the notes page
static const char* notes_message = nullptr;             // shown once on the key row

static const char NOTES_NO_FOLDER[] = "No Game Notes folder - add one";    // 30 letters: the editor's width

// ---- Details typed in on the Spectrum: <game>.info.txt in the Game Notes folder (gameinfo::Details).
// M on the game's information opens a small menu (the hub): N the notes, H the software house,
// Y the year, C the controls, I the instructions, R takes back what was typed in. Each of H Y C I
// is edited in a box of its own, with a limit, and saved on its own.
using DetEditorBig = notes::EditorT<96, 30>;            // the controls and the instructions
using DetEditorOne = notes::EditorT<1, 30>;             // the software house and the year
static const uint32_t DETAILS_OFFSET = 49152;           // the Details of the game
static const uint32_t DET_EDITOR_OFFSET = DETAILS_OFFSET + 3584;
static const uint32_t DET_FILE_OFFSET = DET_EDITOR_OFFSET + 3584;
static const size_t DET_FILE_MAX = 3072;
static_assert(sizeof(gameinfo::Details) <= 3584, "the details do not fit");
static_assert(sizeof(DetEditorBig) <= 3584, "the details editor does not fit");
static_assert(NOTES_HANDLE_OFFSET + sizeof(FileHandle) <= DETAILS_OFFSET, "the details would cover the notes' file handle");
static_assert(DET_FILE_OFFSET + DET_FILE_MAX <= 65536, "the details use the recent games' room");
static_assert(gameinfo::TEXT_MAX + gameinfo::CONTROLS_MAX + 200 <= DET_FILE_MAX, "a details file does not fit its buffer");
enum EditKind : uint8_t { EK_HUB, EK_NOTES, EK_FIELD };
static uint8_t edit_kind = EK_HUB;                      // which screen is up while notes_editing
static char det_field = 0;                              // h y c or i, while EK_FIELD
static bool det_saved = false;                          // something was saved: show the first page after
static bool det_have = false;                           // the game has a details file
static bool det_confirm = false;                        // R was pressed: Y takes the details back

static gameinfo::Details* det_data() { return reinterpret_cast<gameinfo::Details*>(snap_load_buffer + DETAILS_OFFSET); }
static char* det_file() { return snap_load_buffer + DET_FILE_OFFSET; }

// The folder is made on a PC (in the root of the card), not by the Picoface
NOTES_FUNC static bool notes_folder_there() {
    return fsops::kind(Drive::EXTERNAL_SD_CARD, notes::FOLDER) == fsops::KIND_DIR;
}

NOTES_FUNC static bool notes_path(const char* game, char* path, size_t size) {
    char base[screengrab::NAME_MAX + 1];
    if (!sd_card_mounted || screengrab::base_name(game, base) == 0) {
        return false;
    }
    notes::path_for(base, path, size);
    return true;
}

// The file's bytes into dest (at most cap). Returns how many, 0 if there is none
NOTES_FUNC static size_t notes_read(const char* game, char* dest, size_t cap) {
    char path[screengrab::NAME_MAX + 24];
    if (!notes_path(game, path, sizeof(path))) {
        return 0;
    }
    FileHandle& f = *notes_handle();
    if (!f.open(Drive::EXTERNAL_SD_CARD, path, true)) {
        return 0;
    }
    int got = f.read(dest, cap);
    f.close();
    return got > 0 ? (size_t) got : 0;
}

// Saves the notes (removes the file when there is nothing)
NOTES_FUNC static bool notes_write(const char* game, const char* data, size_t n) {
    char path[screengrab::NAME_MAX + 24];
    if (!notes_path(game, path, sizeof(path))) {
        return false;
    }
    if (n == 0) {
        if (notes_folder_there()) {
            fsops::remove_path(Drive::EXTERNAL_SD_CARD, path);
        }
        return true;
    }
    if (!notes_folder_there()) {
        notes_message = NOTES_NO_FOLDER;
        return false;
    }
    // Written straight to the file, as a screen grab is: renaming and deleting are the
    // deepest SD card calls, and core 0's stack is small. On a failure the editor stays
    // open, so the text is not lost and the save can be tried again.
    bool ok = false;
    FileHandle& f = *notes_handle();
    if (f.open(Drive::EXTERNAL_SD_CARD, path, false)) {
        ok = f.write(data, n) == (int) n;
        ok = f.close() && ok;
    }
    if (!ok) {
        fsops::remove_path(Drive::EXTERNAL_SD_CARD, path);
        set_last_error("Cannot save", "notes");
    }
    return ok;
}

NOTES_FUNC static bool det_path(const char* game, char* path, size_t size) {
    char base[screengrab::NAME_MAX + 1];
    if (!sd_card_mounted || screengrab::base_name(game, base) == 0) {
        return false;
    }
    snprintf(path, size, "%s/%s.info.txt", notes::FOLDER, base);
    return true;
}

// The game's details file into the Details (empty ones if there is none). True if there is one.
NOTES_FUNC static bool det_read(const char* game, gameinfo::Details* d) {
    d->clear();
    char path[screengrab::NAME_MAX + 24];
    if (!det_path(game, path, sizeof(path))) {
        return false;
    }
    FileHandle& f = *notes_handle();
    if (!f.open(Drive::EXTERNAL_SD_CARD, path, true)) {
        return false;
    }
    int got = f.read(det_file(), DET_FILE_MAX - 1);
    f.close();
    if (got <= 0) {
        return true;
    }
    d->parse(det_file(), (size_t) got);
    return true;
}

// Keeps the Details as the game's file (removes the file when nothing is left in them)
NOTES_FUNC static bool det_write(const char* game, const gameinfo::Details* d) {
    char path[screengrab::NAME_MAX + 24];
    if (!det_path(game, path, sizeof(path))) {
        return false;
    }
    size_t n = d->serialize(det_file(), DET_FILE_MAX);
    if (!d->any() || n == 0) {
        if (notes_folder_there()) {
            fsops::remove_path(Drive::EXTERNAL_SD_CARD, path);
        }
        det_have = false;
        return true;
    }
    if (!notes_folder_there()) {
        notes_message = NOTES_NO_FOLDER;
        return false;
    }
    bool ok = false;
    FileHandle& f = *notes_handle();
    if (f.open(Drive::EXTERNAL_SD_CARD, path, false)) {
        ok = f.write(det_file(), n) == (int) n;
        ok = f.close() && ok;
    }
    if (!ok) {
        fsops::remove_path(Drive::EXTERNAL_SD_CARD, path);
        set_last_error("Cannot save", "details");
        det_have = false;
    } else {
        det_have = true;
    }
    return ok;
}

// ---- the details menu
// The text of a field to edit as it is now (what the screen shows, from the information)
NOTES_FUNC static const char* det_current(const gameinfo::Info* info, char field) {
    switch (field) {
        case 'h': return info->house;
        case 'y': return info->year;
        case 'c': return info->controls;
        default: return info->text;
    }
}

// Starts the box for a field. False (with a message) if what is there does not fit it.
NOTES_FUNC static bool det_open_field(const gameinfo::Info* info, char field) {
    const char* cur = det_current(info, field);
    char* file = det_file();
    if (field == 'h' || field == 'y') {
        DetEditorOne* one = new (snap_load_buffer + DET_EDITOR_OFFSET) DetEditorOne();
        one->set_limit(field == 'y' ? gameinfo::YEAR_MAX + 2 : 0);
        if (!one->load(cur, strlen(cur))) {
            notes_message = "Too long to edit here";
            return false;
        }
        return true;
    }
    // the text is cut into lines of the box's width, each line ending where it was cut
    size_t n = 0;
    gameinfo::Wrap w(cur, 30);
    char line[40];
    while (w.next(line)) {
        size_t len = strlen(line);
        if (n + len + 1 >= DET_FILE_MAX) {
            break;
        }
        memcpy(file + n, line, len);
        n += len;
        file[n++] = '\n';
    }
    DetEditorBig* big = new (snap_load_buffer + DET_EDITOR_OFFSET) DetEditorBig();
    big->set_limit(field == 'c' ? gameinfo::CONTROLS_MAX : gameinfo::TEXT_MAX);
    if (!big->load(file, n)) {
        notes_message = "Too long for this screen";
        return false;
    }
    return true;
}

// The box's save: the field into the game's file, keeping the other parts that are there
NOTES_FUNC static bool det_save_field(const char* game, char field, gameinfo::Info* info) {
    gameinfo::Details* d = det_data();
    det_read(game, d);
    char* file = det_file();
    size_t n;
    if (field == 'h' || field == 'y') {
        DetEditorOne* one = reinterpret_cast<DetEditorOne*>(snap_load_buffer + DET_EDITOR_OFFSET);
        n = one->text(file, DET_FILE_MAX);
        if (n > 0 && file[n - 1] == '\n') {
            file[--n] = '\0';
        }
    } else {
        DetEditorBig* big = reinterpret_cast<DetEditorBig*>(snap_load_buffer + DET_EDITOR_OFFSET);
        n = big->text(file, DET_FILE_MAX);
    }
    switch (field) {
        case 'h': d->set_house(file); break;
        case 'y': d->set_year(file); break;
        case 'c': d->set_controls(file); break;
        default: d->set_text(file); break;
    }
    if (!det_write(game, d)) {
        return false;
    }
    info->apply(*d);                // what is on the screen from now on
    return true;
}

NOTES_FUNC static void det_hub_draw(uint8_t* screen, const gameinfo::Info* info, const char* game) {
    memset(screen, 0, 6144);
    memset(screen + 6144, 0x07, 768);
    memset(screen + 6144, gameinfo::ATTR_BANNER, 32);
    char title[40];
    snprintf(title, sizeof(title), "Edit: %s", game);
    help_put_text(screen, 0, 1, title, (int) strlen(title) > 30 ? 30 : (int) strlen(title), gameinfo::ATTR_BANNER);
    auto put = [&](int row, int col, const char* s, uint8_t attr) {
        help_put_text(screen, row, col, s, (int) strlen(s) > 30 - col + 1 ? 30 - col + 1 : (int) strlen(s), attr);
    };
    char s[40];
    put(2, 1, "H-Software house", gameinfo::ATTR_HEADING);
    put(3, 3, info->house[0] != '\0' ? info->house : "(none)", info->house[0] != '\0' ? gameinfo::ATTR_TEXT : gameinfo::ATTR_DIM);
    put(4, 1, "Y-Year", gameinfo::ATTR_HEADING);
    put(5, 3, info->year[0] != '\0' ? info->year : "(none)", info->year[0] != '\0' ? gameinfo::ATTR_TEXT : gameinfo::ATTR_DIM);
    snprintf(s, sizeof(s), "C-Controls  %u/%u", (unsigned) info->ncontrols, (unsigned) gameinfo::CONTROLS_MAX);
    put(6, 1, s, gameinfo::ATTR_HEADING);
    snprintf(s, sizeof(s), "I-Instructions  %u/%u", (unsigned) info->ntext, (unsigned) gameinfo::TEXT_MAX);
    put(7, 1, s, gameinfo::ATTR_HEADING);
    put(8, 1, info->nnotes > 0 ? "N-My notes  (there are notes)" : "N-My notes  (none yet)", gameinfo::ATTR_HEADING);
    if (det_have) {
        put(10, 1, "R-Take back what you typed in", gameinfo::ATTR_HEADING);
    }
    put(12, 1, "What you type in is kept in", gameinfo::ATTR_DIM);
    put(13, 1, "the Game Notes folder as", gameinfo::ATTR_DIM);
    put(14, 1, "<game>.info.txt and is shown", gameinfo::ATTR_DIM);
    put(15, 1, "instead of the same part of", gameinfo::ATTR_DIM);
    put(16, 1, "the Instructions file.", gameinfo::ATTR_DIM);
    const char* keys = "X-Back";
    if (det_confirm) {
        keys = "Take back all? Y-Yes N-No";
    }
    if (notes_message != nullptr) {
        put(21, 1, notes_message, gameinfo::ATTR_KEYS);
    }
    help_put_text(screen, 23, 1, keys, (int) strlen(keys), gameinfo::ATTR_KEYS);
}

NOTES_FUNC static const char* det_field_name(char field) {
    switch (field) {
        case 'h': return "Software house";
        case 'y': return "Year";
        case 'c': return "Controls";
        default: return "Instructions";
    }
}

template <class E>
NOTES_FUNC static void det_field_draw_lines(uint8_t* screen, const E& ed) {
    for (int i = 0; i < E::VIEW && ed.top() + i < ed.line_count(); i++) {
        const char* l = ed.line(ed.top() + i);
        help_put_text(screen, 2 + i, 1, l, (int) strlen(l), gameinfo::ATTR_TEXT);
    }
    screen[6144 + (2 + ed.row() - ed.top()) * 32 + 1 + ed.col()] = 0xC7;       // bright white, flashing
    char count[32];
    if (ed.limit() > 0) {
        snprintf(count, sizeof(count), "%u of %u letters", (unsigned) ed.used(), (unsigned) ed.limit());
        help_put_text(screen, 1, 1, count, (int) strlen(count), gameinfo::ATTR_DIM);
    }
    const char* keys = ed.prompting() ? "Y-Save  N-Discard  other-Back" : "BREAK-Finish";
    if (notes_message != nullptr) {
        help_put_text(screen, 21, 1, notes_message, (int) strlen(notes_message), gameinfo::ATTR_KEYS);
    }
    help_put_text(screen, 23, 1, keys, (int) strlen(keys), gameinfo::ATTR_KEYS);
}

NOTES_FUNC static void det_field_draw(uint8_t* screen, const char* game) {
    memset(screen, 0, 6144);
    memset(screen + 6144, 0x07, 768);
    memset(screen + 6144, gameinfo::ATTR_BANNER, 32);
    char title[48];
    snprintf(title, sizeof(title), "%s: %s", det_field_name(det_field), game);
    help_put_text(screen, 0, 1, title, (int) strlen(title) > 30 ? 30 : (int) strlen(title), gameinfo::ATTR_BANNER);
    if (det_field == 'h' || det_field == 'y') {
        det_field_draw_lines(screen, *reinterpret_cast<const DetEditorOne*>(snap_load_buffer + DET_EDITOR_OFFSET));
        const char* hint = det_field == 'y' ? "The year, up to 8 letters" : "The name, up to 30 letters";
        help_put_text(screen, 4, 1, hint, (int) strlen(hint), gameinfo::ATTR_DIM);
        help_put_text(screen, 6, 1, "ENTER finishes. DELETE deletes.", 31, gameinfo::ATTR_DIM);
        help_put_text(screen, 7, 1, "Leave it empty to show nothing.", 31, gameinfo::ATTR_DIM);
    } else {
        det_field_draw_lines(screen, *reinterpret_cast<const DetEditorBig*>(snap_load_buffer + DET_EDITOR_OFFSET));
        help_put_text(screen, 20, 1, "Move: cursor keys or CAPS+5-8", 29, gameinfo::ATTR_DIM);
        help_put_text(screen, 22, 1, "ENTER new line  DELETE deletes", 30, gameinfo::ATTR_DIM);
    }
}

// The editor's screen: the lines, the cursor flashing, the keys
static void notes_draw(uint8_t* screen, const notes::Editor& ed, const char* game) {
    memset(screen, 0, 6144);
    memset(screen + 6144, 0x07, 768);
    memset(screen + 6144, gameinfo::ATTR_BANNER, 32);
    char title[40];
    snprintf(title, sizeof(title), "My notes: %s", game);
    help_put_text(screen, 0, 1, title, (int) strlen(title) > 30 ? 30 : (int) strlen(title), gameinfo::ATTR_BANNER);
    const bool empty = ed.line_count() == 1 && ed.line(0)[0] == '\0';
    if (!notes_edit && empty) {
        help_put_text(screen, 2, 1, "No notes yet.", 13, gameinfo::ATTR_DIM);
    }
    for (int i = 0; i < ed.line_count(); i++) {
        help_put_text(screen, 2 + i, 1, ed.line(i), (int) strlen(ed.line(i)), gameinfo::ATTR_TEXT);
    }
    const char* keys = "E-Edit  X-Back";                     // looking: the notes cannot be changed
    if (notes_edit) {
        screen[6144 + (2 + ed.row()) * 32 + 1 + ed.col()] = 0xC7;       // bright white, flashing
        keys = ed.prompting() ? "Y-Save  N-Discard  other-Back" : "BREAK-Finish";
        help_put_text(screen, 22, 1, "ENTER new line  DELETE deletes", 30, gameinfo::ATTR_DIM);
        help_put_text(screen, 20, 1, "Move: cursor keys or CAPS+5-8", 29, gameinfo::ATTR_DIM);
    }
    if (notes_message != nullptr) {
        help_put_text(screen, 21, 1, notes_message, (int) strlen(notes_message), gameinfo::ATTR_KEYS);
    }
    help_put_text(screen, 23, 1, keys, (int) strlen(keys), gameinfo::ATTR_KEYS);
}

// One step of the edit screens, kept apart from nmi_action_game_info so the SD card's
// file calls do not run on top of that function's big stack frame.
// op 3 starts (M: the details menu, the hub), op 4 gives a key. From the hub: N is the notes
// (viewed first, E edits), H Y C I a box for the field, R takes back what was typed in, X ends.
// True when it has ended and the information should be shown again at the notes page.
NOTES_FUNC static bool notes_open(const char* name, char* file, notes::Editor* editor) {
    // the notes are shown, not to be changed until E is pressed
    notes_edit = false;
    size_t got = notes_read(name, file, notes::FILE_MAX + 1);
    new (editor) notes::Editor();
    if (got > notes::FILE_MAX || !editor->load(file, got)) {
        notes_message = "Notes too long for this screen";
        return false;
    }
    edit_kind = EK_NOTES;
    return true;
}

NOTES_FUNC static bool notes_step(int op, const char* name, uint8_t key) {
    notes::Editor* editor = reinterpret_cast<notes::Editor*>(snap_load_buffer + NOTES_EDITOR_OFFSET);
    char* file = reinterpret_cast<char*>(snap_load_buffer) + NOTES_FILE_OFFSET;
    gameinfo::Info* info = reinterpret_cast<gameinfo::Info*>(snap_load_buffer + INFO_DATA_OFFSET);
    if (op == 3) {
        // M: the menu of what can be edited
        notes_editing = notes_edit = notes_saved = det_saved = det_confirm = false;
        if (name[0] == '\0' || !sd_card_mounted) {
            notes_message = "No SD card or no game";
            return false;
        }
        if (!notes_folder_there()) {
            notes_message = NOTES_NO_FOLDER;
            return false;
        }
        char path[screengrab::NAME_MAX + 24];
        det_have = det_path(name, path, sizeof(path)) && fsops::kind(Drive::EXTERNAL_SD_CARD, path) == fsops::KIND_FILE;
        edit_kind = EK_HUB;
        notes_editing = true;
        return false;
    }
    if (!notes_editing) {
        return false;
    }
    if (edit_kind == EK_HUB) {
        if (det_confirm) {
            det_confirm = false;
            if (key == 'y' || key == 'Y') {
                gameinfo::Details* d = det_data();
                d->clear();
                if (det_write(name, d)) {
                    det_saved = true;
                    info_loaded = false;            // the information as the Instructions file has it
                }
            }
            return false;
        }
        switch (key) {
            case 'n': case 'N':
                notes_open(name, file, editor);
                break;
            case 'h': case 'H': case 'y': case 'Y': case 'c': case 'C': case 'i': case 'I': {
                char f = (char) (key | 0x20);
                if (info_loaded && det_open_field(info, f)) {
                    det_field = f;
                    edit_kind = EK_FIELD;
                }
                break;
            }
            case 'r': case 'R':
                if (det_have) {
                    det_confirm = true;
                }
                break;
            case 'x': case 'X': case notes::KEY_BREAK: case notes::KEY_EDIT:
                notes_editing = false;              // back to the information
                info_loaded = false;
                if (det_saved && !notes_saved) {
                    (rom_data + 16384)[3] = 0;      // the first page, where the details are
                }
                return notes_saved;                 // at the notes page if they were changed
            default:
                break;
        }
        return false;
    }
    if (edit_kind == EK_FIELD) {
        bool one = det_field == 'h' || det_field == 'y';
        // the two editors' results are the same three numbers (CONTINUE, SAVE, DISCARD)
        int r = one ? (int) reinterpret_cast<DetEditorOne*>(snap_load_buffer + DET_EDITOR_OFFSET)->key(key)
                    : (int) reinterpret_cast<DetEditorBig*>(snap_load_buffer + DET_EDITOR_OFFSET)->key(key);
        if (r == notes::Editor::CONTINUE) {
            return false;
        }
        if (r == notes::Editor::SAVE) {
            if (!det_save_field(name, det_field, info)) {
                if (notes_message == nullptr) {
                    notes_message = "Could not save it";
                }
                return false;                       // the box stays open: nothing is lost
            }
            LOG("Details saved for %s\n", name);
            det_saved = true;
        }
        edit_kind = EK_HUB;
        return false;
    }
    if (!notes_edit) {
        if (key == 'e' || key == 'E') {
            notes_edit = true;
        } else if (key == 'x' || key == 'X' || key == notes::KEY_BREAK || key == notes::KEY_EDIT) {
            edit_kind = EK_HUB;                     // back to the menu
            info_loaded = false;
        }
        return false;
    }
    notes::Editor::Result r = editor->key(key);
    if (r == notes::Editor::CONTINUE) {
        return false;
    }
    if (r == notes::Editor::SAVE) {
        size_t n = editor->text(file, notes::FILE_MAX + 2);
        if (!notes_write(name, file, n)) {
            if (notes_message == nullptr) {
                notes_message = "Could not save the notes";
            }
            return false;
        }
        LOG("Notes saved for %s\n", name);
        notes_saved = true;
    } else {
        // not saved: the notes as they are in the file
        size_t got = notes_read(name, file, notes::FILE_MAX + 1);
        new (editor) notes::Editor();
        editor->load(file, got <= notes::FILE_MAX ? got : 0);
    }
    notes_edit = false;                         // and the notes are shown again
    return false;
}

// The text of a page. Apart so its big working space is not part of the frame of the
// function that reads the SD card (core 0's stack is only 2K)
NOTES_FUNC static void info_paint_text(uint8_t* screen, const gameinfo::Info& info, int page) {
    gameinfo::draw_text(info, page, [&](int row, int col, const char* text, int len, uint8_t attr) {
        help_put_text(screen, row, col, text, len, attr);
    });
}

// The information of a game: its entry in the Instructions file, what was typed in on the
// Spectrum (Details) over it, the notes and, for a tape, its summary
NOTES_FUNC static void info_load(gameinfo::Info* info, const char* name, int mode) {
    new (info) gameinfo::Info();
    if (name[0] != '\0' && sd_card_mounted) {
        info_read_file(info, name);
        gameinfo::Details* d = det_data();
        if (det_read(name, d)) {
            info->apply(*d);
        }
        char* file = reinterpret_cast<char*>(snap_load_buffer) + NOTES_FILE_OFFSET;
        size_t got = notes_read(name, file, gameinfo::NOTES_SHOW_MAX);
        info->set_notes(file, got);
    }
    if (mode == 0 && tape_sum_valid) {
        char* text = reinterpret_cast<char*>(snap_load_buffer) + NOTES_FILE_OFFSET;
        const char* say;
        tape_text(text, gameinfo::TAPE_MAX + 1, &say);
        info->set_tape(text, say);
    }
}

// Shows the game information (the editor's screen while the notes are edited). `ended`: the
// notes editor has just finished, so go to the last page, where the notes are.
NOTES_FUNC static void nmi_game_info_show(bool ended) {
    uint8_t* nmi_rom_data = rom_data + 16384;
    int mode = nmi_rom_data[2];
    int page = nmi_rom_data[3];
    int pic = nmi_rom_data[4];
    int op = nmi_rom_data[5];       // 1 asks whether to delete the grab shown, 2 deletes it
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer) + INFO_SCREEN_OFFSET;
    uint8_t* grab = reinterpret_cast<uint8_t*>(snap_load_buffer) + GRAB_SCR_OFFSET;
    gameinfo::Info* info = reinterpret_cast<gameinfo::Info*>(snap_load_buffer + INFO_DATA_OFFSET);
    const char* name = mode == 1 ? (reload_known ? reload_name : "") : info_name;
    notes::Editor* editor = reinterpret_cast<notes::Editor*>(snap_load_buffer + NOTES_EDITOR_OFFSET);
    if (ended) {
        page = 255;                 // the last page
        info_full = false;
    }
    if (op == 3 || op == 4) {
        op = 0;                     // the notes editor's, done in nmi_action_game_info
    }
    if (notes_editing) {
        uint8_t* edit_screen = reinterpret_cast<uint8_t*>(snap_load_buffer) + INFO_SCREEN_OFFSET;
        if (edit_kind == EK_NOTES) {
            notes_draw(edit_screen, *editor, name);
        } else {
            if (!info_loaded) {
                info_load(info, name, mode);
                info_loaded = true;
            }
            if (edit_kind == EK_HUB) {
                det_hub_draw(edit_screen, info, name);
            } else {
                det_field_draw(edit_screen, name);
            }
        }
        preview_screen = edit_screen;
        nmi_rom_data[1] = 3;
        nmi_rom_data[0] = 1;
        return;
    }
    const bool fresh = !info_loaded || (page == 0 && pic == 0);
    if ((page == 0 || !info_loaded) && !(info_full && info_loaded && !fresh)) {
        info_load(info, name, mode);
    }
    if (mode == 0 && fresh) {
        grabs_find(name);
    }
    info_loaded = true;
    int pages = gameinfo::page_count(*info);
    if (page >= pages) page = pages - 1;
    const bool file_picture = mode == 0 && info_source != nullptr;
    int pictures = mode == 1 ? 0 : (file_picture ? 1 : 0) + grab_count;
    if (pic >= pictures) pic = 0;
    if (op == 5) {                  // F: the picture on the whole screen, or back
        op = 0;
        if (info_full) {
            info_full = false;
        } else if (mode == 1 ? screen_stashed : pictures > 0) {
            info_full = true;
        } else {
            notes_message = "No picture to show";
        }
    }
    if (info_full) {
        const uint8_t* full = nullptr;
        if (mode == 1) {
            full = screen_stashed ? screen_stash : nullptr;
        } else if (pictures > 0) {
            if (file_picture && pic == 0) {
                full = info_source;
            } else if (grab_load(name, pic - (file_picture ? 1 : 0), grab)) {
                full = grab;
            }
        }
        if (full != nullptr) {
            memcpy(screen, full, 6912);
            preview_screen = screen;
            nmi_rom_data[1] = 0;
            nmi_rom_data[3] = 0;
            nmi_rom_data[0] = (uint8_t) (1 | ((pictures > 15 ? 15 : pictures) << 4));
            return;
        }
        info_full = false;
    }
    // a screen grab is showing (the picture is on the first page): D can delete it
    bool grab_shown = mode == 0 && page == 0 && pictures > 0 && !(file_picture && pic == 0);
    bool asking = false;
    if (op == 1 && grab_shown) {
        asking = true;
    }
    else if (op == 2 && grab_shown) {
        char base[screengrab::NAME_MAX + 1];
        int k = pic - (file_picture ? 1 : 0);
        if (screengrab::base_name(name, base) > 0 && k >= 0 && k < grab_count) {
            screengrab::path_in(grab_folder(k), base, grab_number[k], ".scr", grab_path, sizeof(grab_path));
            LOG("Delete the grab %s\n", grab_path);
            fsops::remove_path(Drive::EXTERNAL_SD_CARD, grab_path);
        }
        grabs_find(name);
        pictures = (file_picture ? 1 : 0) + grab_count;
        if (pic >= pictures) pic = 0;
        grab_shown = pictures > 0 && !(file_picture && pic == 0);
    }

    memset(screen, 0, 6144);
    memset(screen + 6144, 0x07, 768);
    memset(screen + 6144, gameinfo::ATTR_BANNER, 32);
    // the numbers at the right of the title row
    char numbers[24];
    numbers[0] = '\0';
    if (pictures > 1) {
        snprintf(numbers, sizeof(numbers), "Pic %d/%d", pic + 1, pictures);
    }
    if (pages > 1) {
        size_t used = strlen(numbers);
        snprintf(numbers + used, sizeof(numbers) - used, "%sPg %d/%d", used ? " " : "", page + 1, pages);
    }
    int numberlen = (int) strlen(numbers);
    const char* title = name[0] != '\0' ? name : "Instructions";
    int titlelen = (int) strlen(title);
    int room = 30 - numberlen - (numberlen ? 1 : 0);
    if (titlelen > room) titlelen = room;
    help_put_text(screen, 0, 1, title, titlelen, gameinfo::ATTR_BANNER);
    help_put_text(screen, 0, 31 - numberlen, numbers, numberlen, gameinfo::ATTR_BANNER);
    if (page == 0) {
        const uint8_t* thumb = nullptr;
        if (mode == 1) {
            thumb = screen_stashed ? screen_stash : nullptr;
        } else if (pictures > 0) {
            if (file_picture && pic == 0) {
                thumb = info_source;
            } else if (grab_load(name, pic - (file_picture ? 1 : 0), grab)) {
                thumb = grab;
            }
        }
        if (thumb != nullptr) {
            gameinfo::draw_thumbnail(thumb, screen, gameinfo::THUMB_ROW, gameinfo::THUMB_COL);
        } else {
            help_put_text(screen, 6, 3, "No Picture", 10, gameinfo::ATTR_DIM);
        }
    }
    info_paint_text(screen, *info, page);
    char keys[40];
    gameinfo::key_help(mode == 1, pages, keys, grab_shown, mode == 1 ? screen_stashed : pictures > 0);
    if (asking) {
        snprintf(keys, sizeof(keys), "Delete this grab?  Y-Yes  N-No");
    }
    if (notes_message != nullptr) {
        snprintf(keys, sizeof(keys), "%s", notes_message);
    }
    int keylen = (int) strlen(keys);
    help_put_text(screen, 23, keylen >= 32 ? 0 : 1, keys, keylen, gameinfo::ATTR_KEYS);
    // a software house too long for its place on the first page: the ROM scrolls it there
    static_assert(INFO_HOUSE_WIDTH == gameinfo::CTRL_WIDTH, "the ROM scrolls the software house in its column");
    static_assert(gameinfo::HOUSE_MAX < INFO_HOUSE_SIZE, "INFO_HOUSE holds the whole name");
    if (page == 0 && info->any() && strlen(info->house) > (size_t) INFO_HOUSE_WIDTH) {
        snprintf(reinterpret_cast<char*>(nmi_rom_data) + INFO_HOUSE, INFO_HOUSE_SIZE, "%s", info->house);
    }
    preview_screen = screen;
    nmi_rom_data[1] = asking ? 1 : 0;       // the ROM then waits for Y or N
    nmi_rom_data[3] = (uint8_t) page;       // the page as it was kept to the pages there are
    nmi_rom_data[0] = (uint8_t) (pages | ((pictures > 15 ? 15 : pictures) << 4));
}

// ---- C on the NMI menu: set up the SD card (sdsetup.h). The ROM shows it as the game
// information with mode 2: D asks "Set up this card?", Y does it (op 2), any other key does not.
static uint8_t sd_setup_result[sdsetup::FOLDER_COUNT + 1];     // each folder, then the Instructions file
static bool sd_setup_done = false;

// The folders, then the example Instructions. Called from a small frame, like notes_step, as
// the SD card's calls need the stack.
NOTES_FUNC static void sd_setup_run() {
    bool scraper_ok = false;
    for (int i = 0; i < sdsetup::FOLDER_COUNT; i++) {
        const sdsetup::Folder& f = sdsetup::FOLDERS[i];
        trace_step('S', i);         // a note in the flash: if the Picoface freezes here, Diagnostics says where
        uint8_t kind = fsops::kind(Drive::EXTERNAL_SD_CARD, f.path);
        uint8_t r = sdsetup::FAILED;
        if (kind == fsops::KIND_DIR) {
            r = sdsetup::THERE;
        } else if (kind == fsops::KIND_NONE && make_sd_folder(f.path) == fsops::OK) {
            r = sdsetup::MADE;
            if (f.hidden) {
                f_chmod(f.path, AM_HID, AM_HID);
            }
        }
        sd_setup_result[i] = r;
        if (r == sdsetup::FAILED) {
            set_last_error("Cannot make", f.path + 1);
        }
        if (i == 0) {
            scraper_ok = r != sdsetup::FAILED;
        }
    }
    // an Instructions file is only made if there is none, as one in the root would be hidden by it
    trace_step('S', sdsetup::FOLDER_COUNT);
    uint8_t r = sdsetup::THERE;
    bool have = false;
    for (int i = 0; i < sdsetup::INSTRUCTIONS_PATH_COUNT && !have; i++) {
        have = fsops::kind(Drive::EXTERNAL_SD_CARD, sdsetup::INSTRUCTIONS_PATHS[i]) != fsops::KIND_NONE;
    }
    if (!have) {
        r = sdsetup::FAILED;
        if (scraper_ok) {
            FileHandle& f = *notes_handle();
            if (f.open(Drive::EXTERNAL_SD_CARD, sdsetup::EXAMPLE_PATH, false)) {
                bool ok = f.write(sdsetup::EXAMPLE, sizeof(sdsetup::EXAMPLE) - 1) == (int) (sizeof(sdsetup::EXAMPLE) - 1);
                ok = f.close() && ok;
                r = ok ? sdsetup::MADE : sdsetup::FAILED;
                if (!ok) {
                    fsops::remove_path(Drive::EXTERNAL_SD_CARD, sdsetup::EXAMPLE_PATH);
                }
            }
        }
    }
    sd_setup_result[sdsetup::FOLDER_COUNT] = r;
    trace_step('Z', 0);             // finished
    LOG("SD card set up\n");
}

NOTES_FUNC static void sd_setup_draw(uint8_t* screen, int state, bool card) {
    memset(screen, 0, 6144);
    memset(screen + 6144, 0x07, 768);
    memset(screen + 6144, gameinfo::ATTR_BANNER, 32);
    // the result's heading says how it went: created, failed, or already there
    bool failed = false, made_any = false;
    for (int i = 0; state == 2 && card && i <= sdsetup::FOLDER_COUNT; i++) {
        failed = failed || sd_setup_result[i] == sdsetup::FAILED;
        made_any = made_any || sd_setup_result[i] == sdsetup::MADE;
    }
    const char* title = state != 2 || !card ? "Set up SD card" : failed ? "SD card creation failed" : made_any ? "SD card created" : "SD card already set up";
    help_put_text(screen, 0, 1, title, (int) strlen(title), gameinfo::ATTR_BANNER);
    auto line = [&](int row, const char* text, uint8_t attr = gameinfo::ATTR_TEXT) {
        help_put_text(screen, row, 1, text, (int) strlen(text), attr);
    };
    const char* keys = "X-Exit";
    if (!card) {
        line(2, "No SD card found.");
        line(4, "Put the card in, then try");
        line(5, "again.");
    } else if (state == 2) {
        for (int i = 0; i <= sdsetup::FOLDER_COUNT; i++) {
            const bool inst = i == sdsetup::FOLDER_COUNT;
            const char* name = inst ? "Instructions.txt" : sdsetup::FOLDERS[i].path + 1;
            char text[40];
            const uint8_t r = sd_setup_result[i];
            const char* how = r == sdsetup::MADE ? "made" : r == sdsetup::FAILED ? "FAILED" : inst ? "kept yours" : "there already";
            snprintf(text, sizeof(text), "%s%s", name, inst ? "" : "/");
            line(2 + i, text);
            help_put_text(screen, 2 + i, 18, how, (int) strlen(how), r == sdsetup::FAILED ? gameinfo::ATTR_KEYS : gameinfo::ATTR_HEADING);
        }
        if (failed) {
            line(11, "SD card creation failed.");
            line(12, "Is the card full or locked?");
            line(13, "See Help, Diagnostics.");
        } else {
            line(11, made_any ? "SD card created." : "Nothing new was needed.");
            line(12, "Put your game information");
            line(13, "in Scraper/Instructions.txt.");
        }
    } else {
        line(2, "Makes these on the SD card.");
        line(3, "Nothing there is replaced.");
        for (int i = 0; i < sdsetup::FOLDER_COUNT; i++) {
            char name[24];
            snprintf(name, sizeof(name), "%s/", sdsetup::FOLDERS[i].path + 1);
            line(5 + i, name, gameinfo::ATTR_HEADING);
            help_put_text(screen, 5 + i, 15, sdsetup::FOLDERS[i].what, (int) strlen(sdsetup::FOLDERS[i].what), gameinfo::ATTR_TEXT);
        }
        line(12, "An example Instructions.txt");
        line(13, "is added if there is none.");
        keys = state == 1 ? "Set up this card?  Y-Yes  N-No" : "D-Set up  X-Exit";
    }
    help_put_text(screen, 23, 1, keys, (int) strlen(keys), gameinfo::ATTR_KEYS);
}

static bool sd_setup_ran = false;

// The main loop calls this before processNmiRequest(): the set up (Y on the Set up SD card screen)
// is done from the main loop's own frame, not from inside processNmiRequest() with its big frame
// and the ones under it, as core 0 has only 2K of stack and the SD card's calls (f_mkdir) need a
// lot of it.  nmi_sd_setup() then only draws the result.
NOTES_FUNC static void sd_setup_early() {
    const uint8_t* mailbox = rom_data + 16384;
    if (mailbox[1] == ACTION_GAME_INFO && mailbox[2] == 2 && mailbox[5] == 2 && sd_card_mounted) {
        sd_setup_run();
        sd_setup_ran = true;
    }
}

NOTES_FUNC static void nmi_sd_setup(int op) {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer) + INFO_SCREEN_OFFSET;
    const bool card = sd_card_mounted;
    int state = 0;                  // 0 the list, 1 asking, 2 done
    if (op == 0) {
        sd_setup_done = false;      // a new visit
    } else if (op == 1 && card) {
        sd_setup_done = false;
        state = 1;
    } else if (op == 2 && card) {
        if (!sd_setup_ran) {
            sd_setup_run();         // (the main loop has usually done it already: sd_setup_early)
        }
        sd_setup_ran = false;
        sd_setup_done = true;
    }
    if (sd_setup_done) {
        state = 2;
    }
    sd_setup_draw(screen, state, card);
    preview_screen = screen;
    nmi_rom_data[1] = state == 1 ? 1 : 0;       // the ROM then waits for Y or N
    nmi_rom_data[3] = 0;
    nmi_rom_data[0] = 1;                        // one page, no pictures
}

// ACTION_GAME_INFO. M: edit the game's notes. op 3 starts, op 4 is a key (byte 6) for the
// editor. While it is going byte 1 of the mailbox is 3 and the screen is the editor's.
// The editor's step is done here, ahead of the drawing function, which has a big stack
// frame: the SD card's file calls need the stack that is left, and core 0 has only 2K.
static bool conv_op(uint8_t mode, uint8_t key, uint8_t& keep);
static bool conv_result_shown = false;      // conv_msg is on the key row (see below)
static uint8_t conv_result_pic = 0;         // the picture shown with it
static uint32_t conv_result_game = 0;       // a hash of the tape's path
static uint32_t conv_path_hash(const char* p) {
    uint32_t h = 0x811C9DC5u;
    while (*p) h = (h ^ (uint8_t) *p++) * 0x01000193u;
    return h;
}
static char conv_msg[40];                   // the message on the key row afterwards
static void nmi_action_game_info() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    nmi_rom_data[INFO_HOUSE] = 0;       // no software house to scroll, unless the first page says so
    int op = nmi_rom_data[5];
    if (nmi_rom_data[2] == 2) {         // C on the menu: set up the SD card
        notes_editing = notes_edit = false;
        nmi_sd_setup(op);
        return;
    }
    bool ended = false;
    notes_message = nullptr;
    // The result of a conversion stays on the key row while the first page's pictures change
    // by themselves (op 0, page 0: the converter may have just saved some), until a key
    // (and the same game: a new visit also asks with op 0, page 0, but picture 0)
    if (conv_result_shown) {
        uint8_t pic = nmi_rom_data[4];
        if (op == 0 && nmi_rom_data[3] == 0 && pic != conv_result_pic && conv_result_game == conv_path_hash(info_path)) {
            notes_message = conv_msg;
            conv_result_pic = pic;
        }
        else conv_result_shown = false;
    }
    if (op == 6) {
        // C: convert the tape. The converter shows its own screen while it runs (and says
        // 6 in byte 1, so the ROM asks again), or the normal page with a message
        notes_editing = notes_edit = false;
        uint8_t keep = 0;
        if (conv_op(nmi_rom_data[2], nmi_rom_data[6], keep)) {
            return;
        }
        nmi_game_info_show(false);
        nmi_rom_data[1] = keep;         // 7: the ROM waits for a key for the converter
        return;
    }
    if (op != 3 && op != 4) {
        // the ROM is not in the notes screen (it asks for op 3 and then op 4 for every key
        // in it): a new visit, or the NMI was left while the notes were up
        notes_editing = notes_edit = false;
    }
    if (op == 3 || op == 4) {
        const char* name = nmi_rom_data[2] == 1 ? (reload_known ? reload_name : "") : info_name;
        ended = notes_step(op, name, nmi_rom_data[6]);
    }
    nmi_game_info_show(ended);
}

// ACTION_SCREEN_GRAB: saves the game's screen (as it was when the menu opened) in the
// Screen Grabs folder of the SD card, a folder made if it is not there (it is not hidden: the Browse Games list leaves it out), as
// <game>.scr, or <game>1.scr, <game>2.scr ... if the name is taken. <game> is the
// game last loaded (without " Save Game"), or "Screen" if there is none.
// WORD param1 = where to put the message "Saved as <name>"
// Status 0 saved, 1 no SD card (or the PC has it), 2 could not save, 3 too many grabs of
// the game, 4 there is no screen to save.
static void nmi_action_screen_grab() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t dest = nmi_rom_data[2] | (nmi_rom_data[3] << 8);
    char* message = reinterpret_cast<char*>(nmi_rom_data) + dest;
    if (dest > 16384 - 40) {
        nmi_rom_data[0] = 2;
        return;
    }
    if (!sd_card_mounted) {
        nmi_rom_data[0] = 1;
        return;
    }
    if (!screen_stashed) {
        nmi_rom_data[0] = 4;
        return;
    }
    char base[screengrab::NAME_MAX + 1];
    if (!reload_known || screengrab::base_name(reload_name, base) == 0) {
        snprintf(base, sizeof(base), "Screen");
    }
    uint8_t made = make_sd_folder(screengrab::FOLDER);
    if (made != fsops::OK && made != fsops::NAME_IN_USE) {      // (not hidden: Browse Games just does not list it)
        set_last_error("Cannot make", "Screen Grabs");
        nmi_rom_data[0] = 2;
        return;
    }
    int number = -1;
    for (int i = 0; i < screengrab::MAX_SCAN && number < 0; i++) {
        screengrab::path_for(base, i, ".scr", grab_path, sizeof(grab_path));
        if (fsops::kind(Drive::EXTERNAL_SD_CARD, grab_path) == fsops::KIND_NONE) {
            number = i;
        }
    }
    if (number < 0) {
        nmi_rom_data[0] = 3;
        return;
    }
    bool ok = false;
    FileHandle f;
    if (f.open(Drive::EXTERNAL_SD_CARD, grab_path, false)) {
        ok = f.write(screen_stash, screengrab::SCR_SIZE) == (int) screengrab::SCR_SIZE;
        ok = f.close() && ok;
    }
    if (!ok) {
        fsops::remove_path(Drive::EXTERNAL_SD_CARD, grab_path);
        set_last_error("Cannot save", "screen grab");
        nmi_rom_data[0] = 2;
        return;
    }
    screengrab::saved_message(base, number, message, 36);
    LOG("Screen grab %s\n", grab_path);
    nmi_rom_data[0] = 0;
}

// ACTION_RECENT_LIST: draws the games played recently as a screen in snap_load_buffer
// (which ACTION_PREVIEW_READ then fetches): the title, then 1 to 9 and 0 with the name,
// the type and the drive. A game on the SD card is dim while the card is not there.
// Status: the number of games (0 to 10)
static void nmi_action_recent_list() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer) + INFO_SCREEN_OFFSET;
    extras::RecentList* list = recent_read();
    const uint8_t mode = nmi_rom_data[2];
    if (mode >= 2) {
        // V then a number: get the game's own screen ready for ACTION_GAME_INFO (mode 0),
        // as ACTION_SNAP_PREVIEW does in Browse Games. Status 1: no such game
        int which = mode - 2;
        if (which >= list->count) {
            nmi_rom_data[0] = 1;
            return;
        }
        // copied first: reading the file uses the buffer the list is in
        const extras::RecentEntry& e = list->entry[which];
        char ext[extras::RECENT_EXT_MAX];
        snprintf(ext, sizeof(ext), "%s", e.ext);
        uint8_t drive = e.drive;
        snprintf(info_name, sizeof(info_name), "%s", e.name);
        snprintf(grab_path, sizeof(grab_path), "%s/%s.%s", e.dir, e.name, e.ext);
        preview_screen = nullptr;
        info_source = nullptr;
        uint8_t status = 1;
        if (!(drive == 1 && !sd_card_mounted)) {
            status = find_preview(drive, grab_path, ext);
            info_source = (preview_screen != nullptr && !gameinfo::screen_is_blank(preview_screen)) ? preview_screen : nullptr;
        }
        nmi_rom_data[0] = status;
        return;
    }
    // the whole names, RECENT_SLOT bytes each, where the ROM says: the screen shows 21 letters and the
    // ROM scrolls the rest (the slot size is RECENT_SLOT in nmirom.asm)
    const unsigned names_at = nmi_rom_data[3] | (nmi_rom_data[4] << 8);
    if (names_at != 0 && names_at + 10 * 64 <= 16384) {
        for (int i = 0; i < list->count; i++) {
            snprintf(reinterpret_cast<char*>(nmi_rom_data + names_at + i * 64), 64, "%s", list->entry[i].name);
        }
    }
    memset(screen, 0, 6144);
    memset(screen + 6144, 0x07, 768);
    memset(screen + 6144, gameinfo::ATTR_BANNER, 32);
    help_put_text(screen, 0, 1, "Recently played", 15, gameinfo::ATTR_BANNER);
    if (list->count == 0) {
        help_put_text(screen, 3, 1, "No games played yet", 19, gameinfo::ATTR_TEXT);
    }
    for (int i = 0; i < list->count; i++) {
        const extras::RecentEntry& e = list->entry[i];
        int row = 2 + i;
        char digit = i == 9 ? '0' : (char) ('1' + i);
        bool away = e.drive == 1 && !sd_card_mounted;
        help_put_text(screen, row, 0, &digit, 1, gameinfo::ATTR_HEADING);
        int namelen = (int) strlen(e.name);
        if (namelen > 21) namelen = 21;
        help_put_text(screen, row, 2, e.name, namelen, away ? gameinfo::ATTR_DIM : gameinfo::ATTR_TEXT);
        char ext[extras::RECENT_EXT_MAX];
        snprintf(ext, sizeof(ext), "%s", e.ext);
        for (char* c = ext; *c; c++) {
            if (*c >= 'A' && *c <= 'Z') *c = (char) (*c + 32);
        }
        help_put_text(screen, row, 24, ext, (int) strlen(ext), gameinfo::ATTR_DIM);
        help_put_text(screen, row, 29, e.drive == 1 ? "SD" : "FL", 2, gameinfo::ATTR_DIM);
    }
    const char* keys = list->count == 0 ? "X-Back" : mode == 1 ? "View which? 1-0  X-Cancel" : "1-0 Load  V-View  X-Back";
    help_put_text(screen, 23, 1, keys, (int) strlen(keys), gameinfo::ATTR_KEYS);
    preview_screen = screen;
    nmi_rom_data[0] = (uint8_t) list->count;
}


// ---- ACTION_UPDATE: replace the Picoface's own firmware with a .uf2 file from the SD card
//
// Three steps, each its own request so the Spectrum can show what is happening:
//   0 check: look for the one .uf2 in the root of the card, check it (fwupdate.h) and draw
//     the screen that asks the question. Status 0: ready to go, else the screen says why not
//   1 stage: copy the firmware into a file in the internal flash (fwnew.bin) and check it there
//   2 apply: copy it from there over the program, from RAM. Does not come back
// .uf2 files are only looked at here, nowhere else lists them as games.
static const char FW_STAGE_FILE[] = "fwnew.bin";
static const uint8_t FWU_CHECK = 0, FWU_STAGE = 1, FWU_APPLY = 2;
static const uint8_t FWU_OK = 0, FWU_NO_SD = 1, FWU_NO_FILE = 2, FWU_SEVERAL = 3, FWU_BAD_FILE = 4,
        FWU_NO_ROOM = 5, FWU_STAGE_FAILED = 6, FWU_NOT_READY = 7, FWU_MORE = 8, FWU_WRITE_FAILED = 9;
static const uint32_t FWU_SECTOR_AT = 69632;            // 4096 bytes in snap_load_buffer
// What the updater needs beyond fw_checker lives in snap_load_buffer (free while the menu is
// open, and nothing else is run between the steps): the sector copied, the map of the
// staged file, and the working space for reading the card
struct FwScratch {
    fwupdate::Map map;
    FIL file;
    DIR dir;
    FILINFO info;
    uint8_t block[512];
    char path[66];
};
static const uint32_t FWU_SCRATCH_AT = FWU_SECTOR_AT + 4096;
static_assert(FWU_SCRATCH_AT + sizeof(FwScratch) < SNAP_LOAD_BUFFER_SIZE, "updater scratch does not fit");
static FwScratch& fw_scratch() { return *reinterpret_cast<FwScratch*>(reinterpret_cast<uint8_t*>(snap_load_buffer) + FWU_SCRATCH_AT); }
static char fw_name[64];                                // the .uf2 found
static fwupdate::Checker fw_checker;
static bool fw_stage_open = false;                      // fwnew.bin is being written
static int fw_stage_out = -1;
static uint32_t fw_stage_pages = 0;                     // 256 byte pages copied so far
static bool fw_found = false;                           // the file was checked and is good
static bool fw_staged = false;                          // and is in the internal flash, mapped in fw_map

static const char* fw_error_text(fwupdate::Error e) {
    switch (e) {
        case fwupdate::BAD_SIZE: return "Not a whole .uf2 file";
        case fwupdate::BAD_BLOCK: return "The .uf2 file is damaged";
        case fwupdate::NOT_RP2040: return "Not made for the Pico (RP2040)";
        case fwupdate::BAD_ORDER: return "Blocks missing or out of order";
        case fwupdate::TOO_BIG: return "Too big for this PicoMenu";
        case fwupdate::TOO_SMALL: return "Too small to be a PicoMenu";
        case fwupdate::NOT_PICOFACE: return "Not a PicoMenu firmware";
        case fwupdate::BAD_BOOT2: return "The start-up code is damaged";
        default: return "Cannot use the file";
    }
}

static bool fw_has_uf2_ext(const char* name) {
    size_t n = strlen(name);
    return n > 4 && name[0] != '.' && strcasecmp(name + n - 4, ".uf2") == 0;
}

// Finds and checks the file. Returns a FWU_ status, error_out the reason of FWU_BAD_FILE
static uint8_t fw_check_file(fwupdate::Error* error_out) {
    fw_found = false;
    fw_staged = false;
    fw_stage_open = false;
    pico_remove(FW_STAGE_FILE);                         // left from an update that did not finish
    if (!sd_card_mounted) {
        return FWU_NO_SD;
    }
    walk_stop();
    FwScratch& w = fw_scratch();
    DIR& dir = w.dir;
    FILINFO& info = w.info;
    if (f_opendir(&dir, "/") != FR_OK) {
        return FWU_NO_SD;
    }
    int count = 0;
    while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != '\0') {
        if ((info.fattrib & (AM_DIR | AM_HID | AM_SYS)) == 0 && fw_has_uf2_ext(info.fname)) {
            if (count++ == 0) {
                snprintf(fw_name, sizeof(fw_name), "%s", info.fname);
            }
        }
    }
    f_closedir(&dir);
    if (count == 0) {
        return FWU_NO_FILE;
    }
    if (count > 1) {
        return FWU_SEVERAL;
    }
    FIL& file = w.file;
    uint8_t* block = w.block;
    snprintf(w.path, sizeof(w.path), "/%s", fw_name);
    if (f_open(&file, w.path, FA_READ) != FR_OK) {
        *error_out = fwupdate::BAD_BLOCK;
        return FWU_BAD_FILE;
    }
    fw_checker.begin((uint32_t) f_size(&file));
    fwupdate::Error e = fwupdate::OK;
    for (FSIZE_t at = 0; at + 512 <= f_size(&file) && e == fwupdate::OK; at += 512) {
        UINT got = 0;
        if (f_read(&file, block, 512, &got) != FR_OK || got != 512) {
            e = fwupdate::BAD_BLOCK;
        } else {
            e = fw_checker.feed(block);
        }
    }
    f_close(&file);
    if (e == fwupdate::OK) {
        e = fw_checker.finish();
    }
    if (e != fwupdate::OK) {
        *error_out = e;
        return FWU_BAD_FILE;
    }
    fw_found = true;
    return FWU_OK;
}

static void fw_put(uint8_t* screen, int row, int col, const char* text, uint8_t attr) {
    help_put_text(screen, row, col, text, (int) strlen(text), attr);
}

// The text from column 1, broken at spaces so no line is wider than FW_WIDTH letters (the screen
// has 32 columns); a word longer than that is cut. Returns the row after the last line.
static const int FW_WIDTH = 30;
static int fw_put_wrapped(uint8_t* screen, int row, const char* text, uint8_t attr) {
    size_t len = strlen(text);
    size_t at = 0;
    while (at < len && row < 23) {
        while (at < len && text[at] == ' ') {
            at++;
        }
        size_t end = at + FW_WIDTH;
        if (end >= len) {
            end = len;
        }
        else {
            size_t cut = end;
            while (cut > at && text[cut] != ' ') {
                cut--;
            }
            if (cut > at) {
                end = cut;
            }
        }
        size_t n = end - at;
        while (n > 0 && text[at + n - 1] == ' ') {
            n--;
        }
        help_put_text(screen, row, 1, text + at, (int) n, attr);
        row++;
        at = end;
    }
    return row;
}

// The screen for each outcome of the check
static void fw_draw(uint8_t* screen, uint8_t status, fwupdate::Error error) {
    memset(screen, 0, 6144);
    memset(screen + 6144, 0x07, 768);
    memset(screen + 6144, gameinfo::ATTR_BANNER, 32);
    fw_put(screen, 0, 1, "Update PicoMenu", gameinfo::ATTR_BANNER);
    const char* keys = "X-Back";
    if (status == FWU_OK) {
        char line[40];
        char shown[FW_WIDTH + 1];
        *shorten_name(fw_name, shown, FW_WIDTH) = '\0';
        fw_put(screen, 2, 1, "File", gameinfo::ATTR_HEADING);
        fw_put(screen, 3, 1, shown, gameinfo::ATTR_TEXT);
        fw_put(screen, 5, 1, "This PicoMenu", gameinfo::ATTR_HEADING);
        fw_put(screen, 6, 1, FIRMWARE_VERSION, gameinfo::ATTR_TEXT);
        fw_put(screen, 8, 1, "In the file", gameinfo::ATTR_HEADING);
        snprintf(line, sizeof(line), "%s (%u KB)", fw_checker.version(), (unsigned) ((fw_checker.size() + 1023) / 1024));
        fw_put_wrapped(screen, 9, line, gameinfo::ATTR_TEXT);
        int row = fw_put_wrapped(screen, 12, "Do not switch off while it runs. When it has finished, switch the Spectrum off and on again.", gameinfo::ATTR_TEXT);
        fw_put_wrapped(screen, row + 1, "If it fails, hold BOOTSEL while plugging in USB and copy the .uf2 to RPI-RP2.", gameinfo::ATTR_DIM);
        keys = "Y-Update  N-Cancel";
    } else {
        const char* a = "";
        switch (status) {
            case FWU_NO_SD: a = "No SD card (or the PC has it)."; break;
            case FWU_NO_FILE: a = "No .uf2 file in the root of the SD card."; break;
            case FWU_SEVERAL: a = "More than one .uf2 file in the root. Leave just one."; break;
            case FWU_BAD_FILE: a = "Cannot use the file:"; break;
            case FWU_NO_ROOM: a = "Not enough room on the internal flash. Erase some files there."; break;
            case FWU_STAGE_FAILED: a = "Could not copy the firmware to the internal flash."; break;
            default: a = "Nothing to update."; break;
        }
        int row = fw_put_wrapped(screen, 3, a, gameinfo::ATTR_TEXT);
        if (status == FWU_BAD_FILE) {
            row = fw_put_wrapped(screen, row, fw_error_text(error), gameinfo::ATTR_TEXT);
        }
        if (status == FWU_BAD_FILE || status == FWU_SEVERAL) {
            char shown[FW_WIDTH + 1];
            *shorten_name(fw_name, shown, FW_WIDTH) = '\0';
            fw_put(screen, row + 1, 1, shown, gameinfo::ATTR_DIM);
            row += 2;
        }
        if (status != FWU_NO_SD && status != FWU_NO_FILE) {
            fw_put(screen, row + 1, 1, "Nothing was changed.", gameinfo::ATTR_TEXT);
        }
    }
    fw_put(screen, 23, 1, keys, gameinfo::ATTR_KEYS);
}

// Staging is done a few KB at a time so the Spectrum can draw a progress bar between the
// steps. FWU_MORE: call again. byte 3 of the mailbox is how many of the bar's cells are done

static uint8_t fw_stage_fail() {
    FwScratch& w = fw_scratch();
    if (fw_stage_open) {
        f_close(&w.file);
        if (fw_stage_out >= 0) {
            pico_close(fw_stage_out);
        }
    }
    fw_stage_open = false;
    fw_stage_out = -1;
    fw_stage_pages = 0;
    pico_remove(FW_STAGE_FILE);
    set_last_error("Firmware copy failed", nullptr);
    return FWU_STAGE_FAILED;
}

static uint8_t fw_stage_step(volatile uint8_t* progress) {
    if (!fw_found) {
        return FWU_NOT_READY;
    }
    FwScratch& w = fw_scratch();
    const uint32_t total = fw_checker.size() / fwupdate::PAGE;
    if (!fw_stage_open) {
        // room for the copy, and some for the file system itself
        pico_fsstat_t stat;
        pico_fsstat(&stat);
        uint32_t need = fw_checker.size() / (stat.block_size - 8) + 10;
        if (stat.block_count < stat.blocks_used || stat.block_count - stat.blocks_used < need) {
            return FWU_NO_ROOM;
        }
        snprintf(w.path, sizeof(w.path), "/%s", fw_name);
        if (f_open(&w.file, w.path, FA_READ) != FR_OK) {
            return FWU_STAGE_FAILED;
        }
        fw_stage_out = pico_open(FW_STAGE_FILE, LFS_O_CREAT | LFS_O_WRONLY | LFS_O_TRUNC);
        fw_stage_open = true;
        fw_stage_pages = 0;
        if (fw_stage_out < 0) {
            return fw_stage_fail();
        }
    }
    for (int n = 0; n < 32 && fw_stage_pages < total; n++, fw_stage_pages++) {
        UINT got = 0;
        if (f_read(&w.file, w.block, 512, &got) != FR_OK || got != 512
                || pico_write(fw_stage_out, w.block + 32, fwupdate::PAGE) != (lfs_size_t) fwupdate::PAGE) {
            return fw_stage_fail();
        }
    }
    // cells of the bar, kept below the full bar until the copy is checked
    uint32_t cells = (uint32_t) (((uint64_t) fw_stage_pages * (fwupdate::BAR_CELLS - 1)) / total);
    *progress = (uint8_t) cells;
    if (fw_stage_pages < total) {
        return FWU_MORE;
    }
    f_close(&w.file);
    uint32_t head = 0, size = 0;
    bool ok = pico_fflush(fw_stage_out) == LFS_ERR_OK && pico_ctz_info(fw_stage_out, &head, &size) == LFS_ERR_OK;
    ok = pico_close(fw_stage_out) == LFS_ERR_OK && ok;
    fw_stage_open = false;
    fw_stage_out = -1;
    if (ok) {
        const uint8_t* fs = pico_fs_xip();
        ok = size == fw_checker.size()
            && fwupdate::map_file([fs](uint32_t b, uint32_t o) { return fwupdate::u32(fs + b * 4096 + o); },
                    head, size, 4096, w.map)
            && fwupdate::file_crc(fs, w.map, reinterpret_cast<uint8_t*>(snap_load_buffer) + FWU_SECTOR_AT) == fw_checker.crc();
    }
    if (!ok) {
        return fw_stage_fail();
    }
    *progress = (uint8_t) fwupdate::BAR_CELLS;
    fw_staged = true;
    return FWU_OK;
}

#ifdef PICOFACE_HOST
// On a PC the "flash" is an array the test can look at
uint8_t host_fw_flash[fwupdate::APP_MAX];
int host_fw_applied = 0;
static void host_fw_erase(uint32_t off, uint32_t n) { memset(host_fw_flash + off, 0xFF, n); }
static void host_fw_program(uint32_t off, const uint8_t* d, uint32_t n) { memcpy(host_fw_flash + off, d, n); }
static uint8_t fw_replace_program(volatile uint8_t* mailbox) {
    fwupdate::Flash flash = { host_fw_erase, host_fw_program };
    uint8_t* sector = reinterpret_cast<uint8_t*>(snap_load_buffer) + FWU_SECTOR_AT;
    bool ok = fwupdate::apply(flash, pico_fs_xip(), fw_scratch().map, host_fw_flash, fw_checker.crc(), sector, 3, mailbox + 3);
    host_fw_applied = ok ? 1 : -1;
    return ok ? FWU_OK : FWU_WRITE_FAILED;
}
#else
static void __no_inline_not_in_flash_func(fw_flash_erase)(uint32_t off, uint32_t n) { flash_range_erase(off, n); }
static void __no_inline_not_in_flash_func(fw_flash_program)(uint32_t off, const uint8_t* d, uint32_t n) { flash_range_program(off, d, n); }

// Runs with interrupts off, nothing here may call into the flash. The other core is left running
// (it serves the Spectrum from RAM) so the Spectrum can show the bar: it reads byte 3 of the
// mailbox and waits for byte 0 to stop being 255.
// Returns false if the file was not as checked: nothing has been changed then.
static bool __no_inline_not_in_flash_func(fw_run)(const fwupdate::Map* map, uint32_t crc, uint8_t* sector,
        const uint8_t* fs, void (*to_bootsel)(uint32_t, uint32_t), volatile uint8_t* mailbox) {
    const uint8_t* app = reinterpret_cast<const uint8_t*>(XIP_NOCACHE_NOALLOC_BASE);
    fwupdate::Flash flash;
    flash.erase = fw_flash_erase;
    flash.program = fw_flash_program;
    // still the file that was checked? If not nothing has been touched yet
    if (fwupdate::file_crc(fs, *map, sector) != crc) {
        return false;
    }
    bool ok = fwupdate::apply(flash, fs, *map, app, crc, sector, 3, mailbox + 3);
    mailbox[3] = (uint8_t) fwupdate::BAR_CELLS;
    // let the Spectrum draw the last screen before the Pico goes
    mailbox[0] = ok ? FWU_OK : FWU_WRITE_FAILED;
    uint32_t t0 = timer_hw->timerawl;
    while ((uint32_t) (timer_hw->timerawl - t0) < 3000000u) {}
    if (!ok) {
        to_bootsel(0, 0);       // the program is damaged: the USB drive RPI-RP2 for a new copy
        for (;;) {}
    }
    // restart the whole chip (what watchdog_reboot does, without calling into the flash)
    watchdog_hw->scratch[4] = 0;
    psm_hw->wdsel = PSM_WDSEL_BITS & ~(PSM_WDSEL_XOSC_BITS | PSM_WDSEL_ROSC_BITS);
    hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);
    for (;;) {}
    return true;
}

static uint8_t fw_replace_program(volatile uint8_t* mailbox) {
    void (*to_bootsel)(uint32_t, uint32_t) = reinterpret_cast<void (*)(uint32_t, uint32_t)>(rom_func_lookup(ROM_FUNC_RESET_USB_BOOT));
    uint32_t ints = save_and_disable_interrupts();
    bool ran = fw_run(&fw_scratch().map, fw_checker.crc(), reinterpret_cast<uint8_t*>(snap_load_buffer) + FWU_SECTOR_AT,
            pico_fs_xip(), to_bootsel, mailbox);
    restore_interrupts(ints);
    // only if the file was not as checked: nothing was changed
    return ran ? FWU_OK : FWU_STAGE_FAILED;
}
#endif

// BYTE param1 = FWU_CHECK, FWU_STAGE (repeat while FWU_MORE) or FWU_APPLY. Byte 3 of the mailbox
// is the bar (0 to FW bar cells). The screen is built at INFO_SCREEN_OFFSET
static void nmi_action_update() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer) + INFO_SCREEN_OFFSET;
    const uint8_t op = nmi_rom_data[2];
    fwupdate::Error error = fwupdate::OK;
    uint8_t status;
    if (op == FWU_CHECK) {
        status = fw_check_file(&error);
        fw_draw(screen, status, error);
        preview_screen = screen;
    } else if (op == FWU_STAGE) {
        status = fw_stage_step(nmi_rom_data + 3);
        if (status != FWU_OK && status != FWU_MORE) {
            fw_draw(screen, status, error);
            preview_screen = screen;
        }
    } else {
        status = FWU_NOT_READY;
        if (fw_staged) {
            LOG("Replacing the firmware\n");
            nmi_rom_data[3] = 0;
            status = fw_replace_program(nmi_rom_data);
            if (status != FWU_OK && status != FWU_WRITE_FAILED) {
                fw_draw(screen, status, error);
                preview_screen = screen;
            }
        }
    }
    nmi_rom_data[0] = status;
}

void processNmiRequest() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t action = nmi_rom_data[1];
    LOG("Action to process: %d\n", action);
    if (action == ACTION_BEGIN_SNAP_READ) {
        nmi_action_begin_snap_read();
    }
    else if (action == ACTION_SNAP_READ_NEXT) {
        uint16_t offset =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
        uint16_t count =  (nmi_rom_data[5] << 8) |  nmi_rom_data[4];
        uint32_t tocopy = MIN(count, current_snap_size - current_snap_offset);
        LOG("Continue SNA, destination: %X, count: %X\n", offset, tocopy);
        memcpy(nmi_rom_data + offset, current_snap_data + current_snap_offset, tocopy);
        current_snap_offset += tocopy;
        nmi_rom_data[4] = tocopy & 0xFF;
        nmi_rom_data[5] = (tocopy >> 8) & 0xFF;
    }  else if (action == ACTION_SNA_BEGIN_WRITE) {
        uint16_t headeroffset =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
        LOG("Start SNA save, head destination: %X\n", headeroffset);
        snap_is_paged = false;
        save_is_128k = false;
        memcpy(snap_load_buffer, nmi_rom_data + headeroffset, SNA_HEADER_SIZE);
        current_sna_write_offset = SNA_HEADER_SIZE;
    }  else if (action == ACTION_SNA_BEGIN_WRITE128) {
        uint16_t headeroffset =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
        LOG("Start 128K save, head destination: %X\n", headeroffset);
        snap_is_paged = false;
        save_is_128k = true;
        memcpy(save128_info, nmi_rom_data + headeroffset, sizeof(save128_info));
        current_sna_write_offset = snapload::HEADER_AREA;
    }  else if (action == ACTION_SNA_END_WRITE128) {
        uint8_t port = snapload::finish_save128(reinterpret_cast<uint8_t*>(snap_load_buffer), save128_info, &loaded_paging);
        LOG("End 128K save, paged bank at 0xC000 was %d, port 7FFD = %02X\n", port & 7, port);
        nmi_rom_data[2] = port;
    }  else if (action == ACTION_SNA_WRITE_SCREEN) {
        uint32_t limit = save_is_128k ? snapload::BUFFER128_SIZE : SNA_FILE_SIZE;
        uint32_t tocopy = MIN(sizeof(screen_stash), limit - current_sna_write_offset);
        LOG("Continue SNA save with the screen, tocopy = %X\n", tocopy);
        memcpy(snap_load_buffer + current_sna_write_offset, screen_stash, tocopy);
        current_sna_write_offset += tocopy;
    }  else if (action == ACTION_SCREEN_STASH) {
        nmi_action_screen(SCREEN_STASH);
    }  else if (action == ACTION_SCREEN_RESTORE) {
        nmi_action_screen(SCREEN_RESTORE);
    }  else if (action == ACTION_PREVIEW_READ) {
        nmi_action_screen(SCREEN_PREVIEW);
    }  else if (action == ACTION_SNAP_PREVIEW) {
        nmi_action_snap_preview();
        return;
    }  else if (action == ACTION_RECENT_LIST) {
        nmi_action_recent_list();
        return;
    }  else if (action == ACTION_SCREEN_GRAB) {
        nmi_action_screen_grab();
        return;
    }  else if (action == ACTION_UPDATE) {
        nmi_action_update();
        return;
    }  else if (action == ACTION_GAME_INFO) {
        nmi_action_game_info();
        return;
    }  else if (action == ACTION_HELP_PAGE) {
        nmi_action_help_page();
        return;
    }  else if (action == ACTION_PREPARE_LOAD) {
        nmi_action_prepare_load();
        return;
    }  else if (action == ACTION_POKE_LIST) {
        nmi_action_poke_list();
        return;
    }  else if (action == ACTION_POKE_GET) {
        nmi_action_poke_get();
        return;
    }  else if (action == ACTION_SNA_NEXT_WRITE) {
        uint16_t offset =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
        uint16_t count =  (nmi_rom_data[5] << 8) |  nmi_rom_data[4];
        uint32_t limit = save_is_128k ? snapload::BUFFER128_SIZE : SNA_FILE_SIZE;
        uint32_t tocopy = MIN(count, limit - current_sna_write_offset);
        LOG("Continue SNA save, offset: %X, length = %X; tocopy = %X\n", offset, count, tocopy);
        memcpy(snap_load_buffer + current_sna_write_offset, nmi_rom_data + offset, tocopy);
       current_sna_write_offset += tocopy;
    } else if (action == ACTION_SNAP_LIST) {
        nmi_action_list_sna();
    } else if (action == ACTION_SNAP_CHDIR) {
        nmi_action_snap_chdir();
    } else if (action == ACTION_SNAP_FIND) {
        nmi_action_snap_find();
    } else if (action == ACTION_SNA_SAVE) {
        nmi_action_sna_save();
        return;
    } else if (action == ACTION_SNA_SAVE_DIR) {
        nmi_action_sna_save_dir();
        return;
    } else if (action == ACTION_SNAP_MOVE) {
        nmi_action_snap_move();
        return;
    } else if (action == ACTION_SNAP_MKDIR) {
        nmi_action_snap_mkdir();
        return;
    } else if (action == ACTION_SNAP_RENAME) {
        nmi_action_snap_rename();
        return;
    } else if (action == ACTION_SNAP_ERASE) {
        nmi_action_snap_erase();
        return;
    } else if (action == ACTION_ROM_LIST) {
        nmi_action_list_rom();
    } else if (action == ACTION_ROM_CHANGE) {
        nmi_action_change_rom();
        return;
    
    } else if (action == ACTION_READ_DATA_FROM_PICO) {
        nmi_action_read_data_from_pico();
    } else if (action == ACTION_WRITE_DATA_TO_PICO) {
        nmi_action_write_data_to_pico();
    }


    nmi_rom_data[0] = 0;
}


//---------------------------------------------
// CLI commands

inline bool sendNmiAndWaitForCompletion(uint8_t command, uint16_t param1, uint16_t param2) {
    // Without the volatile then the nmi_rom_data[0] is only
    // accessed once and we fail to process requests from the nmi
    // rom and the spectrum hangs.
    volatile uint8_t* nmi_rom_data = rom_data + 16384;
    if (sendNmiRequest(command, param1, param2)) {
        while (((rom_state.flags & 0x2) != 0) || rom_state.nmi_active) {
            if (nmi_rom_data[0] == 255) {
                processNmiRequest();
            }            
        }
        return true;
    }
    return false;
}

// Attempt to obtain snapshot from the running spectrum
const uint8_t* getSnapshotData(size_t& snapshotLength) {
    // Without the volatile then the nmi_rom_data[0] is only
    // accessed once and we fail to process requests from the nmi
    // rom and the spectrum hangs.
    volatile uint8_t* nmi_rom_data = rom_data + 16384;

    bool loggingWasEnabled = enable_logging;
    enable_logging = false;
    const uint8_t* data = nullptr;

    if (sendNmiRequest(STARTUP_ACTION_TRANSFER_SNAP, 0, 0)) {
        while (((rom_state.flags & 0x2) != 0) || rom_state.nmi_active) {
            if (nmi_rom_data[0] == 255) {
                processNmiRequest();
            }            
        }
        snapshotLength = SNA_FILE_SIZE;
        data = reinterpret_cast<const uint8_t*>(snap_load_buffer);
    }

    enable_logging = loggingWasEnabled;
    return data;
}



//---------------------------------------------
// ROM-trap tape loading
//
// For tapes that cannot be turned into a snapshot (custom loaders that load
// headerless blocks etc.) we serve a copy of the 48K ROM from the Pico in
// which the tape loading routine LD-BYTES (0x0556) has been replaced by a
// small hook.  The hook posts a request (block flag, destination address,
// length) in a mailbox at the end of the ROM area and waits.  The main loop
// below finds the next block in the .tap file, checks the flag, and sends
// the data into Spectrum RAM with an NMI "load memory" request, exactly like
// a real tape load but without the tape.  Anything that loads through the
// ROM routine works, whatever the block layout.  Loaders that read the EAR
// port themselves (turbo loaders) still cannot work.
//
// Needs an unmodified 48K ROM image called TAPE_ROM_NAME in the Pico's flash
// (or the root of the SD card).  The ROM is patched in memory only.

static const char* const TAPE_ROM_NAME = "48.rom";
static const uint32_t TAPE_MBOX = 0x3FF8;        // mailbox, replaces the (c) glyph
static const uint32_t TAPE_HOOK_ADDR = 0x0556;   // LD-BYTES

// Mailbox layout: +0 state (1 = request posted, 2 = ok, 3 = failed),
// +1 flag, +2/+3 destination, +4/+5 length, +6 mode (1 load, 0 verify)

// Z80 hook (assembled and tested in a simulator).  Ends with the 8 byte
// glyph that the hook writes back over the mailbox when it is finished.
static const uint8_t TAPE_HOOK[] = {
    0xF3, 0x21, 0x3F, 0x05, 0xE5, 0x32, 0xF9, 0x3F, 0xDD, 0x22, 0xFA, 0x3F,
    0xED, 0x53, 0xFC, 0x3F, 0x9F, 0xE6, 0x01, 0x32, 0xFE, 0x3F, 0xDD, 0x19,
    0x11, 0x00, 0x00, 0x3E, 0x01, 0x32, 0xF8, 0x3F, 0x3A, 0xF8, 0x3F, 0xFE,
    0x02, 0x38, 0xF9, 0xFE, 0x03, 0xF5, 0x21, 0x90, 0x05, 0x11, 0xF8, 0x3F,
    0x01, 0x08, 0x00, 0xED, 0xB0, 0x11, 0x00, 0x00, 0xF1, 0xC9,
    0x3C, 0x42, 0x99, 0xA1, 0xA1, 0x99, 0x42, 0x3C
};

// What an unmodified 48K ROM has where we patch it
static const uint8_t TAPE_ROM_LDBYTES_SIG[] = {
    0x14, 0x08, 0x15, 0xF3, 0x3E, 0x0F, 0xD3, 0xFE, 0x21, 0x3F, 0x05, 0xE5
};

// Types LOAD "" itself, so the tape starts loading as soon as the Spectrum has reset.
// Three changes to the ROM, all in the 48K ROM's start up, which only runs after a
// reset:
//  * the last step of the start up (SET 5,(IY+2) at 0x129C, then the editor) is
//    replaced by a jump to TAPE_AUTO_CODE, which carries on as before and also
//    sets the input routine of the keyboard channel to the one below.  It has to
//    be done there, since CLS has just set the channel up and would undo a change
//    to the ROM's copy of it.
//  * that routine returns the keys in TAPE_AUTO_KEYS (LOAD, ", ", ENTER, which
//    the editor takes as if they were typed) and then works as KEY-INPUT does.
//  * the count of keys typed is a byte in the (unused) printer buffer.
// 0x386E to 0x3CFF is empty in the ROM.  Assembled and tested with the real ROM in a
// Z80 simulator: the keys are typed, and LOAD "" gets as far as LD-BYTES.
static const bool TAPE_AUTOTYPE = true;
static const uint32_t TAPE_AUTO_CODE = 0x3870;
static const uint32_t TAPE_AUTO_KEYS = 0x38A0;
static const uint32_t TAPE_AUTO_HOOK_ADDR = 0x129C;
static const uint8_t TAPE_AUTO_ROM[] = {
    0xFD, 0xCB, 0x02, 0xEE, 0xAF, 0x32, 0x00, 0x5B, 0x2A, 0x4F, 0x5C, 0x23,
    0x23, 0x36, 0x85, 0x23, 0x36, 0x38, 0xC3, 0xA9, 0x12, 0xE5, 0x21, 0x00,
    0x5B, 0x7E, 0xC6, 0xA0, 0xE5, 0x6F, 0x26, 0x38, 0x7E, 0xE1, 0xB7, 0x28,
    0x04, 0x34, 0xE1, 0x37, 0xC9, 0xE1, 0xC3, 0xA8, 0x10, 0x00, 0x00, 0x00,
};
static_assert(sizeof(TAPE_AUTO_ROM) == TAPE_AUTO_KEYS - TAPE_AUTO_CODE, "the keys come after the code");
// the autokey routine must find its keys without the low byte of the address overflowing
static_assert((TAPE_AUTO_KEYS & 0xFF) + 7 < 256 && (TAPE_AUTO_KEYS & 0xFF) == 0xA0, "keys address is in the code");
static const uint8_t TAPE_AUTO_LOAD[] = { 0xEF, '"', '"', 0x0D, 0 };            // LOAD ""
static const uint8_t TAPE_AUTO_LOAD_CODE[] = { 0xEF, '"', '"', 0xAF, 0x0D, 0 }; // LOAD "" CODE
// What an unmodified 48K ROM has where the start up is changed
static const uint8_t TAPE_AUTO_HOOK_SIG[] = { 0xFD, 0xCB, 0x02, 0xEE, 0x18, 0x07 };

static bool tape_service_active = false;
static bool tape_trap_running() { return tape_service_active; }
static FileHandle tape_file;
static bool tape_file_open = false;
static uint8_t tape_drive = 0;
static char tape_name[BROWSE_PATH_MAX];
static TzxState tape_tz;      // where the tape being served is (a preview of another .tzx has its own)

static bool tape_open_file() {
    if (tape_file_open) {
        tape_file.close();
        tape_file_open = false;
    }
    tape_file_open = tape_file.open(tape_drive == 1 ? Drive::EXTERNAL_SD_CARD : Drive::INTERNAL_FLASH,
        tape_name, true);
    if (tape_file_open && tape_is_tzx(tape_name) && !tzx_begin(tape_file)) {
        tape_file.close();
        tape_file_open = false;
    }
    tape_tz = tz;
    return tape_file_open;
}

// Reads and patches the 48K ROM into rom_data[0..16383]
static bool tape_load_patched_rom(bool first_block_is_code) {
    // The ROM is read into the part of the snapshot buffer that the 48K
    // memory image does not use, instead of a buffer of its own.
    static const uint32_t ROM_SIZE = 16384;
    static const uint32_t ROM_SCRATCH = 65536;
    static_assert(ROM_SCRATCH + ROM_SIZE <= SNAP_LOAD_BUFFER_SIZE, "no room for the ROM");
    static_assert(ROM_SCRATCH >= SNA_FILE_SIZE, "the ROM would overlap the memory image");
    uint8_t* rom = reinterpret_cast<uint8_t*>(snap_load_buffer) + ROM_SCRATCH;
    FileHandle f;
    bool opened = f.open(Drive::INTERNAL_FLASH, TAPE_ROM_NAME, true);
    if (!opened) {
        opened = f.open(Drive::EXTERNAL_SD_CARD, TAPE_ROM_NAME, true);
    }
    if (!opened) {
        LOG("Tape ROM: %s not found\n", TAPE_ROM_NAME);
        return false;
    }
    int got = f.read(rom, ROM_SIZE);
    f.close();
    if (got != (int)ROM_SIZE) {
        LOG("Tape ROM: %s must be a 16384 byte 48K ROM image\n", TAPE_ROM_NAME);
        return false;
    }
    if (memcmp(rom + TAPE_HOOK_ADDR, TAPE_ROM_LDBYTES_SIG, sizeof(TAPE_ROM_LDBYTES_SIG)) != 0 ||
        rom[0x053F] != 0xF5 || rom[0] != 0xF3) {
        LOG("Tape ROM: %s does not look like a standard 48K ROM\n", TAPE_ROM_NAME);
        return false;
    }
    memcpy(rom + TAPE_HOOK_ADDR, TAPE_HOOK, sizeof(TAPE_HOOK));      // hook and its copy of the (c) glyph
    memcpy(rom + TAPE_MBOX, TAPE_HOOK + sizeof(TAPE_HOOK) - 8, 8);   // mailbox starts out as the (c) glyph
    if (TAPE_AUTOTYPE) {
        bool room = true;
        for (uint32_t i = TAPE_AUTO_CODE; i < TAPE_AUTO_KEYS + 8; i++) {
            room = room && rom[i] == 0xFF;
        }
        if (room && memcmp(rom + TAPE_AUTO_HOOK_ADDR, TAPE_AUTO_HOOK_SIG, sizeof(TAPE_AUTO_HOOK_SIG)) == 0) {
            memcpy(rom + TAPE_AUTO_CODE, TAPE_AUTO_ROM, sizeof(TAPE_AUTO_ROM));
            if (first_block_is_code) {
                memcpy(rom + TAPE_AUTO_KEYS, TAPE_AUTO_LOAD_CODE, sizeof(TAPE_AUTO_LOAD_CODE));
            } else {
                memcpy(rom + TAPE_AUTO_KEYS, TAPE_AUTO_LOAD, sizeof(TAPE_AUTO_LOAD));
            }
            rom[TAPE_AUTO_HOOK_ADDR] = 0xC3;                        // JP TAPE_AUTO_CODE
            rom[TAPE_AUTO_HOOK_ADDR + 1] = TAPE_AUTO_CODE & 0xFF;
            rom[TAPE_AUTO_HOOK_ADDR + 2] = TAPE_AUTO_CODE >> 8;
            rom[TAPE_AUTO_HOOK_ADDR + 3] = 0;
        } else {
            LOG("Tape ROM: cannot type LOAD \"\" for you with this ROM\n");
        }
    }
    memcpy(rom_data, rom, ROM_SIZE);
    return true;
}

// Serves one block of the tape.  Returns true if the request was satisfied
// (the way a successful LD-BYTES would).
static bool tape_serve_block_at(uint8_t flag, uint16_t dest, uint16_t length, bool load);
static bool tape_serve_block(uint8_t flag, uint16_t dest, uint16_t length, bool load) {
    tz = tape_tz;
    bool ok = tape_serve_block_at(flag, dest, length, load);
    tape_tz = tz;
    return ok;
}

static bool tape_serve_block_at(uint8_t flag, uint16_t dest, uint16_t length, bool load) {
    const bool tzx = tape_is_tzx(tape_name);
    uint32_t blen;
    if (!tape_next_block(tape_file, tzx, blen)) {
        // The end of the tape: the load fails (as it would wait for ever on a real tape) and
        // the tape is rewound, so the next load starts from the first block again.
        LOG("Tape: end of the tape\n");
        if (tape_open_file()) tz = tape_tz;
        return false;
    }
    uint8_t bflag;
    if (blen < 2 || tape_file.read(&bflag, 1) != 1) {
        return false;
    }
    uint32_t payload = blen - 2;

    if (bflag != flag) {
        LOG("Tape: skipped block with flag %02X (wanted %02X)\n", bflag, flag);
        tap_skip(tape_file, payload + 1);
        return false;
    }

    uint32_t n = payload < length ? payload : length;
    bool ok = payload >= length;      // a shorter block would end in a checksum error
    uint32_t skipfront = dest < 16384 ? 16384 - dest : 0;   // part that would land in ROM
    if (skipfront > n) skipfront = n;
    uint32_t d = dest + skipfront;
    uint32_t keep = n - skipfront;
    if (d + keep > 65536) keep = 65536 - d;

    if (!tap_skip(tape_file, skipfront)) return false;
    if (keep > 0) {
        uint8_t* target = reinterpret_cast<uint8_t*>(snap_load_buffer) + SNA_HEADER_SIZE + (d - 16384);
        if (tape_file.read(target, keep) != (int)keep) return false;
    }
    uint32_t used = skipfront + keep;
    if (!tap_skip(tape_file, payload - used + 1)) return false;   // rest of block and checksum

    LOG("Tape: block flag %02X, %u of %u bytes to %04X%s\n", bflag, (unsigned)keep, (unsigned)length,
        (unsigned)d, ok ? "" : " (short)");

    if (load && keep > 0) {
        bool loggingWasEnabled = enable_logging;
        enable_logging = false;
        bool sent = sendNmiAndWaitForCompletion(STARTUP_ACTION_LOAD_MEMORY, d, keep);
        enable_logging = loggingWasEnabled;
        if (!sent) return false;
    }
    return ok;
}

// Called from the main loop
static void tape_service_poll() {
    volatile uint8_t* mbox = rom_data + TAPE_MBOX;
    if (!tape_service_active || mbox[0] != 1) {
        return;
    }
    uint8_t flag = mbox[1];
    uint16_t dest = mbox[2] | (mbox[3] << 8);
    uint16_t length = mbox[4] | (mbox[5] << 8);
    bool load = mbox[6] != 0;
    bool ok = tape_serve_block(flag, dest, length, load);
    mbox[0] = ok ? 2 : 3;
}

static void tape_service_stop() {
    if (tape_service_active) {
        tape_service_active = false;
        if (tape_file_open) {
            tape_file.close();
            tape_file_open = false;
        }
        rom_state.flags_on_nmi_exit = 0;       // back to the internal ROM
        rom_state.writableStartAddress = 32768;
    }
}

// Starts the ROM trap for a tape.  When the NMI menu exits the Spectrum resets
// into the patched ROM, which types LOAD "" (or LOAD "" CODE) by itself.
static bool tape_service_start(uint8_t drive, const char* filename) {
    strncpy(tape_name, filename, sizeof(tape_name) - 1);
    tape_name[sizeof(tape_name) - 1] = '\0';
    tape_drive = drive;
    if (!tape_open_file()) {
        LOG("Tape: could not open %s\n", filename);
        return false;
    }
    // A tape that starts with a code block is loaded with LOAD "" CODE
    uint8_t first[2];       // flag, header type
    uint32_t first_len;
    bool code_first = tape_next_block(tape_file, tape_is_tzx(tape_name), first_len) && first_len == 19 &&
        tape_file.read(first, sizeof(first)) == (int)sizeof(first) && first[0] == 0 && first[1] == 3;
    if (!tape_open_file() || !tape_load_patched_rom(code_first)) {
        if (tape_file_open) {
            tape_file.close();
            tape_file_open = false;
        }
        return false;
    }
    tape_service_active = true;
    rom_state.flags_on_nmi_exit = 1;                    // serve our ROM when the NMI exits
    rom_state.writableStartAddress = TAPE_MBOX;         // only the mailbox is writable
    return true;
}


uint8_t* beginSendSnapDataToMachine() {
    return reinterpret_cast<uint8_t*>(snap_load_buffer);
}

uint8_t* getRamLoadBuffer() {
    return reinterpret_cast<uint8_t*>(snap_load_buffer + SNA_HEADER_SIZE);
}


bool endSendSnapDataToMachine(uint32_t snapshotSize) {
    bool loggingWasEnabled = enable_logging;
    enable_logging = false;
    current_snap_size = snapshotSize;
    snap_is_paged = false;
    bool ok = sendNmiAndWaitForCompletion(STARTUP_ACTION_LOAD_SNAP, 0, 0);
    enable_logging = loggingWasEnabled;
    return ok;
}


// Long press of the button: stop supplying the tape ROM (or any other ROM) and
// restart the Spectrum from address 0 with ROM 0 and bank 0 paged in, which on a
// 128K machine is the start up menu and on a 48K machine is the start up screen.
// It is done with a snapshot that jumps to 0, the same as the tape loader's reset.
static void reset_spectrum_to_internal_rom() {
    LOG("Long press: resetting into the internal ROM\n");
    tape_service_stop();
    tap_make_reset_snapshot();
    game_running = false;
    reset_to_128k_pending = true;
    endSendSnapDataToMachine(SNA_FILE_SIZE);
    reset_to_128k_pending = false;
}

bool sendLoadBufferToMachine(uint32_t startAddress, uint32_t length) {
    if (startAddress >= 16384 && startAddress + length < 65536) {
        // PARAM1 is destination location
        // PARAM2 is total length
        bool loggingWasEnabled = enable_logging;
        enable_logging = false;
        bool ok = sendNmiAndWaitForCompletion(STARTUP_ACTION_LOAD_MEMORY, startAddress, length);
        enable_logging = loggingWasEnabled;
        return ok;
    }
    return false;
}

bool readMachineMemToLoadBuffer(uint32_t startAddress, uint32_t length) {
    if (startAddress >= 16384 && startAddress + length < 65536) {
        // PARAM1 is source location
        // PARAM2 is total length
        bool loggingWasEnabled = enable_logging;
        enable_logging = false;
        bool ok = sendNmiAndWaitForCompletion(STARTUP_ACTION_SAVE_MEMORY, startAddress, length);
        enable_logging = loggingWasEnabled;
        return ok;
    }
    return false;
}

void mountSdCard() {
    sd_card_t *pSD = sd_get_by_num(0);
    FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
    if (fr == FR_OK) {
        sd_card_mounted = true;
        LOG("Mounted SD CARD\n");
    }
    else {
        sd_card_mounted = false;
        set_last_error("SD card not mounted", nullptr);
        LOG("Failed to mount SD CARD\n");
    }
}


// ---- The SD card as a USB drive (usb_msc.cxx decides when; these are what it asks of us)

// True while the Pico is using the card: the NMI menu is open or about to open, or a tape
// is being played from it. The PC is not given the card then.
bool pico_sd_in_use() {
    return ((rom_state.flags & 0x2) != 0) || rom_state.nmi_active || tape_service_active || tape_file_open;
}

// The card goes to the PC: nothing of ours may keep a view of it. FatFs is unmounted, so
// nothing of ours can open a file on it, and the NMI menu is told there is no SD card (in
// use by the PC). No messages are logged: this runs inside the USB code.
void pico_sd_give_to_pc() {
    bool loggingWasEnabled = enable_logging;
    enable_logging = false;
    walk_stop();
    sd_card_t* pSD = sd_get_by_num(0);
    f_unmount(pSD->pcName);
    sd_card_mounted = false;
    enable_logging = loggingWasEnabled;
}

// The card comes back: the PC may have changed anything on it, so FatFs mounts it afresh
// and the browse position goes back to the top.
void pico_sd_take_back_from_pc() {
    bool loggingWasEnabled = enable_logging;
    enable_logging = false;
    walk_stop();
    sd_cwd[0] = '\0';
    mountSdCard();
    enable_logging = loggingWasEnabled;
}

void initFileSystem() {
    if (pico_mount(false) != LFS_ERR_OK) {
        LOG("Mount failed, try formatting\n");
        if (pico_mount(true) != LFS_ERR_OK) {
            LOG("Mount failed, after formatting\n");
        }
    }
    pico_remove(FW_STAGE_FILE);         // the copy of a firmware update, done with (or never finished)
    mountSdCard();
    load_save_dir();
    load_last_game();
    check_trace();
}

bool sendNmiRequest(uint8_t command, uint16_t param1, uint16_t param2) {

    if (((rom_state.flags & 0x2) != 0) || rom_state.nmi_active) {
        return false;
    }
    else {

        uint8_t* nmi_rom_data = rom_data + 16384;
        memcpy(nmi_rom_data, NMI_ROM, NMI_ROM_SIZE);
        // the NMI rom starts afresh each time, with no search in force, and
        // the card may have been changed since it was last listed
        snap_filter[0] = '\0';
        walk_stop();
        move_picked = false;
        screen_stashed = false;
        preview_screen = nullptr;
        nmi_rom_data[0] = 0;
        nmi_rom_data[STARTUP_COMMAND_OFFSET] = command;
        nmi_rom_data[STARTUP_PARAM1_OFFSET] = param1 & 0xFF;
        nmi_rom_data[STARTUP_PARAM1_OFFSET + 1] = (param1 >> 8) & 0xFF;
        nmi_rom_data[STARTUP_PARAM2_OFFSET] = param2 & 0xFF;
        nmi_rom_data[STARTUP_PARAM2_OFFSET + 1] = (param2 >> 8) & 0xFF;
        // 1 there is a card, 2 it is in use by the PC (USB drive), 0 no card
        nmi_rom_data[STARTUP_SDCARD_PRESENT] = sd_card_mounted ? 1 : (usb_msc_pc_holds_card() ? 2 : 0);
        nmi_rom_data[STARTUP_SDCARD_PRESENT + 1] = running_48k ? 1 : 0;     // start_up_machine48
        // what the L item on the menu says: the last game, or that there is none to reload
        extras::reload_label(reload_known, reload_drive == 0 || sd_card_mounted, reload_name,
            reinterpret_cast<char*>(nmi_rom_data + STARTUP_RELOAD_LABEL));
        rom_state.flags |= 2;
        gpio_put(PIN_NMI, false);
        sleep_ms(3);
        gpio_put(PIN_NMI, true);
        return true;
    }
}

//#define PIO_DEBUGGING


// ---- C on a tape's information: convert the tape to a .z80 snapshot ----
// The tape is loaded on an emulated Spectrum (tzxconv.h, through tzxconv_pico.h), so its own
// loader runs, turbo and protected ones too, and the memory and registers are saved beside
// the tape as <name>.z80 (or "<name> 2.z80" ... if that is taken). The ROM asks for it with
// op 6 of ACTION_GAME_INFO and goes on asking (op 6, key 0) while byte 1 says 6; each time a
// slice of about 120 ms is run and the emulated screen is sent with a title row and a status
// row over it (a live view of the loading). Modes: A Auto (a 128K first if the machine can
// page memory, then a 48K if that fails), 4 a 48K, 1 a 128K.
//
// Memory: the 48K or the eight 128K banks are in snap_load_buffer (as a snapshot would be);
// the 48K ROM at snap_load_buffer + 65536 (as for the tape ROM trap), the two 128K ROMs in
// list_pool (so the Browse list is read again afterwards); the machine, the tape's file and
// the two overlay rows in pok_file (read again when the poke list is next opened).
// The ROMs are 48.rom, and 128.rom (32K: the editor ROM then the 48K BASIC ROM) or
// 128-0.rom and 128-1.rom, in the root of the flash or the SD card.

enum ConvPhase : uint8_t { CONV_IDLE, CONV_CHOOSE, CONV_RUN };
enum ConvMode : uint8_t { CONV_AUTO, CONV_48, CONV_128 };
static uint8_t conv_phase = CONV_IDLE;
static uint8_t conv_mode = CONV_AUTO;
static bool conv_machine128 = false;        // the ROM found the machine can page memory
static bool conv_as128 = false;             // the attempt running is a 128K one
static bool conv_retried = false;           // Auto: the 128K attempt failed, this is the 48K one
static uint32_t conv_size = 0;              // of the tape file
static uint64_t conv_started = 0;           // time_us_64() at the start
static bool conv_file_open = false;         // conv_scratch().file is the tape, open

struct ConvScratch {
    alignas(8) uint8_t machine[pconv::STATE_SIZE];
    FileHandle file;
    uint8_t pix[2][8][32];                  // the overlay rows: 0 (top) and 23 (bottom)
    uint8_t attr[2][32];
};
static_assert(sizeof(ConvScratch) <= sizeof(extras::PokFile), "the converter does not fit in pok_file");
static ConvScratch& conv_scratch() { return *reinterpret_cast<ConvScratch*>(&pok_file); }

static void conv_overlay(uint8_t* dest, uint32_t at, uint32_t length) {
    ConvScratch& c = conv_scratch();
    for (uint32_t i = 0; i < length; i++) {
        uint32_t o = at + i;
        if (o < 6144) {
            uint32_t row = ((o >> 11) << 3) | ((o >> 5) & 7);
            if (row == 0 || row == 23) dest[i] = c.pix[row == 23][(o >> 8) & 7][o & 31];
        } else {
            uint32_t row = (o - 6144) >> 5;
            if (row == 0 || row == 23) dest[i] = c.attr[row == 23][o & 31];
        }
    }
}

// A row of the overlay (0 top, 1 bottom): blank in attr, then the text from col
static void conv_row(int r, uint8_t attr, int col, const char* text) {
    ConvScratch& c = conv_scratch();
    memset(c.pix[r], 0, sizeof(c.pix[r]));
    memset(c.attr[r], attr, sizeof(c.attr[r]));
    for (int i = 0; text[i] != '\0' && col + i < 32; i++) {
        unsigned char ch = (unsigned char) text[i];
        const uint8_t* glyph = help_font[(ch >= 32 && ch < 128 ? ch : '?') - 32];
        for (int line = 0; line < 8; line++) c.pix[r][line][col + i] = glyph[line];
    }
}

// Reads a ROM file of exactly `size` bytes from the root of the flash, else the SD card
static bool conv_read_rom(const char* name, uint8_t* dest, uint32_t size) {
    FileHandle& f = conv_scratch().file;
    for (int d = 0; d < 2; d++) {
        if (d == 1 && !sd_card_mounted) break;
        if (!f.open(d == 0 ? Drive::INTERNAL_FLASH : Drive::EXTERNAL_SD_CARD, name, true)) continue;
        bool ok = f.size() == (int) size && f.read(dest, size) == (int) size;
        f.close();
        if (ok) return true;
    }
    return false;
}

static bool conv_is_tape(const char* path, bool* tzx) {
    size_t n = strlen(path);
    if (n < 4) return false;
    *tzx = strcasecmp(path + n - 4, ".tzx") == 0;
    return *tzx || strcasecmp(path + n - 4, ".tap") == 0;
}

static void conv_titles() {
    char t[48];
    snprintf(t, sizeof(t), "Converting (%s): %s", conv_as128 ? "128K" : conv_retried ? "48K, 2nd go" : "48K", info_name);
    conv_row(0, gameinfo::ATTR_BANNER, 0, t);
}

// ---- screens during a conversion ----
// The tape's loading screen is saved by itself once it has loaded (tzxconv: loading_screen),
// and S saves the screen as it is then. Both go in the scraper's folder, /Scraper/Screen
// Grabs on the SD card, as <name>.scr or the next free <name>1.scr, <name>2.scr ..., so V
// shows them with the ones the scraper downloaded. The top row says what happened for 3 s.
static const uint8_t CONV_ATTR_DONE = 0x60;     // black on bright green
static const uint8_t CONV_ATTR_FAIL = 0x57;     // white on bright red
static void conv_close_tape();
static bool conv_scr_folder = false;            // the folder is there (made at the start)
static bool conv_auto_saved = false;            // the loading screen has been saved
static int conv_auto_number = -1;               // its number (<name><n>.scr), -1 none: BREAK removes it
static bool conv_manual_saved = false;          // S saved a screen in this conversion
static uint64_t conv_note_until = 0;            // the top row shows a note until then

static void conv_note(const char* text, uint8_t attr) {
    conv_row(0, attr, 0, text);
    conv_note_until = time_us_64() + 3000000;
}

// Makes the folder if it isn't there. Done before the ROMs are read, as making a folder
// borrows list_pool, which holds the 128K ROMs during a conversion.
static void conv_scr_prepare() {
    conv_scr_folder = false;
    if (!sd_card_mounted) return;
    if (fsops::kind(Drive::EXTERNAL_SD_CARD, screengrab::SCRAPER_FOLDER) != fsops::KIND_DIR) {
        uint8_t made = make_sd_folder("/Scraper");
        if (made == fsops::OK || made == fsops::NAME_IN_USE) made = make_sd_folder(screengrab::SCRAPER_FOLDER);
        if (made != fsops::OK && made != fsops::NAME_IN_USE) return;
    }
    conv_scr_folder = true;
}

// Saves scr (6912 bytes). The tape's file handle is used, so the tape is closed meanwhile and
// opened again where it was: false if it can't be (the conversion has to stop).
static bool conv_save_screen(const uint8_t* scr, bool automatic) {
    ConvScratch& c = conv_scratch();
    if (!sd_card_mounted || !conv_scr_folder) {
        conv_note(sd_card_mounted ? "Screen not saved (no folder)" : "No SD card: screen not saved", CONV_ATTR_FAIL);
        return true;
    }
    char base[screengrab::NAME_MAX + 1];
    if (screengrab::base_name(info_name, base) == 0) snprintf(base, sizeof(base), "Screen");
    int number = -1;
    for (int i = 0; i < screengrab::MAX_SCAN && number < 0; i++) {
        screengrab::path_in(screengrab::SCRAPER_FOLDER, base, i, ".scr", grab_path, sizeof(grab_path));
        if (fsops::kind(Drive::EXTERNAL_SD_CARD, grab_path) == fsops::KIND_NONE) number = i;
    }
    if (number < 0) {
        conv_note("Screen not saved: too many", CONV_ATTR_FAIL);
        return true;
    }
    uint32_t pos = c.file.tell();
    conv_close_tape();
    bool ok = false;
    if (c.file.open(Drive::EXTERNAL_SD_CARD, grab_path, false)) {
        ok = c.file.write(scr, screengrab::SCR_SIZE) == (int) screengrab::SCR_SIZE;
        ok = c.file.close() && ok;
    }
    if (!ok) fsops::remove_path(Drive::EXTERNAL_SD_CARD, grab_path);
    else if (automatic) conv_auto_number = number;
    else conv_manual_saved = true;
    LOG("Convert: screen %s %s\n", grab_path, ok ? "saved" : "not saved");
    if (!c.file.open(info_drive == 1 ? Drive::EXTERNAL_SD_CARD : Drive::INTERNAL_FLASH, info_path, true)) return false;
    conv_file_open = true;
    if (!c.file.seek(pos)) return false;
    // "Loading screen saved: <name>" or "Screen saved: <name>", the end kept if it is long
    char name[screengrab::NAME_MAX + 8], t[40];
    if (number == 0) snprintf(name, sizeof(name), "%s", base);
    else snprintf(name, sizeof(name), "%s%d", base, number);
    const char* lead = !ok ? "Screen not saved: " : automatic ? "Loading screen saved: " : "Screen saved: ";
    size_t room = 32 - strlen(lead), n = strlen(name);
    if (!ok) snprintf(t, sizeof(t), "Screen not saved (SD card)");
    else if (n > room) snprintf(t, sizeof(t), "%s..%s", lead, name + n - (room - 2));
    else snprintf(t, sizeof(t), "%s%s", lead, name);
    conv_note(t, ok ? CONV_ATTR_DONE : CONV_ATTR_FAIL);
    return true;
}

// Starts an attempt. False, with conv_msg set, if it can't.
static bool conv_start(bool as128) {
    uint8_t* buf = reinterpret_cast<uint8_t*>(snap_load_buffer);
    ConvScratch& c = conv_scratch();
    conv_as128 = as128;
    if (as128) {
        uint8_t* roms = reinterpret_cast<uint8_t*>(list_pool);
        if (!conv_read_rom("128.rom", roms, 32768) &&
            !(conv_read_rom("128-0.rom", roms, 16384) && conv_read_rom("128-1.rom", roms + 16384, 16384))) {
            snprintf(conv_msg, sizeof(conv_msg), "No 128.rom (see Help: Convert)");
            return false;
        }
    } else if (!conv_read_rom(TAPE_ROM_NAME, buf + 65536, 16384)) {
        snprintf(conv_msg, sizeof(conv_msg), "No 48.rom (see Help: Convert)");
        return false;
    }
    bool tzx = false;
    conv_is_tape(info_path, &tzx);
    if (!c.file.open(info_drive == 1 ? Drive::EXTERNAL_SD_CARD : Drive::INTERNAL_FLASH, info_path, true)) {
        snprintf(conv_msg, sizeof(conv_msg), "Cannot open the tape");
        return false;
    }
    conv_file_open = true;
    conv_size = (uint32_t) c.file.size();
    if (as128) {
        uint8_t* banks[8];
        for (uint32_t i = 0; i < 8; i++) banks[i] = snapload::bank_memory(buf, i);
        memset(buf, 0, snapload::HEADER_AREA);
        const uint8_t* roms = reinterpret_cast<const uint8_t*>(list_pool);
        pconv::begin128(c.machine, roms, roms + 16384, banks, &c.file, tzx);
    } else {
        pconv::begin48(c.machine, buf + 65536, buf + SNA_HEADER_SIZE, &c.file, tzx);
    }
    conv_titles();
    LOG("Convert %s as %s\n", info_path, as128 ? "128K" : "48K");
    return true;
}

struct ConvSink {
    FileHandle* f;
    bool write(const uint8_t* p, uint32_t n) { return f->write(p, n) == (int) n; }
};

// Saves the snapshot beside the tape. False if it could not.
static bool conv_save(char* saved, size_t savedsize) {
    uint8_t* buf = reinterpret_cast<uint8_t*>(snap_load_buffer);
    ConvScratch& c = conv_scratch();
    Drive drive = info_drive == 1 ? Drive::EXTERNAL_SD_CARD : Drive::INTERNAL_FLASH;
    static char out[BROWSE_PATH_MAX];
    const char* slash = strrchr(info_path, '/');
    int dirlen = slash != nullptr ? (int) (slash - info_path) : 0;
    bool found = false;
    for (int k = 1; k <= 9 && !found; k++) {
        char suffix[4] = "";
        if (k > 1) snprintf(suffix, sizeof(suffix), " %d", k);
        int n = snprintf(out, sizeof(out), "%.*s/%s%s.z80", dirlen, info_path, info_name, suffix);
        if (n < 0 || n >= (int) sizeof(out)) return false;
        found = fsops::kind(drive, out) == fsops::KIND_NONE;
        if (found) snprintf(saved, savedsize, "%s%s.z80", info_name, suffix);
    }
    if (!found) return false;
    uint8_t h[27];
    uint16_t pc;
    pconv::sna_header(c.machine, h, pc);
    if (!c.file.open(drive, out, false)) return false;
    ConvSink sink{ &c.file };
    bool ok;
    if (conv_as128) {
        snapload::sna_to_z80_header(h, (uint8_t) pc, (uint8_t) (pc >> 8), buf);
        buf[snapload::X_7FFD] = pconv::port_7ffd(c.machine);
        buf[snapload::X_AYFLAG] = 1;
        buf[snapload::X_AYSEL] = pconv::ay_selected(c.machine);
        memcpy(buf + snapload::X_AYREGS, pconv::ay_registers(c.machine), 16);
        ok = snapload::write_z80_128(sink, buf, snap_io_buffer, sizeof(snap_io_buffer));
    } else {
        memcpy(buf, h, sizeof(h));
        ok = snapload::write_z80_48(sink, buf, snap_io_buffer, sizeof(snap_io_buffer), pc);
    }
    ok = c.file.close() && ok;
    if (!ok) fsops::remove_path(drive, out);
    LOG("Converted to %s: %s\n", out, ok ? "saved" : "not saved");
    return ok;
}

static void conv_close_tape() {
    if (conv_file_open) {
        conv_scratch().file.close();
        conv_file_open = false;
    }
}

// The end of a conversion, whatever happened: the buffers it borrowed are given back
static void conv_end() {
    conv_close_tape();
    conv_phase = CONV_IDLE;
    conv_overlay_on = false;
    pok_file.clear();
    walk_stop();                    // list_pool held the 128K ROMs: the list is read again
    info_loaded = false;            // snap_load_buffer held the Spectrum: read the page again
    info_source = nullptr;
    preview_screen = nullptr;
    notes_message = conv_msg;
    conv_result_shown = conv_msg[0] != '\0';
    conv_result_pic = rom_data[16384 + 4];
    conv_result_game = conv_path_hash(info_path);
}

// Why a conversion did not load, for the key row (at most 32 letters)
static const char* conv_why(pconv::Status st) {
    switch (st) {
    case pconv::TAPE_ERROR: return "Not loaded: tape loading error";
    case pconv::BAD_TAPE: return "Not loaded: damaged tape";
    case pconv::TOO_LONG: return "Not loaded: over 30 minutes";
    case pconv::STOPPED: return "Not loaded: the loader stopped";
    case pconv::BASIC_ERROR: return "Not loaded: BASIC error";
    case pconv::RESET: return conv_as128 ? "Not loaded: reset (try 48K)" : "Not loaded: reset (try 128K)";
    case pconv::SAVE: return conv_as128 ? "Not loaded: SAVE (try 48K)" : "Not loaded: went to SAVE";
    default: return "Not loaded";
    }
}

// ACTION_GAME_INFO op 6. True if the converter's screen was sent (byte 1 is 6); false for the
// normal page with notes_message, keep being byte 1 (7: the ROM waits for a key for it).
#ifdef PICOFACE_HOST
extern "C" int host_conv_slice_frames;
#endif
static bool conv_op(uint8_t mode, uint8_t key, uint8_t& keep) {
    uint8_t* nmi_rom_data = rom_data + 16384;
    keep = 0;
    if (conv_phase == CONV_IDLE) {
        // C was pressed: key is 1 if the ROM found the machine can page memory
        bool tzx;
        if (mode != 0 || !conv_is_tape(info_path, &tzx)) {
            notes_message = "C converts a tape (.tap .tzx)";
            return false;
        }
        conv_machine128 = key == 1;
        conv_phase = CONV_CHOOSE;
        notes_message = "Convert: A-Auto 4-48K 1-128K";   // (Auto: a 128K first if it can page)
        keep = 7;
        return false;
    }
    if (conv_phase == CONV_CHOOSE) {
        conv_phase = CONV_IDLE;
        if (key == 'a') conv_mode = CONV_AUTO;
        else if (key == '4') conv_mode = CONV_48;
        else if (key == '1') conv_mode = CONV_128;
        else {
            notes_message = "Not converted";
            return false;
        }
        tape_service_stop();        // its ROM is in rom_data, not in the buffers used here
        conv_retried = false;
        conv_msg[0] = '\0';
        conv_auto_saved = false;
        conv_auto_number = -1;
        conv_manual_saved = false;
        conv_note_until = 0;
        conv_scr_prepare();         // before the ROMs are read (see there)
        bool as128 = conv_mode == CONV_128 || (conv_mode == CONV_AUTO && conv_machine128);
        if (!conv_start(as128)) {
            conv_end();
            return false;
        }
        conv_phase = CONV_RUN;
        conv_started = time_us_64();
        key = 0;
    }
    // CONV_RUN
    ConvScratch& c = conv_scratch();
    if (key == 0x18 || key == 'x') {            // BREAK or X
        // the loading screen it saved by itself goes too; the ones saved with S are kept
        bool kept = conv_manual_saved;
        if (conv_auto_number >= 0) {
            char base[screengrab::NAME_MAX + 1];
            if (screengrab::base_name(info_name, base) == 0) snprintf(base, sizeof(base), "Screen");
            screengrab::path_in(screengrab::SCRAPER_FOLDER, base, conv_auto_number, ".scr", grab_path, sizeof(grab_path));
            fsops::remove_path(Drive::EXTERNAL_SD_CARD, grab_path);
            LOG("Convert stopped: %s removed\n", grab_path);
            conv_auto_number = -1;
        }
        snprintf(conv_msg, sizeof(conv_msg), kept ? "Stopped: only S screens kept" : "Stopped: nothing was saved");
        conv_end();
        return false;
    }
    bool tape_ok = true;
    if (key == 's' || key == 'S') {             // S: the screen as it is now
        tape_ok = conv_save_screen(pconv::screen(c.machine), false);
    }
    pconv::Status st = pconv::RUNNING;
    uint64_t until = time_us_64() + 120000;
#ifdef PICOFACE_HOST
    uint32_t slice_frames = 0;      // the simulator's "slice": so many frames, not 120 ms
#endif
    while (tape_ok && st == pconv::RUNNING && time_us_64() < until) {
#ifdef PICOFACE_HOST
        if (host_conv_slice_frames != 0 && ++slice_frames > (uint32_t) host_conv_slice_frames) break;
#endif
        st = pconv::run(c.machine, 1);
        if (pconv::loading_screen(c.machine)) {
            conv_auto_saved = true;
            tape_ok = conv_save_screen(pconv::screen_data(c.machine), true);
        }
    }
    if (!tape_ok) {
        snprintf(conv_msg, sizeof(conv_msg), "Stopped: cannot read the tape");
        conv_end();
        return false;
    }
    if (st != pconv::RUNNING && st != pconv::LOADED && conv_mode == CONV_AUTO && conv_as128 &&
        st != pconv::BAD_TAPE) {
        // Auto: a 128K did not load it, so a 48K
        conv_close_tape();
        conv_retried = true;
        LOG("Convert: the 128K did not load it (%d), now a 48K\n", (int) st);
        if (!conv_start(false)) {
            conv_end();
            return false;
        }
        if (conv_auto_saved) pconv::no_screen_watch(c.machine);    // it has been saved once
        conv_note_until = 0;
        st = pconv::RUNNING;
    }
    if (st != pconv::RUNNING) {
        conv_close_tape();
        if (st == pconv::LOADED) {
            char saved[gameinfo::GAME_MAX + 8];         // "<name> 9.z80"
            if (conv_save(saved, sizeof(saved))) {
                // "Saved <name>.z80", and " (check it)" if it may still be waiting for the
                // tape; a long name loses its start so that it fits the 31 letters of the row
                const char* tail = pconv::suspect(c.machine) != pconv::NONE ? " (check it)" : "";
                size_t room = 31 - 6 - strlen(tail), n = strlen(saved);
                if (n > room) snprintf(conv_msg, sizeof(conv_msg), "Saved ..%s%s", saved + n - (room - 2), tail);
                else snprintf(conv_msg, sizeof(conv_msg), "Saved %s%s", saved, tail);
            } else {
                snprintf(conv_msg, sizeof(conv_msg), "Could not save the snapshot");
            }
        } else {
            snprintf(conv_msg, sizeof(conv_msg), "%s", conv_why(st));
        }
        conv_end();
        return false;
    }
    // still going: the bar, the percentage of the file read and the time taken
    uint32_t pos = c.file.tell();
    uint32_t pct = conv_size > 0 ? (uint32_t) ((uint64_t) pos * 100 / conv_size) : 0;
    if (pct > 99) pct = 99;
    uint32_t secs = (uint32_t) ((time_us_64() - conv_started) / 1000000);
    char t[40];
    int tn = snprintf(t, sizeof(t), "%u:%02u S-Screen BREAK-Stop", (unsigned) (secs / 60), (unsigned) (secs % 60));
    int cells = 31 - tn;                        // the bar fills the rest (8 cells, 7 from 10 minutes)
    conv_row(1, gameinfo::ATTR_KEYS, cells + 1, t);
    ConvScratch& cs = conv_scratch();
    for (int i = 0; i < cells; i++) {
        cs.attr[1][i] = (uint32_t) i * 100 < pct * (uint32_t) cells ? 0x20 : 0x08;   // green or blue paper
    }
    if (conv_note_until != 0 && time_us_64() >= conv_note_until) {
        conv_note_until = 0;
        conv_titles();
    }
    preview_screen = pconv::screen(c.machine);
    conv_overlay_on = true;
    nmi_rom_data[1] = 6;
    nmi_rom_data[0] = 1;            // one page, no pictures
    return true;
}


#ifdef PIO_DEBUGGING

void setup_pios() {
    PIO pio = pio0;
    pio_sm_claim(pio, MY_SM);
    pio_sm_claim(pio, SM_OUTDATA);
    

    uint offset = pio_add_program(pio, &pushpull_program);
    pio_sm_config sm_config = pushpull_program_get_default_config(offset);
    pio_sm_init(pio, MY_SM, offset, &sm_config);
    pio_sm_set_enabled(pio, MY_SM, true);

    pio_sm_init(pio, SM_OUTDATA, offset, &sm_config);
    pio_sm_set_enabled(pio, SM_OUTDATA, true);

}


void run_pio_test() {


    serverom();
}

int main() {
    tusb_init();            // the USB serial port and drive (usb_descriptors.c); stdio uses it
    stdio_init_all();
    setup_pios();

    rom_state.flags = 2;

    multicore_launch_core1(serverom);


    sleep_ms(1000);


    PIO pio = pio0;

    uint32_t dataval = (0x2 << 14) | 0x66;

    pio_sm_put_blocking(pio, MY_SM, dataval); // entry to nmi
    pio_sm_put_blocking(pio, MY_SM, (0x2 << 14) | EXITNMI); // exit from nmi


    while (true) {
        sleep_ms(1000);
    }

}


#else



int main() {
    tusb_init();            // the USB serial port and drive (usb_descriptors.c); stdio uses it
    stdio_init_all();

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_PROC1_BITS;

    memcpy(rom_data + 16384, NMI_ROM, NMI_ROM_SIZE);
    rom_state.flags = 0;

    multicore_launch_core1(do_my_pio);

    gpio_init(PIN_USER_SWITCH);
    gpio_set_dir(PIN_USER_SWITCH, false);
    gpio_pull_up(PIN_USER_SWITCH);

    gpio_init(PIN_NMI);
    gpio_put(PIN_NMI, true);
    gpio_set_dir(PIN_NMI, true);    


    sleep_ms(200);
    initFileSystem();

    #ifdef WIFI_ENABLE
    if (cyw43_arch_init()) {
        printf("failed to initialise\n");
        return 1;
    }

    cyw43_arch_enable_sta_mode();

    printf("Connecting to Wi-Fi...\n");

    if (cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK) != 0) {
        printf("failed to connect.\n");
        return 1;
    }

    #endif

    uint8_t* nmi_rom_data = rom_data + 16384;

    bool areConnected = false;


    while (true) {
        #ifdef WIFI_ENABLE
        #if PICO_CYW43_ARCH_POLL
                // if you are using pico_cyw43_arch_poll, then you must poll periodically from your
                // main loop (not from a timer) to check for Wi-Fi driver or lwIP work that needs to be done.
                cyw43_arch_poll();
                // you can poll as often as you like, however if you have nothing else to do you can
                // choose to sleep until either a specified time, or cyw43_arch_poll() has work to do:
                //cyw43_arch_wait_for_work_until(make_timeout_time_ms(2));
        #else
                // if you are not using pico_cyw43_arch_poll, then WiFI driver and lwIP work
                // is done via interrupt in the background. This sleep is just an example of some (blocking)
                // work you might be doing.
        #endif
        #endif
        usb_msc_task();
        if (!usb_msc_busy()) {
            sleep_ms(1);        // while the PC is using the SD card go as fast as the card does
        }
        pollUsbCommandHandler();

        #ifdef WIFI_ENABLE
        bool linkup = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP;
        if (linkup != areConnected) {
            printf("WIFI connected: %d\n", linkup);
            if (linkup) {
                start_http_server();
            }
            areConnected = linkup;
        }
        #endif

        if (nmi_rom_data[0] == 255) {
            sd_setup_early();
            processNmiRequest();
        }

        tape_service_poll();

        bool isup = gpio_get(PIN_USER_SWITCH);
        if (!isup) {

            extras::Press press = extras::classify_press(
                [] { return !gpio_get(PIN_USER_SWITCH); }, [](uint32_t ms) { sleep_ms(ms); });

            if (press == extras::Press::SHORT) {
                if (!sendNmiRequest(0, 0, 0)) {
                    LOG("Couldn't send NMI because an NMI is already active.\n");
                }
            } else if (press == extras::Press::DOUBLE) {
                // Two quick presses: save the screen with no menu (the border shows how it went)
                if (!sendNmiRequest(STARTUP_ACTION_GRAB, 0, 0)) {
                    LOG("Couldn't send the screen grab because an NMI is already active.\n");
                }
                while (!gpio_get(PIN_USER_SWITCH)) {
                    sleep_ms(10);       // until the button is let go
                }
            } else {
                // Held for a second: reset the Spectrum into its own ROM
                reset_spectrum_to_internal_rom();
                while (!gpio_get(PIN_USER_SWITCH)) {
                    sleep_ms(10);       // until the button is let go
                }
            }
        }
    }
    
}

#endif