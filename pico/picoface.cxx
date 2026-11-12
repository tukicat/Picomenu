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
#include "hardware/structs/bus_ctrl.h"
#include "usb_command_handler.h"
#include "usb_msc.h"
#include "gameinfo.h"
#include "screengrab.h"
#include "settings.h"
#include "slots.h"
#include "options.h"
#include "menushot.h"
#include "z80tap.h"
#include "notes.h"
#include "sdsetup.h"
#include <new>
#include "tusb.h"


#include "webfiles.h"
#ifdef PICOFACE_HOST
#include <string>
#endif
#ifdef WIFI_ENABLE
#include "wifi.h"
#endif
// B - Bluetooth tape in Tools: in the Pico W build, and in the simulator (with a pretend
// Bluetooth, tools/picosim/bt_host.cpp, whose sound goes to the emulated Spectrum's EAR)
#if defined(WIFI_ENABLE) || defined(PICOFACE_HOST)
#define PICOMENU_BT 1
#include "bt.h"
#include "tapeaudio.h"
#else
#define PICOMENU_BT 0
#endif
// ZXDB (Spectrum Computing's database, through the ZXInfo API) from the WiFi screen and the
// game information's M menu: in the Pico W build, and in the simulator (a pretend network,
// tools/picosim/net_host.cpp, answering with files its script names)
#if defined(WIFI_ENABLE) || defined(PICOFACE_HOST)
#define PICOMENU_NET 1
#include "net.h"
#include "zxdb.h"
#include "unzip.h"
static bool zx_live = false;                // a ZXDB screen is up (zx(), further on, holds its state)
#else
#define PICOMENU_NET 0
#endif
#define PICOMENU_ASSETS 1           // E in Tools, Erase all assets: both firmwares
#if PICOMENU_BT
// The Bluetooth (B - Bluetooth tape, further on): on, and its screen open
static bool bt_on = false;
static bool bt_screen = false;
static bool bt_playing = false;             // P: playing (silence while the menu is open)
static bool bt_tape_ready = false;          // the Bluetooth's player is on tape_file, where it was left
static bool bt_owns = false;                // the tape is the Bluetooth's: the trap's hook has the ROM load from EAR
static void bt_off();
static void nmi_action_bt(uint8_t key);
static void bt_poll();
static bool bt_keeps(uint8_t action);
static uint32_t bt_position();
static bool bt_takes_load();
static void bt_browse_takes();
static bool tape_asking = false;           // T in Browse Games: "Trap or Bluetooth?" asked
#endif
#include "littlefs-lib/pico_hal.h"


#include "picoface.pio.h"

#include "f_util.h"
#include "ff.h"
#include "diskio.h"
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
static void checkpoint_took();          // the checkpoints: see "---- Checkpoints"
static void checkpoints_stop();
static void tape_service_stop();
static bool tape_in = false;        // the tape being played was put in with T (tape in, see tape_in_start)
static void tape_in_hook_to_rom();

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
    // Tape in (piohandler.S): what the bus handler looks for once the NMI ROM has exited.
    // exit_after_nmi is the tape-in hook's exit (shifted, as the handler compares it) while
    // the hook runs, else 1; the handler sets it. tape_entry is the hook's entry (shifted) when
    // a tape is in, else 1; the firmware sets it while the NMI menu is up.
    uint32_t exit_after_nmi;
    uint32_t tape_entry;
    uint32_t tape_exit;         // the watched hook's last byte (shifted, as exit_after_nmi)
};

volatile struct MYSTATE rom_state = {
    .flags = 0,  .nmi_active=0, .writableStartAddress=32768, .flags_on_nmi_exit= 0,
    .exit_after_nmi = 1, .tape_entry = 1, .tape_exit = 1
};
static_assert(offsetof(MYSTATE, exit_after_nmi) == 16 && offsetof(MYSTATE, tape_entry) == 20 &&
    offsetof(MYSTATE, tape_exit) == 24,
    "piohandler.S reads rom_state at these offsets");
uint8_t rom_data[32768];
// The menus' font (96 letters of 8 bytes from space): the NMI ROM's own, in rom_data (helptext.h
// has a copy for the PC tools; the firmware uses the ROM's and leaves that out)
static inline const uint8_t (*menu_font())[8] {
    return reinterpret_cast<const uint8_t (*)[8]>(rom_data + 16384 + NMI_CHARBANK);
}

// HIGH-SPEED TAPE ENGINE GLOBAL STATE
extern "C" {
    void piohandler();                        // Keep this assembly function here
}

static bool enable_logging = true;

// The messages on the USB serial port (for finding faults with a PC). Left out of the firmware
// unless it is built with -DPICOMENU_LOG=1 (their text was about 5 KB of it); the simulator has them.
#if defined(PICOFACE_HOST) || (defined(PICOMENU_LOG) && PICOMENU_LOG)
void LOG(const char* format, ...) {
    if (enable_logging) {
        va_list args;
        va_start(args, format);
        vprintf(format, args);
    }
}
#else
#define LOG(...) do { } while (0)
#endif


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

#ifdef PICOMENU_SNAP_SIZE
// The Pico W build: placed by its linker script (memmap_picomenu_w.ld), which puts the
// radios' memory (the Bluetooth's, the ZXDB downloads') in its last 24 KB (see bt_off)
static_assert(PICOMENU_SNAP_SIZE == SNAP_LOAD_BUFFER_SIZE, "CMakeLists.txt's PICOMENU_SNAP_SIZE is snap_load_buffer's size");
extern "C" char picomenu_snapbuf[SNAP_LOAD_BUFFER_SIZE];
static char (&snap_load_buffer)[SNAP_LOAD_BUFFER_SIZE] = picomenu_snapbuf;
#else
static char snap_load_buffer[SNAP_LOAD_BUFFER_SIZE] __attribute__((aligned(8)));
#endif
static_assert(SNAP_LOAD_BUFFER_SIZE >= SNA_LOAD_SIZE + 1024, "buffer too small for a 48K snapshot");

// Snapshot files are read through this a piece at a time
static uint8_t snap_io_buffer[512];

// snap_load_buffer holds a 128K snapshot, as built by snapload.h.  Only
// meaningful between loading a snapshot file and sending it to the NMI rom.
static bool snap_is_paged = false;
static bool sna_in_buffer = false;          // a snapshot sent to be saved is in snap_load_buffer

// ACTION_SNA_BEGIN_WRITE128 was the last way a snapshot was sent for saving: it is
// a 128K one, which is saved as a .z80 file.  save128_info is the header it sent.
static bool save_is_128k = false;
static uint8_t save128_info[snapload::SAVE128_HEADER];
// The pause the game had for its last autosave (Help > Diagnostics): from the NMI to the save
static uint32_t cp_pause_us = 0, cp_pause_max_us = 0;
static bool cp_pause_128 = false;
static uint64_t cp_nmi_at = 0;              // time_us_64() at the autosave's NMI
static bool nmi_menu_open = false;      // the NMI menu (not a grab, a load ...): see ayswap in nmirom.asm
static_assert(SAVE128_AY_AT + 16 <= snapload::SAVE128_HEADER, "the sound chip registers are in the save header");

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
// In the Waste Bin (or it), on the SD card: erasing there is for good
static bool in_waste_bin(uint8_t drive, const char* path) {
    return drive == 1 && (strcasecmp(path, "/Waste Bin") == 0 || strncasecmp(path, "/Waste Bin/", 11) == 0);
}
static char flash_cwd[sizeof(sd_cwd)] = "";

// The directory being browsed on a drive (0 internal flash, 1 SD card)
static char* cwd_of(uint8_t drive) {
    return drive == 1 ? sd_cwd : flash_cwd;
}

// The drive the last ACTION_SNAP_LIST was for
static uint8_t list_drive = 0;

// Picomenu.ini (settings.h): read at power on, when the PC gives the card back (cfg_stale) and
// when it is saved on the Spectrum. Its start_folder is used at power on and when the card
// comes back from the PC (cfg_folder_due), not each time it is read.
static settings::Settings cfg;
static bool cfg_stale = false;          // read it again before it is next used
static bool cfg_folder_due = true;
static void settings_fresh();
// SD card trouble put right since power on (Help > Diagnostics): reads tried again and failed
// (FatFs_SPI/src/glue.c), and times the card was mounted again (sd_check)
extern "C" unsigned long sd_read_retries, sd_read_failures;
static uint32_t sd_remounts = 0;
// A colour of the pico's own screens (not of a picture) as Picomenu.ini's theme shows it
static inline uint8_t ui_attr(uint8_t a) { return settings::theme_attr(a, cfg.light); }
static size_t slot_game_now(char* out);     // save slots: the game being played
// High score tables (hiscore.h, further on): the Pico W build (and the simulator, unless
// PICOMENU_NO_HISCORE: the standard build's screens)
#if (defined(WIFI_ENABLE) || defined(PICOFACE_HOST)) && !defined(PICOMENU_NO_HISCORE)
#define PICOMENU_HISCORE 1
static void hs_commit();
static bool hs_leave(uint8_t* nmi_rom_data, uint8_t key);
static bool hs_leaving();
static bool hs_auto_poll();
static bool hs_copied();
static void hs_tape_game(const char* tape);
// The game load_snapshot_file has ready (its table goes into it when it is sent: hs_commit)
static struct { char name[96]; bool valid, tape; } hs_next;
static size_t game_base(const char* dir, const char* name, char* out);
#else
#define PICOMENU_HISCORE 0
#endif
static uint32_t tape_block_ms = 0;  // when the tape trap last gave the game a block
static bool hs_bt_played = false;   // the Bluetooth has played the tape put in (the trap gives it no blocks)

// Big enough for sd_cwd, a slash and the longest file name
static const size_t BROWSE_PATH_MAX = sizeof(sd_cwd) + 260;

// The text the snapshot browser is searching for (ACTION_SNAP_FIND), or ""
// when it is simply listing a directory.
static char snap_filter[16] = "";

// The directory that each entry of the last ACTION_SNAP_LIST is in, in the
// same form as sd_cwd.  A search returns entries from many directories, and
// this is how we know where to find the one that the user then picks.
static const int SNAP_LIST_MAX = 10;
#ifdef WIFI_ENABLE
// (the Pico W build: in the scratch RAM bank, where core 1's stack is, as the main RAM is all used)
static char list_entry_dir[SNAP_LIST_MAX][sizeof(sd_cwd)] __attribute__((section(".scratch_x.list_entry_dir")));
#else
static char list_entry_dir[SNAP_LIST_MAX][sizeof(sd_cwd)];
#endif
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
    // The places the blocks went: a block that lands on one loaded before is a multi-load
    // tape (Gauntlet's 31 levels all go to 49152), which needs the real tape (the ROM trap)
    // for its later loads, so it is not a clean conversion.
    static const int PLACED_MAX = 16;
    uint32_t placed_lo[PLACED_MAX], placed_hi[PLACED_MAX];
    int placed = 0;

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
                uint32_t lo = 16384u + (uint32_t) (dest - ram), hi = lo + toread;
                for (int i = 0; i < placed; i++) {
                    if (clean && lo < placed_hi[i] && placed_lo[i] < hi) {
                        LOG("Tape Loader: a block loads over another (a multi-load tape)\n");
                        clean = false;
                    }
                }
                if (placed < PLACED_MAX) {
                    placed_lo[placed] = lo;
                    placed_hi[placed++] = hi;
                } else {
                    clean = false;                // more blocks than a simple tape has
                }
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
        file.close();               // (opened, but not a .tzx: not left open)
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
// " W" after the number: the Pico W build (Update PicoMenu only takes the same kind: fwupdate.h)
#ifdef WIFI_ENABLE
static const char FIRMWARE_ID[] = "Rev 2.6.1 W (tapeloader v261)";
#else
static const char FIRMWARE_ID[] = "Rev 2.6.1 (tapeloader v261)";
#endif
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
// what the file holds, as its length and CRC-32 (not a copy: the RAM is short)
static uint32_t lastgame_stored_crc = 0;
static size_t lastgame_stored_len = 0;
static uint32_t lastgame_crc(const uint8_t* b, size_t n) { return ~fwupdate::crc32_update(0xFFFFFFFFu, b, (uint32_t) n); }

// The last game's record and its file's bytes: one copy, for reading it at start up and writing
// it after (the RAM is short: what is left of it is the heap, which littlefs needs)
static extras::LastGame lastgame_g;
static uint8_t lastgame_buf[extras::LG_FILE_MAX];

static void persist_last_game() {
    extras::LastGame& g = lastgame_g;
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
    uint8_t* buf = lastgame_buf;
    size_t len = extras::lastgame_encode(g, buf);
    if (len == lastgame_stored_len && lastgame_crc(buf, len) == lastgame_stored_crc) {
        return;
    }
    int file = pico_open(LASTGAME_FILE, LFS_O_CREAT | LFS_O_WRONLY | LFS_O_TRUNC);
    if (file >= 0) {
        int written = pico_write(file, buf, len);
        pico_close(file);
        if (written == (int) len) {
            lastgame_stored_crc = lastgame_crc(buf, len);
            lastgame_stored_len = len;
        }
    }
}

static void load_last_game() {
    uint8_t* buf = lastgame_buf;
    int file = pico_open(LASTGAME_FILE, LFS_O_RDONLY);
    if (file < 0) {
        return;
    }
    int n = pico_read(file, buf, sizeof(lastgame_buf));
    pico_close(file);
    extras::LastGame& g = lastgame_g;
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
        lastgame_stored_crc = lastgame_crc(buf, (size_t) n);
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

// The tape position kept with a snapshot: see tape_note_write
static void tape_note_resume(uint8_t drive, const char* snap, bool paged, uint8_t port_7ffd);
static void tape_note_after_save(uint8_t drive, const char* snap);
static void tape_note_remove(uint8_t drive, const char* snap);
// The snapshot ACTION_PREPARE_LOAD made ready is a game (a file loaded), not a reset or a test
// ROM: autosave asks only for a game
static bool prepared_is_game = false;
static void tape_note_follow(uint8_t from_drive, const char* from, uint8_t to_drive, const char* to, bool keep);
static void tape_notes_follow_folder(uint8_t drive, const char* from, const char* to, bool keep);
static void tape_notes_retarget(uint8_t from_drive, const char* from, uint8_t to_drive, const char* to, bool is_dir);
static void tape_labels_follow(uint8_t from_drive, const char* from, uint8_t to_drive, const char* to, bool is_dir);

// remember false: a checkpoint, which is still the game that was loaded (R - Reload, the
// recent list and the save screen's name are left as they are)
void load_snapshot_file(uint8_t drive, const char* dir, const char* fileAndExt, bool remember = true) {
    game_running = true;
    prepared_is_game = true;
    // a snapshot expects the internal ROM; stop any earlier tape trap
    tape_service_stop();
    snap_is_paged = false;

    // fileAndExt is the name and the extension as two strings, one after the
    // other.  On the SD card the file is in directory dir (same form as sd_cwd).
    static char filename[BROWSE_PATH_MAX];
    const char* ext = fileAndExt + strlen(fileAndExt) + 1;
    snprintf(filename, sizeof(filename), "%s/%s.%s", dir, fileAndExt, ext);
    if (remember) {
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
    }

#if PICOMENU_HISCORE
    game_base(dir, fileAndExt, hs_next.name);
    hs_next.valid = true;
    hs_next.tape = false;
#endif
    if (strcasecmp(ext, "tap") == 0 || strcasecmp(ext, "tzx") == 0) {
#if PICOMENU_HISCORE
        hs_next.tape = true;            // (its table is put back at the menu, once it is set up)
#endif
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
    // a tape position kept with the snapshot: the tape trap carries on from there
    tape_note_resume(drive, filename, kind == snapload::KIND_PAGED, buf[snapload::X_7FFD]);
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
// L-All in Browse Games: hidden files and folders are listed too, dimmed (the ROM asks with bit 2
// of the list request's flags). Hidden are: those hidden with H (on the SD card the hidden
// attribute, as Windows sets it; on the flash a littlefs attribute), and always the ones that
// cannot be loaded (.scr and .txt files; the Poke List, Tape Files and old Screen Grabs folders),
// system ones and names starting with a dot.
static bool list_all = false;

inline bool is_listed_ext(const char* extension) {
    return is_snapshot_ext(extension) || strcasecmp(extension, "rom") == 0 || strcasecmp(extension, "pok") == 0 ||
        strcasecmp(extension, "scr") == 0 || strcasecmp(extension, "txt") == 0 || strcasecmp(extension, "ini") == 0;
}

// A list entry is the name and the extension as two strings, then a byte of these flags
static const uint8_t ENTRY_HIDDEN = 1;          // shown dimmed, and only with L-All
static const uint8_t ENTRY_HIDDEN_SET = 2;      // hidden with H (or by Windows): H shows it again
static const uint8_t ENTRY_ALWAYS = 4;          // hidden whatever is set: H can't show it
static const uint8_t FLASH_HIDDEN_ATTR = 'H';   // the littlefs attribute of a file hidden on the flash
// The bytes of an entry's two strings (without the flags)
static size_t entry_len(const char* nameAndExt) {
    size_t n = strlen(nameAndExt) + 1;
    return n + strlen(nameAndExt + n) + 1;
}
static uint8_t entry_flags(const char* nameAndExt) {
    return (uint8_t) nameAndExt[entry_len(nameAndExt)];
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

bool add_file_to_list(const char* name, const uint16_t destoffset, uint16_t& stringoffset, bool hidden = false) {
    uint8_t* nmi_rom_data = rom_data + 16384;
    int index = nmi_rom_data[destoffset];
    if (index >= 10) {
        nmi_rom_data[destoffset + 1] = 1;
        return false;
    }
    nmi_rom_data[destoffset] = index + 1;
    nmi_rom_data[destoffset + 2 + 2 * index] = stringoffset & 0xFF;
    nmi_rom_data[destoffset + 2 + 2 * index + 1] = (stringoffset >> 8) & 0xFF;
    if (hidden) {
        nmi_rom_data[destoffset + SNAPLIST_HIDDEN + index / 8] |= (uint8_t) (1 << (index % 8));
    }
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

// Sub directories that can be listed ("." and ".." never are; hidden ones only with L-All, which
// walk_next sees to)
static bool is_listable_dir(DirHandle& dir) {
    const char* n = dir.fileName();
    return dir.isDirectory() && strcmp(n, ".") != 0 && strcmp(n, "..") != 0;
}

static char list_path[BROWSE_PATH_MAX];
// A file or folder on the flash hidden with H. dir is in the form of sd_cwd
static bool flash_hidden(const char* dir, const char* name) {
    int n = snprintf(list_path, sizeof(list_path), "%s/%s", dir, name);
    if (n < 0 || n >= (int) sizeof(list_path)) return false;
    uint8_t v = 0;
    return pico_getattr(list_path, FLASH_HIDDEN_ATTR, &v, 1) == 1 && v != 0;
}

// A directory is returned with a '/' on the end of its name and an empty
// extension
static void make_dir_entry(char* fname) {
    size_t len = strlen(fname);
    fname[len] = '/';
    fname[len + 1] = '\0';
    fname[len + 2] = '\0';
}

// Room for a name, a '/', two terminators and the flags
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
// aligned: FatFs is lent this memory (ff_pool) for its long name buffer, which it reads and writes
// 16 bits at a time, and the Pico's Cortex-M0+ stops (a HardFault) on a 16-bit access at an odd
// address. A char array may be put anywhere: in a test build it was at 0x2002c855
static char list_pool[LIST_POOL_SIZE] __attribute__((aligned(8)));
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
    size_t namelen = entry_len(nameAndExt) + 1;      // and the flags
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

// Picomenu.ini's sort and folders change this (settings::compare_names)
static int compare_entry_names(const char* a, const char* b) {
    return settings::compare_names(a, b, cfg.sort, cfg.folders);
}

// Sorts n 16-bit entries (a shell sort: the C library's qsort was 1.5 KB of the firmware)
static void sort_u16(uint16_t* a, size_t n, int (*cmp)(const void*, const void*)) {
    size_t gap = 1;
    while (gap < n / 3) gap = gap * 3 + 1;
    for (; gap > 0; gap /= 3) {
        for (size_t i = gap; i < n; i++) {
            uint16_t v = a[i];
            size_t j = i;
            while (j >= gap && cmp(&a[j - gap], &v) > 0) {
                a[j] = a[j - gap];
                j -= gap;
            }
            a[j] = v;
        }
    }
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
    sort_u16(list_cache, list_cached, compare_cached);
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
    memcpy(to, nameAndExt, entry_len(nameAndExt) + 1);      // and the flags
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
        // hidden with H: on the SD card its attribute; on the flash a littlefs attribute
        uint8_t flags = 0;
        if (fname[0] == '.' || d.isSystem()) {
            flags = ENTRY_HIDDEN | ENTRY_ALWAYS;
        } else if (walk.drive == 1 ? d.isHiddenSet() : flash_hidden(walk.path + 3, fname)) {
            flags = ENTRY_HIDDEN | ENTRY_HIDDEN_SET;
        }
        if (!d.isDirectory()) {
            if (walk.mode == WALK_DIRS_THEN_FILES && walk.phase == 0) {
                continue;
            }
            const char* ext = split_filename_and_get_ext(fname);
            if (strcasecmp(ext, "scr") == 0 || strcasecmp(ext, "txt") == 0 || strcasecmp(ext, "ini") == 0) {
                flags |= ENTRY_HIDDEN | ENTRY_ALWAYS;       // pictures and text (Picomenu.ini too): not loaded
            }
            if (is_listed_ext(ext) && (list_all || !(flags & ENTRY_HIDDEN)) &&
                matches_snap_filter(fname, walk.filter)) {
                fname[entry_len(fname)] = (char) flags;
                return WALK_FOUND;
            }
            continue;
        }
        if ((walk.mode == WALK_DIRS_THEN_FILES && walk.phase == 1) || !is_listable_dir(d)) {
            continue;
        }
        if ((*dir)[0] == '\0' &&
                ((walk.drive == 1 && (strcasecmp(fname, "Poke List") == 0 || strcasecmp(fname, "Screen Grabs") == 0)) ||
                 strcasecmp(fname, "Tape Files") == 0)) {
            // Poke List: only a PC puts files there; Screen Grabs: V shows them (grabs saved before
            // a test build); Tape Files: the tape positions of snapshots (either drive)
            flags |= ENTRY_HIDDEN | ENTRY_ALWAYS;
        }
        if ((flags & ENTRY_HIDDEN) && !list_all) {
            continue;
        }
        if (walk.mode == WALK_FIND) {
            walk.descend = true;    // look inside it before moving on
            if (!matches_snap_filter(fname, walk.filter)) {
                continue;
            }
        }
        make_dir_entry(fname);
        fname[entry_len(fname)] = (char) flags;
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
    char* path = list_path;
    snprintf(path, sizeof(list_path), "%s/%s.%s", dir, nameAndExt, ext);
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
    if (!add_file_to_list(shown, destoffset, stringoffset, (entry_flags(nameAndExt) & ENTRY_HIDDEN) != 0)) {
        return false;
    }
    // the details mean opening each file: not while a search is still going, as the results
    // found so far are sent again with each request, and opening them all took all the time a
    // request has, so the search never got any further (it hung until BREAK). The finished
    // search's page is sent again, with them.
    bool searching = walk.order == ORDER_FOUND && walk.depth >= 0;
    if (list_details && nameAndExt[strlen(nameAndExt) + 1] != '\0') {
        // the ROM reads a third string for each file while the details are shown, so one is
        // always sent: while a search goes on, "..." in its place (a test build sent none, and the
        // list was read out of step: garbled until the search was over)
        char info[9];
        if (searching) snprintf(info, sizeof(info), "   ...  ");
        else read_file_details(list_drive, dir, nameAndExt, info);
        memcpy(reinterpret_cast<char*>(rom_data + 16384) + stringoffset, info, strlen(info) + 1);
        stringoffset += strlen(info) + 1;
    }
    if (list_entry_count < SNAP_LIST_MAX) {
        list_entry_offset[list_entry_count] = offset;
        snprintf(list_entry_dir[list_entry_count], sizeof(list_entry_dir[0]), "%s", dir);
        size_t len = entry_len(nameAndExt) + 1;      // and the flags (H looks at them)
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
    nmi_rom_data[destoffset + SNAPLIST_HIDDEN] = 0;
    nmi_rom_data[destoffset + SNAPLIST_HIDDEN + 1] = 0;
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
    bool was_searching = walk.order == ORDER_FOUND && walk.depth >= 0;     // (list_out: no details then)

    // A page is SNAP_LIST_MAX results, and one more tells us there is a next page
    int need = start + SNAP_LIST_MAX + 1;
    int out = start;

    if (walk.order == ORDER_BIG) {
        big_cover(start);
        const char* dir = walk.cwd;
        for (; out < need && out < big_start + big_count; out++) {
            list_out(big_entry(out - big_start), dir, destoffset, stringoffset);
        }
        nmi_rom_data[destoffset + SNAPLIST_HASPARENT] = (cwd_of(drive)[0] != '\0' ? 1 : 0) | (in_waste_bin(drive, cwd_of(drive)) ? 2 : 0);
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
            memcpy(walk_last_name, list_fname, entry_len(list_fname) + 1);
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
    bool resend = false;
    if (!busy && walk.order == ORDER_FOUND && walk.depth < 0 && !walk.sorted &&
        !list_cache_full && walk.produced == list_cached) {
        list_cache_sort();
        walk.sorted = true;
        resend = true;
    } else if (!busy && list_details && was_searching && walk.depth < 0 &&
               (need <= list_cached || walk.produced == list_cached)) {
        resend = true;      // the search has just finished, and the page is in the cache: again, with the details
    }
    if (resend) {
        nmi_rom_data[destoffset] = 0;
        nmi_rom_data[destoffset + 1] = 0;
        nmi_rom_data[destoffset + SNAPLIST_HIDDEN] = 0;
        nmi_rom_data[destoffset + SNAPLIST_HIDDEN + 1] = 0;
        list_entry_count = 0;
        stringoffset = destoffset + SNAPLIST_STRINGS;
        for (out = start; out < need && out < list_cached; out++) {
            list_out(list_cache_name(out), list_cache_dir(out), destoffset, stringoffset);
        }
    }

    nmi_rom_data[destoffset + SNAPLIST_HASPARENT] = (cwd_of(drive)[0] != '\0' ? 1 : 0) | (in_waste_bin(drive, cwd_of(drive)) ? 2 : 0);
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
// A tape being played (the tape trap's) has its file open, which FatFs won't let be erased,
// moved or renamed: before that is done to it, or to a folder it is in, it is ejected
static void tape_eject_if_in(uint8_t drive, const char* path);

static const uint8_t FILEOP_OK = fsops::OK;
static const uint8_t FILEOP_NAME_IN_USE = fsops::NAME_IN_USE;
static const uint8_t FILEOP_FAILED = fsops::FAILED;
static const uint8_t FILEOP_SAME_DIRECTORY = 3;
// ---- The Waste Bin: /Waste Bin, a hidden folder in the root of the SD card ----
// Erasing a file or folder in Browse Games (or a screen grab with D on V) moves it here instead,
// a file on the internal flash being copied here (a folder on the flash is erased, as folders
// don't change drive). Erasing one in the Waste Bin removes it for good; W on the menu opens
// Browse Games in it, where B empties it.
static const char WASTE_BIN[] = "/Waste Bin";
// The folder Browse Games was in on the SD card when W opened the bin: it goes back there when
// the bin is left (op 2, or the next time the menu opens), so B doesn't show the bin
static char bin_return_cwd[sizeof(sd_cwd)];
static bool bin_open = false;
static void bin_leave() {
    if (bin_open) {
        bin_open = false;
        if (in_waste_bin(1, sd_cwd)) {          // still in it (else the user has gone elsewhere)
            snprintf(sd_cwd, sizeof(sd_cwd), "%s", bin_return_cwd);
            snap_filter[0] = '\0';
        }
    }
}
// The paths erasing (and emptying the Waste Bin) works with (static: core 0's stack is small)
static char erase_path[BROWSE_PATH_MAX];
static char erase_scratch[BROWSE_PATH_MAX + 8];
static uint8_t make_sd_folder(const char* path);
static uint8_t copy_file(uint8_t from_drive, const char* from, uint8_t to_drive, const char* to);

// Makes the Waste Bin (hidden) if it is not there. False if it can't be
static bool waste_bin_make() {
    if (fsops::kind(Drive::EXTERNAL_SD_CARD, WASTE_BIN) == fsops::KIND_DIR) {
        return true;
    }
    uint8_t made = make_sd_folder(WASTE_BIN);
    if (made == fsops::OK) {
        f_chmod(WASTE_BIN, AM_HID, AM_HID);
    }
    return made == fsops::OK || made == fsops::NAME_IN_USE;
}

enum BinResult { BIN_OK, BIN_NONE, BIN_FAILED };   // BIN_NONE: there is no bin for it, erase it

// Moves path (a file or folder, on drive) into the Waste Bin, as its name or, if that is taken
// there, "<name> (2)" ... "<name> (99)" (before the extension). target gets its new path.
static BinResult to_waste_bin(uint8_t drive, const char* path, bool is_dir, char* target, size_t tsize) {
    if (!cfg.waste_bin || !sd_card_mounted || (drive != 1 && is_dir)) {     // waste_bin = off: erased for good
        return BIN_NONE;
    }
    const char* name = strrchr(path, '/');
    name = name != nullptr ? name + 1 : path;
    const char* dot = is_dir ? nullptr : strrchr(name, '.');
    int baselen = dot != nullptr ? (int) (dot - name) : (int) strlen(name);
    const char* ext = dot != nullptr ? dot : "";
    const char* from = path;                        // (not target's buffer)
    if (!waste_bin_make()) {
        return BIN_FAILED;
    }
    bool found = false;
    for (int n = 1; n < 100 && !found; n++) {
        int len = n == 1 ? snprintf(target, tsize, "%s/%.*s%s", WASTE_BIN, baselen, name, ext) :
            snprintf(target, tsize, "%s/%.*s (%d)%s", WASTE_BIN, baselen, name, n, ext);
        found = len > 0 && len < (int) tsize && fsops::kind(Drive::EXTERNAL_SD_CARD, target) == fsops::KIND_NONE;
    }
    if (!found) {
        return BIN_FAILED;
    }
    uint8_t r;
    if (drive == 1) {
        r = fsops::rename_path(Drive::EXTERNAL_SD_CARD, from, target);
    } else {
        r = copy_file(0, from, 1, target);
        if (r == fsops::OK && !fsops::remove_path(Drive::INTERNAL_FLASH, from)) {
            fsops::remove_path(Drive::EXTERNAL_SD_CARD, target);
            r = fsops::FAILED;
        }
    }
    LOG("Waste Bin: %s (drive %d) to %s: %d\n", from, drive, target, r);
    return r == fsops::OK ? BIN_OK : BIN_FAILED;
}

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
                    tape_notes_follow_folder(drive, from, to, true);
                    move_picked = false;
                }
                else if (status == FILEOP_FAILED) {
                    set_last_error("Cannot copy", move_name);
                }
            }
            else {
                walk_stop();        // directories change, and none may be open
                tape_eject_if_in(drive, from);
                status = fsops::rename_path(fsops::drive_of(drive), from, to);
                LOG("Move folder %s to %s (drive %d): %d\n", from, to, drive, status);
                if (status == FILEOP_OK) {
                    tape_notes_follow_folder(drive, from, to, false);
                    tape_notes_retarget(drive, from, drive, to, true);
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
                tape_eject_if_in(move_from_drive, from);
                status = fsops::move_file(fsops::drive_of(move_from_drive), from, fsops::drive_of(drive), to,
                    reinterpret_cast<uint8_t*>(list_pool), sizeof(list_pool));
                LOG("Move %s (drive %d) to %s (drive %d): %d\n", from, move_from_drive, to, drive, status);
                if (status == FILEOP_OK) {
                    tape_note_follow(move_from_drive, from, drive, to, false);
                    tape_notes_retarget(move_from_drive, from, drive, to, false);
                }
            }
            else {
                status = copy_file(move_from_drive, from, drive, to);
                LOG("Copy %s (drive %d) to %s (drive %d): %d\n", from, move_from_drive, to, drive, status);
                if (status == FILEOP_OK) tape_note_follow(move_from_drive, from, drive, to, true);
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
                if (status == FILEOP_OK) tape_note_follow(drive, from, drive, to, true);
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
                    tape_eject_if_in(drive, from);
                    status = fsops::rename_path(fsops::drive_of(drive), from, to);
                    LOG("Rename folder %s to %s: %d\n", from, to, status);
                    if (status == FILEOP_OK) {
                        tape_notes_follow_folder(drive, from, to, false);
                        tape_notes_retarget(drive, from, drive, to, true);
                    }
                    if (status == FILEOP_FAILED) {
                        set_last_error("Cannot rename", bare);
                    }
                }
            }
            else {
                entry_path(i, nullptr, from, sizeof(from));
                entry_path(i, newname, to, sizeof(to));
                walk_stop();
                tape_eject_if_in(drive, from);
                status = fsops::rename_path(fsops::drive_of(drive), from, to);
                LOG("Rename %s to %s: %d\n", from, to, status);
                if (status == FILEOP_OK) {
                    tape_note_follow(drive, from, drive, to, false);
                    tape_notes_retarget(drive, from, drive, to, false);
                }
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
        char* path = erase_path;
        const size_t path_size = sizeof(erase_path);
        const char* typed = reinterpret_cast<const char*>(nmi_rom_data) + param;
        if (is_rom && strcasecmp(typed, "ok") != 0) {
            status = FILEOP_NOT_CONFIRMED;      // a ROM file needs "ok" as a folder does
        }
        else if (!is_dir) {
            entry_path(i, nullptr, path, path_size);
            walk_stop();
            tape_eject_if_in(drive, path);
            char* target = erase_scratch;
            BinResult bin = in_waste_bin(drive, path) ? BIN_NONE : to_waste_bin(drive, path, false, target, sizeof(erase_scratch));
            if (bin == BIN_OK) {
                status = FILEOP_OK;
                tape_note_follow(drive, path, 1, target, false);
                tape_notes_retarget(drive, path, 1, target, false);
            } else if (bin == BIN_FAILED) {
                status = FILEOP_FAILED;
                set_last_error("Cannot move to", "the Waste Bin");
            } else {
                status = fsops::remove_path(fsops::drive_of(drive), path) ? FILEOP_OK : FILEOP_FAILED;
                LOG("Erase %s: %d\n", path, status);
                if (status == FILEOP_OK) {
                    tape_note_follow(drive, path, drive, nullptr, false);
                    tape_labels_follow(drive, path, drive, nullptr, false);     // a tape's labels (if it had any)
                }
                if (status != FILEOP_OK) {
                    set_last_error("Cannot erase", list_entry_name[i]);
                }
            }
        }
        else {
            // the user must have typed "ok"
            if (strcasecmp(typed, "ok") != 0) {
                status = FILEOP_NOT_CONFIRMED;
            }
            else {
                dir_entry_path(i, path, path_size);
                walk_stop();        // none of the directories may be open, and the list's memory is free
                tape_eject_if_in(drive, path);
                char* scratch = erase_scratch;
                const size_t scratch_size = sizeof(erase_scratch);
                BinResult bin = in_waste_bin(drive, path) ? BIN_NONE : to_waste_bin(drive, path, true, scratch, scratch_size);
                bool ok;
                if (bin == BIN_OK) {
                    ok = true;
                    tape_notes_follow_folder(drive, path, scratch, false);     // the notes go with it
                    tape_notes_retarget(drive, path, 1, scratch, true);
                } else if (bin == BIN_FAILED) {
                    ok = false;
                    set_last_error("Cannot move to", "the Waste Bin");
                } else {
                    ok = fsops::remove_tree(fsops::drive_of(drive), path, path_size, find_dirs, 0, ERASE_MAX_DEPTH,
                        scratch, scratch_size);
                    LOG("Erase directory %s: %d\n", path, ok);
                    if (ok) {
                        tape_notes_follow_folder(drive, path, nullptr, false);
                        tape_labels_follow(drive, path, drive, nullptr, true);
                    } else {
                        set_last_error("Cannot erase", list_entry_name[i]);
                    }
                }
                status = ok ? FILEOP_OK : FILEOP_FAILED;
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
        // (a slot, "/Save Games/Exolon/Slot 2", is "Exolon Save Game"; Q puts it in the slot, though)
        char game[screengrab::NAME_MAX + 1];
        const char* named = slots::folder_game(last_game_dir) != nullptr && slot_game_now(game) > 0 ? game : last_game_name;
        size_t len = !game_running ? 0 : last_game_usable() ? extras::quick_save_name(named, SAVE_NAME_MAX, quick) : 0;
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

// ---- Save slots (slots.h): nine saves of each game in /Save Games/<game> on the SD card ----

// Autosaves (checkpoints, see "---- Checkpoints"): kept in /Autosave/<game> as "Autosave 1" ...
static const int CP_COUNT = 6;
static const char CP_FOLDER[] = "/Autosave";

// The game a file in an autosave folder is (the folder's name), or null: "/Autosave/Exolon"
static const char* cp_folder_game(const char* dir) {
    const size_t fl = sizeof(CP_FOLDER) - 1;
    if (strncasecmp(dir, CP_FOLDER, fl) != 0 || dir[fl] != '/' || dir[fl + 1] == '\0') return nullptr;
    return strchr(dir + fl + 1, '/') == nullptr ? dir + fl + 1 : nullptr;
}

// Whether a name (without the extension) is an autosave's: "Autosave 3"
static bool cp_is_name(const char* name) {
    return strncasecmp(name, "Autosave ", 9) == 0 && name[9] >= '1' && name[9] <= '0' + CP_COUNT && name[10] == '\0';
}

// The name a game's grabs and slots go by: a file in a slot folder ("/Save Games/Exolon/Slot 2")
// or a checkpoint folder ("/Checkpoints/Exolon/Checkpoint 3") is the folder's game, anything
// else its own name without " Save Game" (screengrab::base_name)
static size_t game_base(const char* dir, const char* name, char* out) {
    const char* folder = slots::folder_game(dir);
    if (folder != nullptr && slots::parse(name, nullptr, 0) != 0) {
        return screengrab::base_name(folder, out);
    }
    folder = cp_folder_game(dir);
    if (folder != nullptr && cp_is_name(name)) {
        return screengrab::base_name(folder, out);
    }
    return screengrab::base_name(name, out);
}

// The game being played, for its slots: 0 if none is known
static size_t slot_game_now(char* out) {
    out[0] = '\0';
    if (!last_game_known || !game_running) return 0;
    return game_base(last_game_dir, last_game_name, out);
}

struct SlotEntry {
    bool used;
    char label[slots::LABEL_MAX + 1];
    char ext[4];
};

// The SD calls here use the static grab_dir and no FileHandle: core 0's stack is small, and
// a save's snapshot is in snap_load_buffer. The slot used last is the name of an empty folder,
// ".last3" (a dot name: never listed), so it is changed with a rename, not a file.
static DirHandle grab_dir;          // (also the screen grabs' folder being looked through)

// Reads a game's slot folder: e[0..8] for slots 1 to 9. Returns the slot used last (0 none).
__attribute__((noinline)) static int slots_scan(const char* game, SlotEntry* e) {
    for (int i = 0; i < slots::COUNT; i++) e[i] = SlotEntry();
    if (!sd_card_mounted) return 0;
    char path[sizeof(sd_cwd) + 8];
    snprintf(path, sizeof(path), "/sd%s/%s", slots::FOLDER, game);
    int last = 0;
    DirHandle& d = grab_dir;
    if (d.open(path)) {
        while (!d.atEnd()) {
            const char* fn = d.fileName();
            if (d.isDirectory()) {
                if (strncasecmp(fn, slots::LAST_FILE, sizeof(slots::LAST_FILE) - 1) == 0) {
                    int n = slots::parse_last(fn + sizeof(slots::LAST_FILE) - 1, strlen(fn + sizeof(slots::LAST_FILE) - 1));
                    if (n > 0 && fn[sizeof(slots::LAST_FILE)] == '\0') last = n;
                }
            } else {
                char name[80];
                snprintf(name, sizeof(name), "%s", fn);
                char* dot = strrchr(name, '.');
                if (dot != nullptr && (strcasecmp(dot + 1, "sna") == 0 || strcasecmp(dot + 1, "z80") == 0 ||
                        strcasecmp(dot + 1, "szx") == 0)) {
                    *dot = '\0';
                    char label[slots::LABEL_MAX + 1];
                    int n = slots::parse(name, label, sizeof(label));
                    if (n > 0 && !e[n - 1].used) {
                        e[n - 1].used = true;
                        snprintf(e[n - 1].label, sizeof(e[n - 1].label), "%s", label);
                        snprintf(e[n - 1].ext, sizeof(e[n - 1].ext), "%s", dot + 1);
                    }
                }
            }
            if (!d.next()) break;
        }
        d.close();
    }
    return last;
}

// Marks slot n as the one used last: renames the ".lastN" folder (or makes one)
static void slots_write_last(const char* game, int n, int was) {
    if (n == was) return;
    char to[sizeof(sd_cwd) + 16];
    snprintf(to, sizeof(to), "%s/%s/%s%d", slots::FOLDER, game, slots::LAST_FILE, n);
    uint8_t r;
    if (was > 0) {
        snprintf(erase_scratch, sizeof(erase_scratch), "%s/%s/%s%d", slots::FOLDER, game, slots::LAST_FILE, was);
        r = fsops::rename_path(Drive::EXTERNAL_SD_CARD, erase_scratch, to);
    } else {
        walk_stop();
        ff_pool = reinterpret_cast<unsigned char*>(list_pool);
        ff_pool_size = sizeof(list_pool);
        r = fsops::make_dir(Drive::EXTERNAL_SD_CARD, to);
        ff_pool = nullptr;
        ff_pool_size = 0;
    }
    LOG("Slots: %s is the last (%d)\n", to, r);
}

// Makes /Save Games and /Save Games/<game>: true if the folder is there
static bool slots_folder_make(const char* game, char* path, size_t size) {
    snprintf(path, size, "%s/%s", slots::FOLDER, game);
    if (fsops::kind(Drive::EXTERNAL_SD_CARD, path) == fsops::KIND_DIR) return true;
    walk_stop();
    ff_pool = reinterpret_cast<unsigned char*>(list_pool);
    ff_pool_size = sizeof(list_pool);
    if (fsops::kind(Drive::EXTERNAL_SD_CARD, slots::FOLDER) != fsops::KIND_DIR) {
        fsops::make_dir(Drive::EXTERNAL_SD_CARD, slots::FOLDER);
    }
    uint8_t made = fsops::make_dir(Drive::EXTERNAL_SD_CARD, path);
    ff_pool = nullptr;
    ff_pool_size = 0;
    if (made != FILEOP_OK) set_last_error("Cannot make", path);
    return made == FILEOP_OK;
}

// What ACTION_SNA_SAVE's slot save (param3 = 3, or Q on a game with a slot used) saves to
static uint8_t slot_target = 0;
static char slot_target_label[slots::LABEL_MAX + 1];

// After slot n of game was saved as name.ext: any other file of that slot (another label or
// type) is removed, and n is the slot used last
static void slots_after_save(const char* game, int n, const char* name, const char* ext) {
    char path[sizeof(sd_cwd) + 8];
    snprintf(path, sizeof(path), "/sd%s/%s", slots::FOLDER, game);
    DirHandle& d = grab_dir;
    for (int round = 0; round < slots::COUNT && d.open(path); round++) {
        bool removed = false;
        while (!d.atEnd()) {
            char f[80];
            snprintf(f, sizeof(f), "%s", d.fileName());
            char* dot = strrchr(f, '.');
            if (!d.isDirectory() && dot != nullptr) {
                *dot = '\0';
                if (slots::parse(f, nullptr, 0) == n && !(strcmp(f, name) == 0 && strcasecmp(dot + 1, ext) == 0)) {
                    *dot = '.';
                    d.close();
                    snprintf(erase_scratch, sizeof(erase_scratch), "%s/%s/%s", slots::FOLDER, game, f);
                    fsops::remove_path(Drive::EXTERNAL_SD_CARD, erase_scratch);
                    tape_note_follow(1, erase_scratch, 1, nullptr, false);
                    removed = true;
                    break;
                }
            }
            if (!d.next()) break;
        }
        if (!removed) {
            d.close();
            break;
        }
    }
    SlotEntry e[slots::COUNT];
    slots_write_last(game, n, slots_scan(game, e));
}

// A snapshot named name (tidied, no extension) in dir is there already as a .sna, .z80 or .szx:
// a save of that name asks "Overwrite?" whatever the type it is saved as (Commando.z80 there,
// saving Commando as a .sna). except: the extension not to count (nullptr: count them all).
static const char* const SNAP_SAVE_EXTS[] = { "sna", "z80", "szx" };
static bool snap_name_taken(uint8_t drive, const char* dir, const char* name, const char* except) {
    for (const char* ext : SNAP_SAVE_EXTS) {
        if (except != nullptr && strcasecmp(ext, except) == 0) continue;
        int n = snprintf(grab_path, sizeof(grab_path), "%s/%s.%s", dir, name, ext);
        if (n > 0 && n < (int) sizeof(grab_path) && fsops::kind(fsops::drive_of(drive), grab_path) == fsops::KIND_FILE) {
            return true;
        }
    }
    return false;
}

// Overwriting (the user said yes): the snapshots of that name with the other extensions go,
// with their tape notes, so the new one takes their place
static void snap_name_clear_others(uint8_t drive, const char* dir, const char* name, const char* keep) {
    walk_stop();
    for (const char* ext : SNAP_SAVE_EXTS) {
        if (strcasecmp(ext, keep) == 0) continue;
        int n = snprintf(grab_path, sizeof(grab_path), "%s/%s.%s", dir, name, ext);
        if (n <= 0 || n >= (int) sizeof(grab_path)) continue;
        const Drive d = fsops::drive_of(drive);
        if (fsops::kind(d, grab_path) == fsops::KIND_FILE && fsops::remove_path(d, grab_path)) {
            LOG("Save: %s replaced\n", grab_path);
            tape_note_follow(drive, grab_path, drive, nullptr, false);
        }
    }
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
//               3 for a save slot: slot_target of the game being played (the name is not used)
// BYTE param4 = the file type: ACTION_SNA_SAVE_TYPE_AUTO, _SNA, _Z80 or _SZX
// status is 0 for success, 1 if the file exists, 2 for any other error
// A quick save (Q) of a game with a slot used goes in the slot used last, over what is there.
// The file a snapshot is saved to on the SD card (a save, or a checkpoint: never both at
// once). Static: it is too big for core 0's stack, and one is enough.
static FIL save_fil;

void nmi_action_sna_save() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t nameoffset =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
    uint8_t overwrite = nmi_rom_data[4];
    bool over_last = nmi_rom_data[5] != 0;
    bool quick = nmi_rom_data[5] == 2;
    bool slot_save = nmi_rom_data[5] == 3;
    uint8_t type = nmi_rom_data[6];
#if PICOMENU_HISCORE
    if (nmi_rom_data[5] == 4 && hs_copied()) {
        nmi_rom_data[0] = FILEOP_OK;    // (not an autosave: the game's memory, for its high scores)
        return;
    }
#endif
    if (nmi_rom_data[5] == 4) {
        // a checkpoint: written to /Checkpoints/<game> as soon as the game carries on (checkpoint_flush)
        cp_pause_us = (uint32_t) (time_us_64() - cp_nmi_at);
        cp_pause_128 = save_is_128k;
        if (cp_pause_us > cp_pause_max_us) cp_pause_max_us = cp_pause_us;
        checkpoint_took();
        nmi_rom_data[0] = FILEOP_OK;
        return;
    }
    const char* dir;
    uint8_t drive;
    char slot_game[screengrab::NAME_MAX + 1] = "";
    if ((quick || slot_save) && sd_card_mounted && slot_game_now(slot_game) > 0) {
        if (quick) {
            SlotEntry e[slots::COUNT];
            int last = slots_scan(slot_game, e);
            if (last > 0) {
                slot_save = true;
                slot_target = (uint8_t) last;
                snprintf(slot_target_label, sizeof(slot_target_label), "%s", e[last - 1].label);
            }
        }
    } else if (slot_save) {
        nmi_rom_data[0] = FILEOP_FAILED;
        return;
    }
    if (slot_save) {
        quick = false;
        over_last = false;
        if (slot_target < 1 || slot_target > slots::COUNT || !slots_folder_make(slot_game, erase_path, sizeof(erase_path))) {
            nmi_rom_data[0] = FILEOP_FAILED;
            return;
        }
        drive = 1;
        dir = erase_path;
        overwrite = 1;
        type = cfg.save_type;
    }
    else if (quick) {
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
        extension, slot_save ? "(slot)" : (const char*) nmi_rom_data + nameoffset, overwrite, drive, dir);
    char name[64];
    if (slot_save) {
        slots::name(slot_target, slot_target_label, name, sizeof(name));
    } else {
        snprintf(name, sizeof(name), "%s", reinterpret_cast<const char*>(nmi_rom_data) + nameoffset);
    }

    // Picomenu.ini's quick_save: a quick save over one already there overwrites it, or is the
    // next number, with no question
    if (quick && overwrite == 0 && cfg.quick_save != settings::QUICK_ASK) {
        char probe[sizeof(name)];
        snprintf(probe, sizeof(probe), "%s", name);
        if (tidy_new_name(probe) && snap_name_taken(drive, dir, probe, nullptr)) {
            overwrite = cfg.quick_save == settings::QUICK_OVERWRITE ? 1 : 2;
        }
    }
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
            if (!snap_name_taken(drive, dir, probe, nullptr)) {
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

    // a snapshot of this name with another extension counts as the name being in use: asked
    // first ("Overwrite?"), and replaced when the answer is yes (not for a slot: its own files)
    if (!slot_save) {
        char probe[sizeof(name)];
        snprintf(probe, sizeof(probe), "%s", name);
        if (tidy_new_name(probe)) {
            if (overwrite == 0 && snap_name_taken(drive, dir, probe, extension)) {
                LOG("Save: %s is there with another extension\n", probe);
                nmi_rom_data[0] = FILEOP_NAME_IN_USE;
                return;
            }
            if (overwrite != 0) snap_name_clear_others(drive, dir, probe, extension);
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
            tape_note_after_save(0, filename);
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
        FIL& file = save_fil;
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
        if (fr == FR_OK && slot_save) {
            slots_after_save(slot_game, slot_target, name, extension);
        }
        if (fr == FR_OK) {
            tape_note_after_save(1, path);
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



static const char ROMS_FOLDER[] = "/ROMS";     // on the SD card: ROM files (Set up SD card makes it, hidden)
static FileHandle* notes_handle();

// C - Change ROM lists the .rom files of one place: the root of the internal flash, or the SD
// card's ROMS folder. The list starts with Internal Rom and an entry that shows the other place
// (choosing it lists that place: the ROM reads the list again when a choice does not leave).
static uint8_t rom_source = 0;              // 0 the flash, 1 the SD card's ROMS folder
static const char ROM_SHOW_SD[] = "Show SD card /ROMS";
static const char ROM_SHOW_FLASH[] = "Show internal flash";

void nmi_action_list_rom() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t start =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
    uint16_t destoffset =  (nmi_rom_data[5] << 8) |  nmi_rom_data[4];
    uint16_t stringoffset = destoffset + 22;
    if (rom_source == 1 && !sd_card_mounted) rom_source = 0;

    LOG("List roms (%s) starting at %d and storing at %X\n", rom_source ? "SD /ROMS" : "flash", start, destoffset);

    nmi_rom_data[destoffset] = 0;
    nmi_rom_data[destoffset + 1] = 0;
    int index = 0;
    bool room = true;
    auto out = [&](const char* nameAndExt) {
        if (room && index >= start) room = add_file_to_list(nameAndExt, destoffset, stringoffset);
        index++;
    };
    char entry[40];
    out("Internal Rom\0int");
    if (rom_source == 1 || sd_card_mounted) {
        snprintf(entry, sizeof(entry), "%s", rom_source == 0 ? ROM_SHOW_SD : ROM_SHOW_FLASH);
        entry[strlen(entry) + 1] = '\0';
        memcpy(entry + strlen(entry) + 1, "src", 4);
        out(entry);
    }
    // the .rom files, gathered in list_pool ("name", "rom") and put in order
    walk_stop();
    uint16_t* offs = reinterpret_cast<uint16_t*>(list_pool);
    const int MAX = 300;
    int count = 0;
    size_t used = MAX * sizeof(uint16_t);
    auto keep = [&](const char* n) {
        size_t len = strlen(n);
        if (len <= 4 || len >= 64 || n[0] == '.' || strcasecmp(n + len - 4, ".rom") != 0) return;
        if (count >= MAX || used + len + 1 > LIST_POOL_SIZE) return;
        memcpy(list_pool + used, n, len + 1);
        list_pool[used + len - 4] = '\0';         // "name\0rom"
        offs[count++] = (uint16_t) used;
        used += len + 1;
    };
    if (rom_source == 0) {
        int dir = pico_dir_open("/");
        struct lfs_info info;
        while (dir >= 0 && pico_dir_read(dir, &info) > 0) {
            if (info.type == LFS_TYPE_REG) keep(info.name);
        }
        if (dir >= 0) pico_dir_close(dir);
    } else {
        DirHandle& d = find_dirs[0];
        if (d.open("/sd/ROMS")) {
            while (!d.atEnd()) {
                if (!d.isDirectory() && !d.isSystem()) keep(d.fileName());
                if (!d.next()) break;
            }
            d.close();
        }
    }
    sort_u16(offs, (size_t) count, [](const void* x, const void* y) {
        return strcasecmp(list_pool + *(const uint16_t*) x, list_pool + *(const uint16_t*) y);
    });
    for (int i = 0; i < count && room; i++) out(list_pool + offs[i]);
    walk_stop();                    // (list_pool was used)
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

    if (strcmp(ext, "src") == 0) {
        // the other place's .rom files: status 2, the ROM lists them
        rom_source = rom_source == 0 && sd_card_mounted ? 1 : 0;
        nmi_rom_data[0] = 2;
        return;
    }
    if (strcmp(filename, "Internal Rom.int") == 0) {
         // disable PICO from supplying its own rom when the nmi routine exits
         // and thereby return to the Spectrum internal ROM
        rom_state.flags_on_nmi_exit = 0;
        rom_state.writableStartAddress = 32768;
        nmi_rom_data[0] = 0;
    }
    else if (rom_source == 1) {
        // the SD card's ROMS folder
        char path[160];
        snprintf(path, sizeof(path), "%s/%s", ROMS_FOLDER, filename);
        FileHandle& f = *notes_handle();
        if (!f.open(Drive::EXTERNAL_SD_CARD, path, true)) {
            nmi_rom_data[0] = 1;
            return;
        }
        f.read(rom_data, 16384);
        f.close();
        rom_state.flags_on_nmi_exit = 1;
        rom_state.writableStartAddress = makeWritable ? 64 : 32768;
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

// Lock the paging port for a 48K program, as "48 BASIC" does (lock_48k in Picomenu.ini):
// tapes (the default) when a tape is being played by the trap (a tape loaded with its number,
// or a 48K snapshot that carries on with its tape), always, or off. A 48K/128K game left
// unlocked finds the 128's memory and acts as the 128K version: Out Run, converted as 48K,
// then took its 48K stage for a 128K one and crashed. A locked port ignores all writes until
// the Spectrum is really reset, so a long press of the button then gets to 48 BASIC, not the
// 128's start up menu (the Spectrum's own reset button does).
static bool lock_48k_now() {
    return cfg.lock_48k == 2 || (cfg.lock_48k == 1 && tape_trap_running());
}
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
#if PICOMENU_HISCORE
    hs_commit();                    // the game going in now: its high score table put in it
#endif

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
            lock_48k_now() ? PORT_7FFD_48K_LOCKED : PORT_7FFD_48K;
        memcpy(nmi_rom_data + headeroffset, current_snap_data, Z80_HEADER_SIZE);
        current_snap_offset = Z80_HEADER_SIZE;
    }
    else {
        LOG("Sending SNA file header\n");
        nmi_rom_data[2] = 0;
        nmi_rom_data[3] = reset_to_128k_pending ? PORT_7FFD_128K_ROM0 :
            lock_48k_now() ? PORT_7FFD_48K_LOCKED : PORT_7FFD_48K;
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

// The diagnostic ROMs, kept in the top folder of the internal flash (or the SD card's ROMS folder)
// so that Browse Games can
// copy them there: Brendan Alford's zx-diagnostics testrom.bin as testrom.rom (which = 0)
// and Phil Ruston's DiagROM as DiagROM.rom (which = 1).  The .bin name, and for DiagROM a
// name with no extension, are found too, in any case.  The first 16384 bytes are put in place
// of the Spectrum's ROM.
// Returns 0, 1 if there is no such file, 2 if it is too short.
static FileHandle* notes_handle();
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
    bool whole = false;
    if (found[0] != '\0') {
        int file = pico_open(found, LFS_O_RDONLY);
        if (file < 0) {
            return 1;
        }
        whole = pico_size(file) >= 16384 && pico_read(file, rom_data, 16384) == 16384;   // size first: rom_data is in use
        pico_close(file);
    } else {
        // not on the flash: the SD card's ROMS folder (its names in any case, as the card has them)
        FileHandle& f = *notes_handle();
        for (int n = 0; n < 3 && found[0] == '\0'; n++) {
            if (wanted[n] == nullptr || !sd_card_mounted) continue;
            char path[48];
            snprintf(path, sizeof(path), "/ROMS/%s", wanted[n]);
            if (f.open(Drive::EXTERNAL_SD_CARD, path, true)) {
                snprintf(found, sizeof(found), "%s", path);
                whole = f.size() >= 16384 && f.read(rom_data, 16384) == 16384;
                f.close();
            }
        }
        if (found[0] == '\0') {
            return 1;
        }
    }
    if (!whole) {
        if (tape_in) {
            tape_in_hook_to_rom();      // a tape put in with T is still in: its hook was read over
        }
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
    prepared_is_game = false;
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
        checkpoints_stop();
        if (!tape_in) {
            tape_service_stop();        // a tape put in with T stays in (the Tape Loader, LOAD "")
        }
        snap_is_paged = false;
        tap_make_reset_snapshot();
        game_running = tape_in;         // its game is still the one Q saves
        menu_reset_128k = what == PREPARE_LOAD_RESET_128K;
        reset_to_128k_pending = menu_reset_128k;
        snap_prepared = true;
        prepared_is_game = false;
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
    if (n < max) n += snprintf(out + n, max - n, "\n\x01" "C converts this tape to a .z80\n");
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
    if (!is_snapshot_ext(ext)) {
        nmi_rom_data[0] = 3;        // V is for games only (.tap .tzx .sna .z80 .szx): the ROM says so
        return;
    }
    static char filename[BROWSE_PATH_MAX];
    snprintf(filename, sizeof(filename), "%s/%s.%s", list_entry_dir_for(nameoffset), fileAndExt, ext);
    nmi_rom_data[0] = find_preview(drive, filename, ext);
    snprintf(info_name, sizeof(info_name), "%s", fileAndExt);
    // a screen of nothing but zeros is no picture: only the screen grabs are cycled then
    info_source = (preview_screen != nullptr && !gameinfo::screen_is_blank(preview_screen)) ? preview_screen : nullptr;
}

static void help_put_text(uint8_t* screen, int row, int col, const char* text, int len, uint8_t attr);

// ---- S on the menu: the save slots screen (ACTION_SLOTS), and the question when a game with
// slots is loaded (ACTION_SLOT_ASK). Both are drawn by the pico and take the keys (picoscreen).
enum SlotState : uint8_t { SLOT_LIST, SLOT_OVERWRITE, SLOT_PICK_LABEL, SLOT_TYPING, SLOT_PICK_VIEW, SLOT_VIEWING,
    SLOT_PICK_ERASE, SLOT_ERASE };
static SlotState slot_state = SLOT_LIST;
static uint8_t slot_pick = 0;
static char slot_typed[slots::LABEL_MAX + 1];
static const char* slot_msg = nullptr;

static bool is_key_break(uint8_t key) { return key == notes::KEY_BREAK; }
// The keys to leave a screen: BREAK always; X too where nothing is being typed. Their names on
// the screens: "BREAK-Back" where X is typed, else "X-Back" (back a step) or "X-Exit" (out of the
// menu, back to the game: the menu's own X, and S or G opened alone by the button)
static bool is_key_back(uint8_t key) { return key == notes::KEY_BREAK || key == 'x' || key == 'X'; }
static bool menu_alone() {
    const uint8_t c = (rom_data + 16384)[STARTUP_COMMAND_OFFSET];
    return c == STARTUP_ACTION_SAVE_SCREEN || c == STARTUP_ACTION_GRAB_SCREEN;
}

// The slots, one on each of rows 2 to 10. only_used: the empty ones are left out (the question).
static void slots_draw_list(uint8_t* screen, const SlotEntry* e, int last, bool only_used) {
    int row = 2;
    for (int i = 0; i < slots::COUNT; i++) {
        if (only_used && !e[i].used) continue;
        char t[40];
        snprintf(t, sizeof(t), "%d", i + 1);
        help_put_text(screen, row, 1, t, 1, gameinfo::ATTR_HEADING);
        if (!e[i].used) {
            help_put_text(screen, row, 3, "- empty -", 9, gameinfo::ATTR_DIM);
        } else {
            const char* label = e[i].label[0] != '\0' ? e[i].label : "Saved game";
            help_put_text(screen, row, 3, label, (int) strlen(label), e[i].label[0] != '\0' ? gameinfo::ATTR_TEXT : gameinfo::ATTR_DIM);
            snprintf(t, sizeof(t), ".%s", e[i].ext);
            help_put_text(screen, row, 21, t, (int) strlen(t), gameinfo::ATTR_DIM);
            if (i + 1 == last) help_put_text(screen, row, 27, "last", 4, gameinfo::ATTR_HEADING);
        }
        row++;
    }
}

static void slots_banner(uint8_t* screen, const char* lead, const char* game) {
    memset(screen, 0, 6144);
    memset(screen + 6144, ui_attr(0x07), 768);
    memset(screen + 6144, gameinfo::ATTR_BANNER, 32);
    char t[40];
    snprintf(t, sizeof(t), "%s%s", lead, game);
    help_put_text(screen, 0, 1, t, (int) strlen(t), gameinfo::ATTR_BANNER);
}

// ACTION_SLOTS: BYTE param1 = the key (0: opened). Status 0 the screen stays; 1 closed (back to
// the menu); 2 the Save snapshot screen instead; 3 save now, to slot_target (the ROM sends the
// snapshot, then ACTION_SNA_SAVE with param3 = 3). With save_screen = named in Picomenu.ini,
// no SD card, or no game known, it is 2 straight away.
__attribute__((noinline)) static void nmi_action_slots() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t key = nmi_rom_data[2];
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer);
    char game[screengrab::NAME_MAX + 1];
    if (key == 0) {
        slot_state = SLOT_LIST;
        slot_msg = nullptr;
        if (!cfg.save_screen_slots || !sd_card_mounted || slot_game_now(game) == 0) {
            nmi_rom_data[0] = 2;
            return;
        }
    }
    if (slot_game_now(game) == 0) {
        nmi_rom_data[0] = 1;
        return;
    }
    SlotEntry e[slots::COUNT];
    int last = slots_scan(game, e);
    const int n = key >= '1' && key <= '9' ? key - '0' : 0;
    const bool back = is_key_break(key) || key == 'x' || key == 'X';
    if (key != 0) slot_msg = nullptr;
    switch (key == 0 ? SLOT_LIST : slot_state) {
    case SLOT_LIST:
        if (key == 0) break;
        if (n > 0 && e[n - 1].used) {
            slot_pick = (uint8_t) n;
            slot_state = SLOT_OVERWRITE;
        } else if (n > 0) {
            slot_target = (uint8_t) n;
            slot_target_label[0] = '\0';
            nmi_rom_data[0] = 3;
            return;
        } else if (key == 'l' || key == 'L') {
            slot_state = SLOT_PICK_LABEL;
        } else if (key == 'v' || key == 'V') {
            slot_state = SLOT_PICK_VIEW;
        } else if (key == 'e' || key == 'E') {
            slot_state = SLOT_PICK_ERASE;
        } else if (key == 's' || key == 'S') {
            nmi_rom_data[0] = 2;
            return;
        } else if (back) {
            nmi_rom_data[0] = 1;
            return;
        }
        break;
    case SLOT_OVERWRITE:
        if (key == 'y' || key == 'Y') {
            slot_target = slot_pick;
            snprintf(slot_target_label, sizeof(slot_target_label), "%s", e[slot_pick - 1].label);
            nmi_rom_data[0] = 3;
            return;
        }
        slot_state = SLOT_LIST;
        break;
    case SLOT_PICK_LABEL:
    case SLOT_PICK_VIEW:
    case SLOT_PICK_ERASE:
        if (back) {
            slot_state = SLOT_LIST;
        } else if (n > 0) {
            slot_pick = (uint8_t) n;
            if (slot_state == SLOT_PICK_LABEL) {
                snprintf(slot_typed, sizeof(slot_typed), "%s", e[n - 1].label);
                slot_state = SLOT_TYPING;
            } else if (!e[n - 1].used) {
                slot_msg = "That slot is empty";
                slot_state = SLOT_LIST;
            } else if (slot_state == SLOT_PICK_ERASE) {
                slot_state = SLOT_ERASE;
            } else {
                char file[80];
                slots::name(n, e[n - 1].label, file, sizeof(file));
                snprintf(erase_path, sizeof(erase_path), "%s/%s/%s.%s", slots::FOLDER, game, file, e[n - 1].ext);
                preview_screen = nullptr;
                if (find_preview(1, erase_path, e[n - 1].ext) == 0 && preview_screen != nullptr) {
                    slot_state = SLOT_VIEWING;
                    nmi_rom_data[0] = 0;        // the slot's own screen until a key is pressed
                    return;
                }
                slot_msg = "No picture in that slot";
                slot_state = SLOT_LIST;
            }
        }
        break;
    case SLOT_TYPING: {
        size_t len = strlen(slot_typed);
        if (is_key_break(key)) {
            slot_state = SLOT_LIST;
        } else if (key == notes::KEY_DELETE) {
            if (len > 0) slot_typed[len - 1] = '\0';
        } else if (key == notes::KEY_ENTER || key == 13) {
            slots::tidy_label(slot_typed);
            slot_state = SLOT_LIST;
            if (!e[slot_pick - 1].used) {
                slot_target = slot_pick;            // an empty slot: saved now, with its label
                snprintf(slot_target_label, sizeof(slot_target_label), "%s", slot_typed);
                nmi_rom_data[0] = 3;
                return;
            }
            char from[80], to[80];
            slots::name(slot_pick, e[slot_pick - 1].label, from, sizeof(from));
            slots::name(slot_pick, slot_typed, to, sizeof(to));
            if (strcmp(from, to) != 0) {
                snprintf(erase_path, sizeof(erase_path), "%s/%s/%s.%s", slots::FOLDER, game, from, e[slot_pick - 1].ext);
                snprintf(erase_scratch, sizeof(erase_scratch), "%s/%s/%s.%s", slots::FOLDER, game, to, e[slot_pick - 1].ext);
                walk_stop();
                uint8_t r = fsops::rename_path(Drive::EXTERNAL_SD_CARD, erase_path, erase_scratch);
                if (r == FILEOP_OK) {
                    tape_note_follow(1, erase_path, 1, erase_scratch, false);
                    slot_msg = "Label saved";
                } else {
                    slot_msg = "Could not change the label";
                }
                last = slots_scan(game, e);
            }
        } else if (slots::label_char(key) && len < slots::LABEL_MAX) {
            slot_typed[len] = (char) key;
            slot_typed[len + 1] = '\0';
        }
        break;
    }
    case SLOT_VIEWING:
        slot_state = SLOT_LIST;
        break;
    case SLOT_ERASE:
        if (key == 'y' || key == 'Y') {
            char file[80];
            slots::name(slot_pick, e[slot_pick - 1].label, file, sizeof(file));
            snprintf(erase_path, sizeof(erase_path), "%s/%s/%s.%s", slots::FOLDER, game, file, e[slot_pick - 1].ext);
            walk_stop();
            BinResult bin = to_waste_bin(1, erase_path, false, erase_scratch, sizeof(erase_scratch));
            bool ok = bin == BIN_OK || (bin == BIN_NONE && fsops::remove_path(Drive::EXTERNAL_SD_CARD, erase_path));
            if (ok) tape_note_follow(1, erase_path, 1, bin == BIN_OK ? erase_scratch : nullptr, false);
            slot_msg = !ok ? "Could not erase it" : bin == BIN_OK ? "Moved to the Waste Bin" : "Erased";
            last = slots_scan(game, e);
        }
        slot_state = SLOT_LIST;
        break;
    }

    slots_banner(screen, "Save slots: ", game);
    slots_draw_list(screen, e, last, false);
    help_put_text(screen, 13, 1, "1-9", 3, gameinfo::ATTR_KEYS);
    help_put_text(screen, 13, 5, "Save to a slot", 14, gameinfo::ATTR_TEXT);
    help_put_text(screen, 14, 1, "L-Label  V-View  E-Erase", 24, gameinfo::ATTR_KEYS);
    help_put_text(screen, 15, 1, "S-Save snapshot (own name)", 26, gameinfo::ATTR_KEYS);
    help_put_text(screen, 16, 1, menu_alone() ? "X-Exit" : "X-Back", 6, gameinfo::ATTR_KEYS);
    char t[48];
    switch (slot_state) {
    case SLOT_OVERWRITE:
        snprintf(t, sizeof(t), "Save over slot %d?", slot_pick);
        help_put_text(screen, 19, 1, t, (int) strlen(t), gameinfo::ATTR_HEADING);
        help_put_text(screen, 20, 1, "Y-Yes  N-No", 11, gameinfo::ATTR_KEYS);
        break;
    case SLOT_PICK_LABEL:
    case SLOT_PICK_VIEW:
    case SLOT_PICK_ERASE: {
        const char* what = slot_state == SLOT_PICK_LABEL ? "Label which slot? 1-9" :
            slot_state == SLOT_PICK_VIEW ? "View which slot? 1-9" : "Erase which slot? 1-9";
        help_put_text(screen, 19, 1, what, (int) strlen(what), gameinfo::ATTR_HEADING);
        help_put_text(screen, 20, 1, "X-Back", 6, gameinfo::ATTR_KEYS);
        break;
    }
    case SLOT_TYPING:
        snprintf(t, sizeof(t), "Label for slot %d:", slot_pick);
        help_put_text(screen, 18, 1, t, (int) strlen(t), gameinfo::ATTR_HEADING);
        snprintf(t, sizeof(t), "%s_", slot_typed);
        help_put_text(screen, 19, 1, t, (int) strlen(t), gameinfo::ATTR_TEXT);
        help_put_text(screen, 20, 1, "ENTER-Done  BREAK-Back", 22, gameinfo::ATTR_KEYS);
        break;
    case SLOT_ERASE:
        snprintf(t, sizeof(t), "Erase slot %d?", slot_pick);
        help_put_text(screen, 19, 1, t, (int) strlen(t), gameinfo::ATTR_HEADING);
        help_put_text(screen, 20, 1, "Y-Yes  N-No", 11, gameinfo::ATTR_KEYS);
        break;
    default:
        break;
    }
    if (slot_msg != nullptr) help_put_text(screen, 22, 1, slot_msg, (int) strlen(slot_msg), gameinfo::ATTR_TEXT);
    preview_screen = screen;
    nmi_rom_data[0] = 0;
}

// Where the question's game is, kept from when it was opened (the list may change meanwhile)
static uint8_t ask_drive = 0;
// V and E on the question: 0 the list, 1 "View which slot?", 2 a slot's screen showing (any key
// goes back), 3 "Erase which slot?", 4 "Erase slot n?" (slot_pick)
static uint8_t ask_view = 0;

// ACTION_SLOT_ASK: a game chosen in Browse Games has save slots. BYTE param1 = the key (0: the
// game was chosen: WORD param2 = its list entry's name, BYTE param4 = the drive; or BYTE param4
// = 2: R - Reload's game, which asks only when it is not itself a save, in /Save Games). E erases
// a slot (after Y), as on the save slots screen. Status 0 the
// question stays; 1 load the game as chosen (there is no question: no slots, slots_on_load =
// never, the game is itself a slot); 2 the game or a slot is ready in the pico (ACTION_PREPARE_LOAD,
// load with no name); 3 back to the list.
__attribute__((noinline)) static void nmi_action_slot_ask() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t key = nmi_rom_data[2];
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer);
    // erase_path: the game's folder; erase_scratch: its name and extension (two strings)
    char game[screengrab::NAME_MAX + 1];
    if (key == 0) {
        const char* fileAndExt;
        const char* dir;
        if (nmi_rom_data[5] == 2) {
            // R - Reload: a save (in /Save Games, a slot or not, or a checkpoint in /Checkpoints)
            // loads as it is, with no question
            const size_t fl = sizeof(slots::FOLDER) - 1;
            if (!reload_known || (reload_drive == 1 && !sd_card_mounted) ||
                    (strncasecmp(reload_dir, slots::FOLDER, fl) == 0 && (reload_dir[fl] == '\0' || reload_dir[fl] == '/')) ||
                    cp_folder_game(reload_dir) != nullptr) {
                nmi_rom_data[0] = 1;
                return;
            }
            ask_drive = reload_drive;
            fileAndExt = reload_name;
            dir = reload_dir;
        } else {
            uint16_t nameoffset = (nmi_rom_data[4] << 8) | nmi_rom_data[3];
            ask_drive = nmi_rom_data[5] == 1 ? 1 : 0;
            fileAndExt = list_entry_name_for(nameoffset, reinterpret_cast<const char*>(nmi_rom_data + nameoffset));
            dir = list_entry_dir_for(nameoffset);
        }
        size_t nl = strlen(fileAndExt) + 1;
        size_t el = strlen(fileAndExt + nl) + 1;
        if (!cfg.slots_on_load || !sd_card_mounted || slots::is_slot_folder(dir) || nl + el > sizeof(erase_scratch) ||
                strlen(dir) >= sizeof(erase_path) || screengrab::base_name(fileAndExt, game) == 0) {
            nmi_rom_data[0] = 1;
            return;
        }
        snprintf(erase_path, sizeof(erase_path), "%s", dir);
        memcpy(erase_scratch, fileAndExt, nl + el);
        ask_view = 0;
        slot_msg = nullptr;
    } else if (screengrab::base_name(erase_scratch, game) == 0) {
        nmi_rom_data[0] = 3;
        return;
    }
    SlotEntry e[slots::COUNT];
    int last = slots_scan(game, e);
    bool any = false;
    for (int i = 0; i < slots::COUNT; i++) any = any || e[i].used;
    if (!any && key == 0) {
        nmi_rom_data[0] = 1;
        return;
    }
    const int n = key >= '1' && key <= '9' ? key - '0' : 0;
    if (key != 0) slot_msg = nullptr;
    const bool viewing = ask_view != 0;     // this key is for V's question or its picture
    if (ask_view == 2) {
        ask_view = 0;                       // the slot's screen: any key goes back to the list
    } else if (ask_view == 1) {
        // V, then a slot: its own screen, as on the save slots screen (the game's path stays in
        // erase_path and erase_scratch)
        ask_view = 0;
        if (n > 0 && e[n - 1].used) {
            char file[80];
            slots::name(n, e[n - 1].label, file, sizeof(file));
            snprintf(grab_path, sizeof(grab_path), "%s/%s/%s.%s", slots::FOLDER, game, file, e[n - 1].ext);
            preview_screen = nullptr;
            if (find_preview(1, grab_path, e[n - 1].ext) == 0 && preview_screen != nullptr) {
                ask_view = 2;
                nmi_rom_data[0] = 0;
                return;
            }
            slot_msg = "No picture in that slot";
        } else if (n > 0) {
            slot_msg = "That slot is empty";
        }
    } else if (ask_view == 3) {
        // E, then a slot: asks first
        ask_view = 0;
        if (n > 0 && e[n - 1].used) {
            slot_pick = (uint8_t) n;
            ask_view = 4;
        } else if (n > 0) {
            slot_msg = "That slot is empty";
        }
    } else if (ask_view == 4) {
        // Y: the slot goes to the Waste Bin (or is erased, waste_bin = off), its tape note with it
        ask_view = 0;
        if (key == 'y' || key == 'Y') {
            char file[80];
            // where it went in the Waste Bin: the end of snap_load_buffer (the screen is at its start)
            const size_t tsize = BROWSE_PATH_MAX + 8;
            char* to = snap_load_buffer + SNAP_LOAD_BUFFER_SIZE - tsize;
            slots::name(slot_pick, e[slot_pick - 1].label, file, sizeof(file));
            snprintf(grab_path, sizeof(grab_path), "%s/%s/%s.%s", slots::FOLDER, game, file, e[slot_pick - 1].ext);
            walk_stop();
            BinResult bin = to_waste_bin(1, grab_path, false, to, tsize);
            bool ok = bin == BIN_OK || (bin == BIN_NONE && fsops::remove_path(Drive::EXTERNAL_SD_CARD, grab_path));
            if (ok) tape_note_follow(1, grab_path, 1, bin == BIN_OK ? to : nullptr, false);
            slot_msg = !ok ? "Could not erase it" : bin == BIN_OK ? "Moved to the Waste Bin" : "Erased";
            last = slots_scan(game, e);
            any = false;
            for (int i = 0; i < slots::COUNT; i++) any = any || e[i].used;
        }
    } else if (key == 'v' || key == 'V') {
        ask_view = 1;
    } else if (key == 'e' || key == 'E') {
        ask_view = 3;
    } else if (key == '0' || key == notes::KEY_ENTER || key == 13) {
        load_snapshot_file(ask_drive, erase_path, erase_scratch);       // the game as chosen
        snap_prepared = true;
        nmi_rom_data[0] = 2;
        return;
    }
    if (!viewing && ask_view == 0 && n > 0 && e[n - 1].used) {
        // the slot's folder and its name and extension (two strings), in place of the game's
        snprintf(erase_path, sizeof(erase_path), "%s/%s", slots::FOLDER, game);
        slots::name(n, e[n - 1].label, erase_scratch, sizeof(erase_scratch));
        size_t fl = strlen(erase_scratch) + 1;
        snprintf(erase_scratch + fl, sizeof(erase_scratch) - fl, "%s", e[n - 1].ext);
        load_snapshot_file(1, erase_path, erase_scratch);
        snap_prepared = true;
        nmi_rom_data[0] = 2;
        return;
    }
    if (!viewing && (is_key_break(key) || key == 'x' || key == 'X')) {
        nmi_rom_data[0] = 3;
        return;
    }
    slots_banner(screen, "Saved games: ", game);
    slots_draw_list(screen, e, last, true);
    if (!any) help_put_text(screen, 2, 1, "No saved games left", 19, gameinfo::ATTR_DIM);
    help_put_text(screen, 13, 1, "1-9", 3, gameinfo::ATTR_KEYS);
    help_put_text(screen, 13, 5, "Carry on from a slot", 20, gameinfo::ATTR_TEXT);
    help_put_text(screen, 14, 1, "0", 1, gameinfo::ATTR_KEYS);
    help_put_text(screen, 14, 5, "Start the game", 14, gameinfo::ATTR_TEXT);
    help_put_text(screen, 15, 1, "V", 1, gameinfo::ATTR_KEYS);
    help_put_text(screen, 15, 5, "View a slot's screen", 20, gameinfo::ATTR_TEXT);
    help_put_text(screen, 16, 1, "E", 1, gameinfo::ATTR_KEYS);
    help_put_text(screen, 16, 5, "Erase a slot", 12, gameinfo::ATTR_TEXT);
    help_put_text(screen, 17, 1, "X-Back", 6, gameinfo::ATTR_KEYS);
    if (ask_view == 1 || ask_view == 3) {
        const char* what = ask_view == 1 ? "View which slot? 1-9" : "Erase which slot? 1-9";
        help_put_text(screen, 19, 1, what, (int) strlen(what), gameinfo::ATTR_HEADING);
        help_put_text(screen, 20, 1, "X-Back", 6, gameinfo::ATTR_KEYS);
    } else if (ask_view == 4) {
        char t[24];
        snprintf(t, sizeof(t), "Erase slot %d?", slot_pick);
        help_put_text(screen, 19, 1, t, (int) strlen(t), gameinfo::ATTR_HEADING);
        help_put_text(screen, 20, 1, "Y-Yes  N-No", 11, gameinfo::ATTR_KEYS);
    }
    if (slot_msg != nullptr) help_put_text(screen, 22, 1, slot_msg, (int) strlen(slot_msg), gameinfo::ATTR_TEXT);
    preview_screen = screen;
    nmi_rom_data[0] = 0;
}

// ---- Checkpoints (Picomenu.ini's checkpoints): a snapshot of the game every 5, 10 or 20 s ----
// A game loaded from the menu asks first ("Checkpoints for this game?", ACTION_CHECKPOINT_ASK).
// The checkpoints are snapshots taken with no menu (STARTUP_ACTION_CHECKPOINT): the NMI ROM sends
// one as for a save, and it is written to /Checkpoints/<game> on the SD card as soon as the game
// carries on (checkpoint_flush), so the game only waits for the copy. The last CP_COUNT are kept,
// the oldest written over. K on the menu (ACTION_CHECKPOINTS) lists them by age and loads one,
// moves one to a save slot, or clears the game's checkpoint files.
// All of it in a few bytes: the RAM left for the heap is tight
static struct {
    uint32_t due_ms;            // when the next checkpoint is due
    uint32_t menu_ms;           // when the menu opened: the ages are counted to then
    uint16_t at_t[CP_COUNT];    // when each was taken, in eighths of a second of game time (the
    uint16_t pending_t;         // menu left out; 16 bits go round in 2 hours, far more than 6 take)
    uint8_t used;               // a bit for each checkpoint there
    uint8_t big;                // a bit for each that is a 128K one (.z80; else .sna)
    uint8_t next : 3;           // the entry the next checkpoint goes in
    uint8_t pending_at : 3;
    bool on : 1;                // this game's answer to the question
    bool pending : 1;           // a checkpoint in snap_load_buffer, still to be written
    bool loading : 1;           // the load being made is a checkpoint: no question
    bool paused : 1;            // a menu (or a screen with no menu) is open
    uint8_t secs;               // seconds between them for this game (Picomenu.ini's, or 20 when
                                // turned on from the A screen with it off)
} rw;
// The K screen: 0 the list, 1 "View which?", 2 a screen showing, 3 "Move which?", 4 "To which
// slot?", 5 "Save over slot n?", 6 "Clear ...?"; cp_pick the checkpoint picked (its entry)
static uint8_t cp_state = 0, cp_pick = 0;
static bool rw_used(int i) { return (rw.used >> i) & 1; }
static const char* rw_ext(int i) { return (rw.big >> i) & 1 ? "z80" : "sna"; }

static uint32_t rw_now() { return to_ms_since_boot(get_absolute_time()); }
#ifdef PICOFACE_HOST
static bool rw_host_take = false;
#endif
static bool conv_running();             // (a tape being converted: the Spectrum is the converter's)
static uint16_t rw_ticks(uint32_t ms) { return (uint16_t) (ms / 125); }

// The game's checkpoint folder, "/Checkpoints/<game>" (the game as its save slots know it), or
// false if no game is known
static bool cp_folder(char* out, size_t size) {
    char game[screengrab::NAME_MAX + 1];
    if (slot_game_now(game) == 0) return false;
    return snprintf(out, size, "%s/%s", CP_FOLDER, game) < (int) size;
}

static void checkpoints_clear() {
    rw.used = 0;
    rw.next = 0;
    rw.pending = false;
}

// Checkpoints stop (a reset, another game): the ones taken are forgotten (their files stay)
static void checkpoints_stop() {
    rw.on = false;
    checkpoints_clear();
}

// The checkpoints newest first: order[0] is the newest. Returns how many there are.
static int rw_order(int* order) {
    int n = 0;
    for (int k = 1; k <= CP_COUNT; k++) {
        int i = (rw.next - k + CP_COUNT) % CP_COUNT;
        if (rw_used(i)) order[n++] = i;
    }
    return n;
}

// Writes the checkpoint the ROM sent (still in snap_load_buffer) to /Checkpoints/<game>
static void checkpoint_flush() {
    if (!rw.pending) return;
    rw.pending = false;
    const int at = rw.pending_at;
    rw.used &= (uint8_t) ~(1 << at);
    char* path = grab_path;             // (free: no screen grab is being saved)
    const size_t psize = sizeof(grab_path);
    if (!sd_card_mounted || !cp_folder(path, psize)) return;
    walk_stop();
    if (fsops::kind(Drive::EXTERNAL_SD_CARD, path) != fsops::KIND_DIR) {
        if (fsops::kind(Drive::EXTERNAL_SD_CARD, CP_FOLDER) != fsops::KIND_DIR) make_sd_folder(CP_FOLDER);
        if (make_sd_folder(path) != fsops::OK) {
            set_last_error("Cannot make", path);
            return;
        }
    }
    const size_t dl = strlen(path);
    const uint8_t format = save_is_128k ? ACTION_SNA_SAVE_TYPE_Z80 : ACTION_SNA_SAVE_TYPE_SNA;
    const char* ext = save_is_128k ? "z80" : "sna";
    snprintf(path + dl, psize - dl, "/Autosave %d.%s", at + 1, save_is_128k ? "sna" : "z80");
    f_unlink(path);                     // one of the other type, from before
    snprintf(path + dl, psize - dl, "/Autosave %d.%s", at + 1, ext);
    FIL& file = save_fil;
    FRESULT fr = f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr == FR_OK) {
        SdSink sink = { &file };
        fr = write_snapshot_file(sink, format) ? FR_OK : FR_DENIED;
        FRESULT fc = f_close(&file);
        if (fr == FR_OK) fr = fc;
        if (fr != FR_OK) f_unlink(path);
    }
    LOG("Autosave %s: %d\n", path, fr);
    if (fr != FR_OK) return;
    tape_note_after_save(1, path);
    rw.used |= (uint8_t) (1 << at);
    if (save_is_128k) rw.big |= (uint8_t) (1 << at);
    else rw.big &= (uint8_t) ~(1 << at);
    rw.at_t[at] = rw.pending_t;
}

// ACTION_SNA_SAVE with param3 = 4: the checkpoint is kept in snap_load_buffer for now
static void checkpoint_took() {
    rw.pending = true;
    rw.pending_at = rw.next;
    rw.pending_t = rw_ticks(rw_now());
    rw.next = (uint8_t) ((rw.next + 1) % CP_COUNT);
}

// A menu (or the S or G screen on its own) opens: the game's time stops
static void checkpoint_menu_opens() {
    checkpoint_flush();
    if (!rw.paused) {
        rw.paused = true;
        rw.menu_ms = rw_now();
    }
}

// Called from the main loop: writes a checkpoint the ROM sent, and takes the next when it is due
static void checkpoint_poll() {
    checkpoint_flush();
    uint32_t now = rw_now();
    if (rw.paused) {
        if (rom_state.nmi_active) return;
        // the menu has closed: the game's time carries on where it stopped
        rw.paused = false;
        uint16_t away = (uint16_t) (rw_ticks(now) - rw_ticks(rw.menu_ms));
        for (int i = 0; i < CP_COUNT; i++) rw.at_t[i] = (uint16_t) (rw.at_t[i] + away);
        rw.due_ms = now + (uint32_t) rw.secs * 1000;
        return;
    }
    if (!rw.on || rw.secs == 0 || !game_running || !sd_card_mounted || rom_state.nmi_active ||
            conv_running() || (int32_t) (now - rw.due_ms) < 0) {
        return;
    }
#ifdef PICOFACE_HOST
    if (!rw_host_take) return;          // the simulator takes them when its script says (checkpoint)
#endif
    rw.due_ms = now + (uint32_t) rw.secs * 1000;
    if (!sendNmiRequest(STARTUP_ACTION_CHECKPOINT, 0, 0)) {
        rw.due_ms = now + 500;          // (an NMI already active: soon again)
    }
}

#ifdef PICOFACE_HOST
// The simulator: the main loop's part; take: a checkpoint now (checkpoints on). True if one was sent.
bool host_checkpoint_poll(bool take) {
#if PICOMENU_HISCORE
    if (!take && hs_auto_poll()) return true;   // (a high score table put back: the Bluetooth on too)
#endif
    if (bt_on) return false;            // as the main loop: not while the Bluetooth is on
    bool was = (rom_state.flags & 2) != 0;
    checkpoint_poll();
    if (!take) return false;
    rw.due_ms = rw_now();
    rw_host_take = true;
    was = (rom_state.flags & 2) != 0;
    checkpoint_poll();
    rw_host_take = false;
    return !was && (rom_state.flags & 2) != 0;
}
#endif

// ACTION_CHECKPOINT_ASK: param1 0, before a load from the menu (through picoscreen): status 3
// to ask, 1 not (checkpoints off, no SD card, a checkpoint being loaded). Then 'y' or 'n'. On the
// Pico W the high score table of the game being left comes first (hs_leave: PICO_RUN, its own
// screen, or 2 to stay in the game).
static void nmi_action_checkpoint_ask() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t key = nmi_rom_data[2];
#if PICOMENU_HISCORE
    // first the high score table of the game being left
    if (key == 0 || hs_leaving()) {
        if (hs_leave(nmi_rom_data, key)) return;
        key = 0;                    // (the question, if any, is over: on with the load)
    }
#endif
    if (key == 0) {
        if (rw.loading) {           // a checkpoint: still the same game, checkpoints stay on
            rw.loading = false;
            nmi_rom_data[0] = 1;
            return;
        }
        checkpoints_stop();         // another game: its own checkpoints, if any
        rw.secs = cfg.checkpoint_s;
        // only for a game: not a reset (4, 1) or a test ROM, which the pico has ready
        bool game = !snap_prepared || prepared_is_game;
        nmi_rom_data[0] = cfg.checkpoint_s != 0 && sd_card_mounted && nmi_menu_open && game ? 3 : 1;
        return;
    }
    rw.on = key == 'y' || key == 'Y';
    LOG("Autosave %s\n", rw.on ? "on" : "off");
    nmi_rom_data[0] = 0;
}

// C on the K screen: the game's checkpoint files go (for good, with their tape notes), and the
// folder if nothing else is in it. False if one could not be erased.
static bool checkpoints_erase_files() {
    char* path = grab_path;
    if (!cp_folder(path, sizeof(grab_path))) return true;
    walk_stop();
    const size_t dl = strlen(path);
    bool ok = true;
    for (int i = 1; i <= CP_COUNT; i++) {
        for (int k = 0; k < 2; k++) {
            snprintf(path + dl, sizeof(grab_path) - dl, "/Autosave %d.%s", i, k == 0 ? "sna" : "z80");
            if (fsops::kind(Drive::EXTERNAL_SD_CARD, path) != fsops::KIND_FILE) continue;
            if (fsops::remove_path(Drive::EXTERNAL_SD_CARD, path)) tape_note_remove(1, path);
            else ok = false;
        }
    }
    path[dl] = '\0';
    f_unlink(path);                     // the folder, when it is empty
    checkpoints_clear();
    return ok;
}

// M on the K screen: checkpoint entry i becomes save slot n of the game (its label kept if the
// slot had one), moved, not copied. The message to show.
static const char* checkpoint_to_slot(int i, int n) {
    char game[screengrab::NAME_MAX + 1];
    if (slot_game_now(game) == 0 || !cp_folder(grab_path, sizeof(grab_path))) return "No game to move it for";
    SlotEntry e[slots::COUNT];
    slots_scan(game, e);
    char* to = erase_path;              // (free here: nothing is being loaded or erased)
    const size_t tsize = sizeof(erase_path);
    if (!slots_folder_make(game, to, tsize)) return "Could not make the slot folder";
    const char* ext = rw_ext(i);
    char name[80];
    slots::name(n, e[n - 1].used ? e[n - 1].label : "", name, sizeof(name));
    size_t tl = strlen(to);
    snprintf(to + tl, tsize - tl, "/%s.%s", name, ext);
    size_t dl = strlen(grab_path);
    snprintf(grab_path + dl, sizeof(grab_path) - dl, "/Autosave %d.%s", i + 1, ext);
    walk_stop();
    if (fsops::kind(Drive::EXTERNAL_SD_CARD, to) == fsops::KIND_FILE) {
        fsops::remove_path(Drive::EXTERNAL_SD_CARD, to);        // the slot it goes over
        tape_note_remove(1, to);
    }
    if (fsops::rename_path(Drive::EXTERNAL_SD_CARD, grab_path, to) != FILEOP_OK) return "Could not move it";
    tape_note_follow(1, grab_path, 1, to, false);
    rw.used &= (uint8_t) ~(1 << i);
    slots_after_save(game, n, name, ext);       // the slot's other files go, and it is the last
    static const char* const MOVED[] = { "Moved to slot 1", "Moved to slot 2", "Moved to slot 3",
        "Moved to slot 4", "Moved to slot 5", "Moved to slot 6", "Moved to slot 7", "Moved to slot 8",
        "Moved to slot 9" };
    return MOVED[n - 1];
}

// ---- High score tables (hiscore.h): the Pico W build ----
// Where a game keeps its table is found from a name or score in it (T on A-Autosave, or the
// hotkey T), and kept in /High Scores/<game>.txt; the table itself in <game>.bin. The .bin is
// put into the game when it is loaded (a snapshot: into it before it starts; a tape: when the
// menu is first opened once the tape has stopped). It is saved only when asked: S on the high
// score screen, or Y to "Save the high scores?" when the game is left from the menu (another
// game, a save slot or an autosave loaded, or a reset), which is asked only if the table is not
// the one saved. The .bin it replaces is kept as <game>.bak.
#if PICOMENU_HISCORE
#include "hiscore.h"
static void help_put_text(uint8_t* screen, int row, int col, const char* text, int len, uint8_t attr);
static bool tape_busy();
// the code at the menu's NMI (after hotkeys, which is at spare_space), and its data
static const uint16_t HS_MENU_CODE = SPARE_AT + 256, HS_MENU_DATA = SPARE_AT + 512;
static_assert(hiscore::TABLE_MAX <= 512, "the table fits below the screen's pieces (MOVE_INFO)");
// the code the high score screens run (PICO_RUN) at PICO_CODE_AT, its data after it
static const uint16_t HS_READ_DATA = PICO_CODE_AT + 64;
static const uint16_t HS_READ_MAX = 1728;
static const uint32_t HS_MEM_OFFSET = 16384;        // the game's 48K in snap_load_buffer
static struct {
    char game[screengrab::NAME_MAX + 1];    // the game whose table is in the Spectrum
    bool restored;          // the .bin is in the game (or was saved from it)
    uint8_t state;          // the screen: 0 its keys, 1 typing, 2 reading, 3 the finds, 4 a table, 5 "Forget it?", 6 putting it back, 7 saving
    bool direct;            // opened with the hotkey: X goes back to the game
    uint8_t leave;          // leaving the game (ACTION_CHECKPOINT_ASK): 0, 1 reading its table, 2 asking
    uint32_t sent_ms;       // when the game went into the Spectrum (a tape's table: put back 5 s after)
    uint8_t def;            // its .txt and .bin: 0 not looked at yet, 1 none, 2 both there
    bool tried;             // tried to put it back (hs_auto_poll), at the tape's tried_at
    uint32_t tried_at;      // (tape_block_ms then: a block since, and it is tried again)
    uint16_t check_cmp, check_len;  // ACTION_HS_CHECK: where the memory was copied to, its length
    uint16_t check_data;    //   and where the saved table is
    char typed[hiscore::TYPED_MAX + 1];
    uint32_t read_at;       // the next byte of the game's memory the finder reads
    uint16_t read_n;
    int8_t pick;
    const char* msg;
    uint8_t cap;            // a tape game's screen redrawn (hs_auto_poll): 0, 1 its memory being copied,
                            //   2 copied (in snap_load_buffer), 3 the patches made, for the restore's NMI
    uint32_t cap_ms;
} hs;
static bool hs_open_direct = false;     // the next A screen opens on the high scores (hotkey T)
// The finder's finds and the table picked: in snap_load_buffer after the game's memory (RAM is
// short), as they are only wanted while its screens are up
struct HsFound { hiscore::Finder f; hiscore::Table t; };
static const uint32_t HS_FOUND_OFFSET = 16384 + hiscore::MEM_SIZE;
static_assert(HS_FOUND_OFFSET + sizeof(HsFound) <= SNAP_LOAD_BUFFER_SIZE - 2048, "the finds fit");
static HsFound& hs_found() { return *reinterpret_cast<HsFound*>(snap_load_buffer + HS_FOUND_OFFSET); }

// "/High Scores/<game>.<ext>"
static void hs_path(char* out, size_t outsize, const char* game, const char* ext) {
    snprintf(out, outsize, "%s/%s.%s", hiscore::FOLDER, game, ext);
}

// The game in the Spectrum is this one now
static void hs_game_is(const char* game, bool restored) {
    snprintf(hs.game, sizeof(hs.game), "%s", game);
    hs.restored = restored;
    hs.sent_ms = to_ms_since_boot(get_absolute_time());
    hs.def = 0;
    hs.tried = false;
    hs.cap = 0;
    hs_bt_played = false;
}

// A tape put in (T in Browse Games, then the Virtual Tape Player or the Bluetooth: the Spectrum's
// own ROM loads it): its game is the tape's name, not known to be in the Spectrum yet (its table
// is put back only if the memory looks like it, hs_restore_code)
static void hs_tape_game(const char* tape) {
    char dir[sizeof(sd_cwd)], name[96], game[screengrab::NAME_MAX + 1];
    const char* slash = strrchr(tape, '/');
    const char* file = slash != nullptr ? slash + 1 : tape;
    snprintf(dir, sizeof(dir), "%.*s", (int) (slash != nullptr ? slash - tape : 0), tape);
    snprintf(name, sizeof(name), "%s", file);
    char* dot = strrchr(name, '.');
    if (dot != nullptr) *dot = '\0';
    game_base(dir, name, game);
    if (strcmp(game, hs.game) != 0 || hs.restored) {
        hs_game_is(game, false);
        LOG("High scores: the tape's game is %s\n", game);
    }
}

// The game's .txt: false if there is none (or it can't be a table)
static bool hs_def(const char* game, uint16_t& addr, uint16_t& len) {
    if (!sd_card_mounted || game[0] == '\0') return false;
    char path[sizeof(sd_cwd) + 32];
    hs_path(path, sizeof(path), game, "txt");
    UINT n = 0;
    char text[200];
    if (f_open(&save_fil, path, FA_READ) != FR_OK) return false;
    f_read(&save_fil, text, sizeof(text), &n);
    f_close(&save_fil);
    return hiscore::read_def(text, n, addr, len);
}

// Reads the game's .bin (len bytes) to out: false if there isn't one that long
static bool hs_bin_read(const char* game, uint8_t* out, uint16_t len) {
    char path[sizeof(sd_cwd) + 32];
    hs_path(path, sizeof(path), game, "bin");
    UINT n = 0;
    if (f_open(&save_fil, path, FA_READ) != FR_OK) return false;
    bool ok = f_size(&save_fil) == len && f_read(&save_fil, out, len, &n) == FR_OK && n == len;
    f_close(&save_fil);
    return ok;
}

static bool hs_bin_exists(const char* game, uint16_t len) {
    char path[sizeof(sd_cwd) + 32];
    hs_path(path, sizeof(path), game, "bin");
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK && fi.fsize == len;
}

// Is the game's .bin the table p (len bytes)?
static bool hs_bin_same(const char* game, const uint8_t* p, uint16_t len) {
    char path[sizeof(sd_cwd) + 32];
    hs_path(path, sizeof(path), game, "bin");
    uint8_t chunk[64];
    if (f_open(&save_fil, path, FA_READ) != FR_OK) return false;
    bool same = f_size(&save_fil) == len;
    for (uint16_t i = 0; same && i < len; i += sizeof(chunk)) {
        UINT n = 0;
        UINT want = (UINT) (len - i < sizeof(chunk) ? len - i : sizeof(chunk));
        same = f_read(&save_fil, chunk, want, &n) == FR_OK && n == want && memcmp(chunk, p + i, want) == 0;
    }
    f_close(&save_fil);
    return same;
}

// Saves the table p (len bytes) as the game's .bin, the one before it kept as its .bak
static bool hs_bin_write(const char* game, const uint8_t* p, uint16_t len) {
    if (hs_bin_same(game, p, len)) return true;
    char path[sizeof(sd_cwd) + 32], bak[sizeof(sd_cwd) + 32];
    hs_path(path, sizeof(path), game, "bin");
    hs_path(bak, sizeof(bak), game, "bak");
    if (fsops::kind(Drive::EXTERNAL_SD_CARD, hiscore::FOLDER) != fsops::KIND_DIR) make_sd_folder(hiscore::FOLDER);
    FILINFO fi;
    if (f_stat(path, &fi) == FR_OK) {
        f_unlink(bak);
        f_rename(path, bak);
    }
    UINT n = 0;
    bool ok = f_open(&save_fil, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
    if (ok) {
        ok = f_write(&save_fil, p, len, &n) == FR_OK && n == len;
        if (f_close(&save_fil) != FR_OK) ok = false;
    }
    if (!ok) {
        f_unlink(path);
        set_last_error("Cannot save", path);
    }
    LOG("High scores: %s saved: %d\n", path, ok);
    return ok;
}

// Where the Spectrum's address is in a snapshot in snap_load_buffer: a 48K one (header bytes
// first: .sna 27, .z80 30) or a 128K one (its banks, port 0x7FFD's value in X_7FFD)
static uint8_t* hs_in_snapshot(uint8_t* buf, bool paged, uint32_t header, uint32_t address) {
    if (!paged) return buf + header + (address - 0x4000);
    uint8_t bank = address < 0x8000 ? 5 : address < 0xC000 ? 2 : (uint8_t) (buf[snapload::X_7FFD] & 7);
    return snapload::bank_memory(buf, bank) + (address & 0x3FFF);
}

// ---- The table on the screen (hiscore.h's redraw): put back with the table, where the game
// shows it already (a snapshot showing its table, or a tape game that shows it once loaded).
// Its working memory is the listing's (list_pool, read again after); the Spectrum's ROM font
// comes from 48.rom.
static bool rom_file_open(FileHandle& f, const char* name);
struct HsRedraw {
    uint8_t scratch[hiscore::RD_SCRATCH];   // redraw_find's (then the patches, HsPatch)
    hiscore::Redraw w;
    uint8_t old[hiscore::TABLE_MAX];        // the old table, as it is on the screen
    uint8_t now[hiscore::TABLE_MAX];        // the saved one
    uint8_t rom[768];                       // the ROM's font, characters 32 to 127
    bool have_rom;
    uint16_t patches;                       // (hs_auto_poll's) bytes of the screen to change
};
struct HsPatch { uint16_t at; uint8_t was, now; };
static const uint16_t HS_PATCH_MAX = hiscore::RD_SCRATCH / sizeof(HsPatch);
static_assert(sizeof(HsRedraw) <= LIST_POOL_SIZE && alignof(HsRedraw) <= 8, "the redraw's memory fits the listing's");
static HsRedraw& hs_rd() { return *reinterpret_cast<HsRedraw*>(list_pool); }
static HsPatch* hs_patches() { return reinterpret_cast<HsPatch*>(hs_rd().scratch); }

static void hs_rd_begin() {
    walk_stop();                    // (the listing's memory: Browse Games reads the card again)
    big_valid = false;
    HsRedraw& r = hs_rd();
    r.have_rom = false;
    r.patches = 0;
    FileHandle* f = new (r.scratch) FileHandle();   // (the scratch is free until redraw_find)
    if (rom_file_open(*f, "48.rom")) {
        r.have_rom = f->seek(0x3D00) && f->read(r.rom, sizeof(r.rom)) == (int) sizeof(r.rom);
        f->close();
    }
}

// Finds the old table on the screen and draws the new one (r.now) there: put(screen offset,
// byte). mem: the memory (as hiscore's redraw), scr the screen shown
template <typename Mem, typename Put>
static bool hs_rd_run(uint16_t addr, uint16_t len, Mem mem, const uint8_t* scr, Put put) {
    HsRedraw& r = hs_rd();
    auto rom = [&](uint8_t c, uint8_t* g) {
        if (!r.have_rom || c < 32 || c > 127) return false;
        memcpy(g, r.rom + 8 * (c - 32), 8);
        return true;
    };
    for (uint16_t k = 0; k < len; k++) r.old[k] = mem((uint16_t) (addr + k));
    if (memcmp(r.old, r.now, len) == 0) return false;
    if (!hiscore::redraw_find(r.w, mem, scr, addr, len, rom, r.scratch)) {
        LOG("High scores: the table isn't on the screen\n");
        return false;
    }
    bool ok = hiscore::redraw_apply(r.w, mem, [&](int k) { return r.old[k]; }, [&](int k) { return r.now[k]; }, scr, rom, put);
    LOG("High scores: the table on the screen (%d runs, %s font): %s\n", r.w.runs, r.w.rom ? "the ROM's" : "the game's",
        ok ? "drawn again" : "not drawn (a character missing)");
    return ok;
}

// A snapshot of the game is going into the Spectrum (snap_load_buffer): its table put in it
static void hs_loaded_snapshot(const char* game, bool paged, uint32_t header) {
    hs_game_is(game, false);
    uint16_t addr, len;
    if (!hs_def(game, addr, len)) return;
    uint8_t* buf = reinterpret_cast<uint8_t*>(snap_load_buffer);
    char path[sizeof(sd_cwd) + 32];
    hs_path(path, sizeof(path), game, "bin");
    if (f_open(&save_fil, path, FA_READ) != FR_OK) {
        hs.restored = true;             // nothing kept yet: what the game has is the table
        return;
    }
    bool ok = f_size(&save_fil) == len;
    // is what the snapshot has there like the table? (the saved one read twice, a piece at a time)
    hiscore::Match m;
    uint8_t chunk[64];
    for (int pass = 0; ok && pass < 2; pass++) {
        f_lseek(&save_fil, 0);
        for (uint32_t i = 0; ok && i < len; i += sizeof(chunk)) {
            UINT want = (UINT) (len - i < sizeof(chunk) ? len - i : sizeof(chunk)), n = 0;
            ok = f_read(&save_fil, chunk, want, &n) == FR_OK && n == want;
            if (!ok) break;
            if (pass == 0) {
                m.learn(chunk, want);
            } else {
                uint8_t ram[sizeof(chunk)];
                for (UINT k = 0; k < want; k++) ram[k] = *hs_in_snapshot(buf, paged, header, addr + i + k);
                m.compare(ram, chunk, want);
            }
        }
        if (pass == 0) m.ranges();
    }
    if (ok && !m.ok()) {
        f_close(&save_fil);
        LOG("High scores: %s's snapshot has no table like it there\n", game);
        return;
    }
    if (ok && len <= hiscore::TABLE_MAX) {
        // the table on the snapshot's screen drawn again too, if it shows it
        hs_rd_begin();
        UINT n = 0;
        f_lseek(&save_fil, 0);
        if (f_read(&save_fil, hs_rd().now, len, &n) == FR_OK && n == len) {
            uint8_t* scr = paged ? snapload::bank_memory(buf, (buf[snapload::X_7FFD] & 8) ? 7 : 5) : buf + header;
            hs_rd_run(addr, len, [&](uint16_t a) { return *hs_in_snapshot(buf, paged, header, a); }, scr,
                [&](uint16_t at, uint8_t v) { scr[at] = v; });
        }
    }
    // (read straight into the snapshot, a bank at a time)
    f_lseek(&save_fil, 0);
    for (uint32_t i = 0; ok && i < len; ) {
        uint32_t a = addr + i;
        uint32_t run = 0x4000 - (a & 0x3FFF);
        if (run > len - i) run = len - i;
        UINT n = 0;
        ok = f_read(&save_fil, hs_in_snapshot(buf, paged, header, a), run, &n) == FR_OK && n == run;
        i += run;
    }
    f_close(&save_fil);
    hs.restored = ok;
    LOG("High scores: %s put in the snapshot: %d\n", game, ok);
}

// The snapshot in snap_load_buffer is going into the Spectrum (nmi_action_begin_snap_read): the
// game it is (load_snapshot_file said) is the one now; a reset or a snapshot from a PC: none
static void hs_commit() {
    if (!hs_next.valid) {
        hs_game_is("", false);
        return;
    }
    hs_next.valid = false;
    if (hs_next.tape) {
        hs_game_is(hs_next.name, false);
    } else {
        hs_loaded_snapshot(hs_next.name, snap_is_paged, current_snap_size == Z80_FILE_SIZE ? 30 : SNA_HEADER_SIZE);
    }
}

// Code at code that gives the game its saved table (at data, read here) if what is there now is
// like it: the memory copied to cmp, ACTION_HS_CHECK says, then the table copied in; then JP next.
// False if there is no table to give it.
static bool hs_restore_code(uint8_t* nmi_rom_data, uint16_t code, uint16_t data, uint16_t cmp, uint16_t next,
        uint16_t patch = 0) {
    uint16_t addr, len;
    if (!hs_def(hs.game, addr, len) || !hs_bin_read(hs.game, nmi_rom_data + data, len)) return false;
    uint8_t* c = nmi_rom_data + code;
    auto word = [&](uint16_t w) { *c++ = (uint8_t) (w & 0xFF); *c++ = (uint8_t) (w >> 8); };
    *c++ = 0x21; word(addr);                    // LD HL,table
    *c++ = 0x11; word(cmp);                     // LD DE,cmp
    *c++ = 0x01; word(len);                     // LD BC,length
    *c++ = 0xED; *c++ = 0xB0;                   // LDIR
    *c++ = 0xDD; *c++ = 0x21; word(0);          // LD IX,0
    *c++ = 0xDD; *c++ = 0x36; *c++ = 1; *c++ = ACTION_HS_CHECK;     // LD (IX+1),ACTION_HS_CHECK
    *c++ = 0xCD; word(WAITPICO);                // CALL waitpico
    *c++ = 0xB7;                                // OR A
    *c++ = 0xCA; word(next);                    // JP Z,next (not like it)
    *c++ = 0x21; word(data);                    // LD HL,data
    *c++ = 0x11; word(addr);                    // LD DE,table
    *c++ = 0x01; word(len);                     // LD BC,length
    *c++ = 0xED; *c++ = 0xB0;                   // LDIR
    if (patch != 0) {
        // the table on the screen (hs_auto_poll): records of the address, the byte that was there
        // and the new one, ending with address 0; all of them only if the screen still has every
        // byte that was (else it has moved on: nothing is drawn)
        *c++ = 0x21; word(patch);               // LD HL,patch
        uint8_t* check = c;
        *c++ = 0x5E; *c++ = 0x23; *c++ = 0x56; *c++ = 0x23;     // LD E,(HL) / INC HL / LD D,(HL) / INC HL
        *c++ = 0x7A; *c++ = 0xB7;               // LD A,D / OR A
        *c++ = 0x28; *c++ = 9;                  // JR Z,draw
        *c++ = 0x1A; *c++ = 0xBE;               // LD A,(DE) / CP (HL)
        *c++ = 0xC2; word(next);                // JP NZ,next (the screen is not as it was)
        *c++ = 0x23; *c++ = 0x23;               // INC HL / INC HL
        *c++ = 0x18; *c = (uint8_t) (check - (c + 1)); c++;     // JR check
        *c++ = 0x21; word(patch);               // draw: LD HL,patch
        uint8_t* draw = c;
        *c++ = 0x5E; *c++ = 0x23; *c++ = 0x56; *c++ = 0x23;     // LD E,(HL) / INC HL / LD D,(HL) / INC HL
        *c++ = 0x7A; *c++ = 0xB7;               // LD A,D / OR A
        *c++ = 0xCA; word(next);                // JP Z,next (all drawn)
        *c++ = 0x23; *c++ = 0x7E; *c++ = 0x12; *c++ = 0x23;     // INC HL / LD A,(HL) / LD (DE),A / INC HL
        *c++ = 0x18; *c = (uint8_t) (draw - (c + 1)); c++;      // JR draw
    }
    *c++ = 0xC3; word(next);                    // JP next
    hs.check_cmp = cmp;
    hs.check_data = data;
    hs.check_len = len;
    return true;
}

// ACTION_HS_CHECK: is the memory (copied to check_cmp) like the saved table?
__attribute__((noinline)) static void nmi_action_hs_check(uint8_t* nmi_rom_data) {
    bool like = hs.check_len != 0 &&
        hiscore::table_matches(nmi_rom_data + hs.check_cmp, nmi_rom_data + hs.check_data, hs.check_len);
    hs.restored = like;
    hs.check_len = 0;
    LOG("High scores: %s %s\n", hs.game, like ? "put back" : "not put back: no table like it there");
    nmi_rom_data[0] = like ? 1 : 0;
}

// The menu's NMI (sendNmiRequest): a tape game's table put into it (if hs_auto_poll has not),
// by code at HS_MENU_CODE called in place of the menu (then the menu, or hotkeys)
__attribute__((noinline)) static void hs_menu_nmi(uint8_t* nmi_rom_data) {
    uint16_t addr, len;
    if (hs.restored || !sd_card_mounted || !hs_def(hs.game, addr, len) || !hs_bin_exists(hs.game, len)) return;
    // (not while a tape loads, or in the game's first 5 s: it would set up its own table after it)
    if (tape_busy() || to_ms_since_boot(get_absolute_time()) - hs.sent_ms < 5000) return;
    const uint16_t menu = (uint16_t) (nmi_rom_data[HK_CALLMENU + 1] | nmi_rom_data[HK_CALLMENU + 2] << 8);
    // (the memory is compared in the screen's pieces, MOVE_INFO, which are not wanted until the menu)
    if (!hs_restore_code(nmi_rom_data, HS_MENU_CODE, HS_MENU_DATA, SPARE_AT + 1024, menu)) return;
    nmi_rom_data[HK_CALLMENU + 1] = (uint8_t) (HS_MENU_CODE & 0xFF);
    nmi_rom_data[HK_CALLMENU + 2] = (uint8_t) (HS_MENU_CODE >> 8);
}

// ACTION_SNA_SAVE for STARTUP_ACTION_CHECKPOINT: true if it was hs_auto_poll's copy of the
// game's memory (now in snap_load_buffer), not an autosave
static bool hs_copied() {
    if (hs.cap != 1) return false;
    hs.cap = 2;
    return true;
}
static int8_t cp_bank_now();
static uint8_t cp_port_now();

// hs_auto_poll: the game's memory copied (cpsave's copy, in snap_load_buffer: hs.cap 2). If
// its screen shows the old table, the bytes to draw the saved one there (HsPatch, for the restore's
// NMI, hs_auto_code)
static void hs_rd_patches() {
    uint16_t addr, len;
    if (!hs_def(hs.game, addr, len) || len > hiscore::TABLE_MAX) return;
    hs_rd_begin();
    HsRedraw& r = hs_rd();
    if (!hs_bin_read(hs.game, r.now, len)) return;
    uint8_t* buf = reinterpret_cast<uint8_t*>(snap_load_buffer);
    const bool big = save_is_128k;
    auto mem = [&](uint16_t a) -> uint8_t {
        if (!big) return buf[SNA_HEADER_SIZE + (a - 0x4000)];
        uint8_t bank = a < 0x8000 ? 5 : a < 0xC000 ? 2 : (uint8_t) cp_bank_now();
        return snapload::bank_memory(buf, bank)[a & 0x3FFF];
    };
    const uint8_t* scr = big ? snapload::bank_memory(buf, (cp_port_now() & 8) ? 7 : 5) : buf + SNA_HEADER_SIZE;
    HsPatch* p = hs_patches();
    uint16_t n = 0;
    bool over = false;
    bool ok = hs_rd_run(addr, len, mem, scr, [&](uint16_t at, uint8_t v) {
        if (n < HS_PATCH_MAX) p[n++] = { at, scr[at], v };
        else over = true;
    });
    r.patches = ok && !over ? n : 0;
}

// A tape's game (a .tap or .tzx: loaded through the tape trap, made into a snapshot, or put in
// and played from the Virtual Tape Player or the Bluetooth): its table is put back by itself once
// the game has set up its own, 5 s after the tape last gave it a block (and after it went in), if
// the memory there looks like the table (hs_restore_code): an NMI the game does not see
// (STARTUP_ACTION_CHECKPOINT with that code in place of cpsave). If it doesn't, it is tried
// again after the tape has given more. True if one was sent.
static bool hs_auto_nmi = false;
static bool hs_auto_poll() {
    static bool bt_was = false;
    if (bt_playing) {
        bt_was = true;
        return false;
    }
    if (bt_was) {
        bt_was = false;
        hs_bt_played = true;
        tape_block_ms = to_ms_since_boot(get_absolute_time());     // (the Bluetooth has stopped: as a block)
    }
    if (hs.restored || hs.game[0] == '\0' || hs.def == 1 || !game_running || !sd_card_mounted ||
            rom_state.nmi_active || (rom_state.flags & 2) != 0 || conv_running() || tape_busy()) {
        return false;
    }
    if (hs.tried && hs.tried_at == tape_block_ms) return false;        // (nothing new from the tape)
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - hs.sent_ms < 5000 || (tape_block_ms != 0 && now - tape_block_ms < 5000)) return false;
    if (hs.def == 0) {
        // (the SD card is looked at once for each game)
        uint16_t addr, len;
        hs.def = hs_def(hs.game, addr, len) && hs_bin_exists(hs.game, len) ? 2 : 1;
        if (hs.def == 1) return false;
    }
    if (hs.cap == 0 && !bt_on) {
        // first a copy of the game's memory (as an autosave's, cpsave): does its screen show the
        // table? (not with the Bluetooth on: its memory is snap_load_buffer)
        hs.cap = 1;
        hs.cap_ms = now;
        if (!sendNmiRequest(STARTUP_ACTION_CHECKPOINT, 0, 0)) {
            hs.cap = 0;
            return false;
        }
        return true;
    }
    if (hs.cap == 1) {
        if (now - hs.cap_ms > 3000) hs.cap = 0;     // (the copy never came: again)
        return false;
    }
    if (hs.cap == 2) {
        hs_rd_patches();
        hs.cap = 3;
    }
    hs.tried = true;
    hs.tried_at = tape_block_ms;
    hs_auto_nmi = true;
    if (!sendNmiRequest(STARTUP_ACTION_CHECKPOINT, 0, 0)) {
        hs_auto_nmi = false;
        hs.tried = false;               // (soon again)
        return false;
    }
    return true;
}

// sendNmiRequest, for hs_auto_poll's NMI: the code at CP_AT, where cpsave would go (the saved
// table and the memory's copy after it), then out of the NMI. False if there is nothing to do.
static const uint16_t HS_AUTO_PATCH = CP_AT + 128 + 2 * hiscore::TABLE_MAX;
static const uint16_t HS_AUTO_PATCH_MAX = (CP_LIMIT - HS_AUTO_PATCH - 2) / 4;
static bool hs_auto_code(uint8_t* nmi_rom_data) {
    hs_auto_nmi = false;
    static_assert(CP_AT + 128 + 2 * hiscore::TABLE_MAX + 4 * 64 <= CP_LIMIT, "the tables fit where cpsave goes");
    uint16_t patch = 0;
    if (hs.cap == 3 && hs_rd().patches > 0 && hs_rd().patches <= HS_AUTO_PATCH_MAX) {
        uint8_t* d = nmi_rom_data + HS_AUTO_PATCH;
        const HsPatch* p = hs_patches();
        for (uint16_t i = 0; i < hs_rd().patches; i++) {
            const uint16_t at = (uint16_t) (0x4000 + p[i].at);
            *d++ = (uint8_t) (at & 0xFF); *d++ = (uint8_t) (at >> 8); *d++ = p[i].was; *d++ = p[i].now;
        }
        *d++ = 0; *d = 0;
        patch = HS_AUTO_PATCH;
    }
    hs.cap = 0;
    // (code at CP_AT, the saved table at +128, the memory's copy after it, then the patches)
    return hs_restore_code(nmi_rom_data, CP_AT, CP_AT + 128, CP_AT + 128 + hiscore::TABLE_MAX, EXIT_NO_SCREEN, patch);
}

// Code at PICO_CODE_AT (PICO_RUN): n bytes copied from to (LDIR), then back
static void hs_run_copy(uint8_t* nmi_rom_data, uint16_t from, uint16_t to, uint16_t n) {
    uint8_t* c = nmi_rom_data + PICO_CODE_AT;
    auto word = [&](uint16_t w) { *c++ = (uint8_t) (w & 0xFF); *c++ = (uint8_t) (w >> 8); };
    *c++ = 0x21; word(from);                    // LD HL,from
    *c++ = 0x11; word(to);                      // LD DE,to
    *c++ = 0x01; word(n);                       // LD BC,n
    *c++ = 0xED; *c++ = 0xB0;                   // LDIR
    *c = 0xC9;                                  // RET
    nmi_rom_data[0] = PICO_RUN;
}

// BREAK goes back to the game: from the menu, and only when nothing was made ready for the load
// (a game picked in Browse Games; a reset, Recent, Reload, an autosave or a slot has already
// changed things for it)
static bool hs_can_stay() { return nmi_menu_open && !snap_prepared; }

static void hs_draw_leave(uint8_t* nmi_rom_data) {
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer);
    slots_banner(screen, "High scores: ", hs.game);
    auto put = [&](int row, int col, const char* t, uint8_t a) { help_put_text(screen, row, col, t, (int) strlen(t), a); };
    put(2, 1, "The game's high score table", gameinfo::ATTR_TEXT);
    put(3, 1, "is not the one saved.", gameinfo::ATTR_TEXT);
    put(5, 1, "Save the high scores before", gameinfo::ATTR_HEADING);
    put(6, 1, "leaving this game?", gameinfo::ATTR_HEADING);
    put(8, 1, "Y", gameinfo::ATTR_KEYS);
    put(8, 8, "Yes, save them", gameinfo::ATTR_TEXT);
    put(9, 1, "N", gameinfo::ATTR_KEYS);
    put(9, 8, "No", gameinfo::ATTR_TEXT);
    if (hs_can_stay()) {
        put(10, 1, "BREAK", gameinfo::ATTR_KEYS);
        put(10, 8, "Back to the game", gameinfo::ATTR_TEXT);
    }
    preview_screen = screen;
    nmi_rom_data[0] = 0;
}

static bool hs_leaving() { return hs.leave != 0; }

// Leaving the game (ACTION_CHECKPOINT_ASK, through picoscreen, before a load from the menu):
// its table read, and "Save the high scores?" if it is not the one saved. True: the status is
// set (go on with the question, or 2: back to the game, see hs_can_stay); false: carry on with
// the load.
__attribute__((noinline)) static bool hs_leave(uint8_t* nmi_rom_data, uint8_t key) {
    uint16_t addr, len;
    if (key == 0) {
        hs.leave = 0;
        // only a table that is the game's own (put back, or none saved yet)
        if (!sd_card_mounted || !hs_def(hs.game, addr, len) || (!hs.restored && hs_bin_exists(hs.game, len))) return false;
        hs.leave = 1;
        hs_run_copy(nmi_rom_data, addr, HS_READ_DATA, len);
        return true;
    }
    if (hs.leave == 0 || !hs_def(hs.game, addr, len)) {
        hs.leave = 0;
        return false;
    }
    if (hs.leave == 1) {
        if (key != 254) return false;
        if (hs_bin_same(hs.game, nmi_rom_data + HS_READ_DATA, len)) {
            hs.leave = 0;
            return false;
        }
        if (hs_bin_exists(hs.game, len) && hs_bin_read(hs.game, nmi_rom_data + HS_READ_DATA + hiscore::TABLE_MAX, len) &&
                !hiscore::table_matches(nmi_rom_data + HS_READ_DATA, nmi_rom_data + HS_READ_DATA + hiscore::TABLE_MAX, len)) {
            // not like the game's table (another game is in the Spectrum): nothing to save
            hs.leave = 0;
            return false;
        }
        hs.leave = 2;
        hs_draw_leave(nmi_rom_data);
        return true;
    }
    // asking
    if (key == 'y' || key == 'Y') {
        hs_bin_write(hs.game, nmi_rom_data + HS_READ_DATA, len);
    } else if (is_key_break(key) && hs_can_stay()) {
        hs.leave = 0;
        nmi_rom_data[0] = 2;
        return true;
    } else if (key != 'n' && key != 'N') {
        hs_draw_leave(nmi_rom_data);
        return true;
    }
    hs.leave = 0;
    return false;
}

// The finder's code: the game's memory from read_at into HS_READ_DATA (PICO_RUN)
static void hs_read_next(uint8_t* nmi_rom_data) {
    uint32_t n = 0x10000 - hs.read_at;
    if (n > HS_READ_MAX) n = HS_READ_MAX;
    hs.read_n = (uint16_t) n;
    uint8_t* c = nmi_rom_data + PICO_CODE_AT;
    auto word = [&](uint16_t w) { *c++ = (uint8_t) (w & 0xFF); *c++ = (uint8_t) (w >> 8); };
    *c++ = 0x21; word((uint16_t) hs.read_at);       // LD HL,from
    *c++ = 0x11; word(HS_READ_DATA);                // LD DE,data
    *c++ = 0x01; word((uint16_t) n);                // LD BC,n
    *c++ = 0xED; *c++ = 0xB0;                       // LDIR
    *c = 0xC9;                                      // RET
    nmi_rom_data[0] = PICO_RUN;
}

static uint8_t* hs_mem() { return reinterpret_cast<uint8_t*>(snap_load_buffer) + HS_MEM_OFFSET; }
static_assert(HS_MEM_OFFSET >= 6912 && HS_MEM_OFFSET + hiscore::MEM_SIZE <= SNAP_LOAD_BUFFER_SIZE, "the game's memory is clear of the screen");

// The high score table's screens, on A - Autosave (nmi_action_checkpoints). Returns false when
// it is left for the autosaves' screen (true: the status is set)
__attribute__((noinline)) static bool hs_action(uint8_t* nmi_rom_data, uint8_t key) {
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer);
    HsFound& found = hs_found();
    const bool back = is_key_break(key) || key == 'x' || key == 'X';
    const char* g = hs.game;                // the game in the Spectrum
    uint16_t addr = 0, len = 0;
    bool known = hs_def(g, addr, len);
    bool saved = known && hs_bin_exists(g, len);
    hs.msg = nullptr;
    if (key != 0) {
        switch (hs.state) {
        case 0:
            if (back) {
                if (hs.direct) {
                    nmi_rom_data[0] = 1;
                    return true;
                }
                return false;
            } else if ((key == 'f' || key == 'F') && g[0] != '\0') {
                hs.state = 1;
                hs.typed[0] = '\0';
            } else if ((key == 'r' || key == 'R') && saved) {
                if (hs_bin_read(g, nmi_rom_data + HS_READ_DATA, len)) {
                    hs.state = 6;
                    hs_run_copy(nmi_rom_data, HS_READ_DATA, addr, len);
                    return true;
                }
                hs.msg = "Cannot read the saved table";
            } else if ((key == 's' || key == 'S') && known) {
                // S: the table saved now (read from the game first)
                hs.state = 7;
                hs_run_copy(nmi_rom_data, addr, HS_READ_DATA, len);
                return true;
            } else if ((key == 'c' || key == 'C') && known) {
                hs.state = 5;
            }
            break;
        case 1: {       // typing
            size_t n = strlen(hs.typed);
            if (key == notes::KEY_BREAK) {
                hs.state = 0;
            } else if (key == notes::KEY_DELETE) {
                if (n > 0) hs.typed[n - 1] = '\0';
            } else if (key == notes::KEY_ENTER) {
                if (hiscore::classify(hs.typed) == hiscore::T_NONE || strlen(hs.typed) < 2) {
                    hs.msg = "Two letters or digits at least";
                } else {
                    hs.state = 2;
                    hs.read_at = 0x4000;
                    sna_in_buffer = false;          // (the game's memory goes where a snapshot would)
                    snap_is_paged = false;
                    hs_read_next(nmi_rom_data);
                    return true;
                }
            } else if (key >= 32 && key < 127 && key != '~' && n < hiscore::TYPED_MAX) {
                hs.typed[n] = (char) (key >= 'a' && key <= 'z' ? key - 32 : key);
                hs.typed[n + 1] = '\0';
            }
            break;
        }
        case 2:         // reading: the code has run
            if (key == 254) {
                memcpy(hs_mem() + (hs.read_at - 0x4000), nmi_rom_data + HS_READ_DATA, hs.read_n);
                hs.read_at += hs.read_n;
                if (hs.read_at < 0x10000) {
                    hs_read_next(nmi_rom_data);
                    return true;
                }
                found.f.find(hs_mem(), hs.typed);
                if (found.f.count == 0) {
                    hs.state = 1;
                    hs.msg = "Not found: try another";
                } else {
                    hs.state = 3;
                }
            }
            break;
        case 3: {       // the finds
            int n = key >= '1' && key <= '9' ? key - '0' : 0;
            if (back) {
                hs.state = 1;
            } else if (n > 0 && n <= found.f.count) {
                hs.pick = (int8_t) (n - 1);
                found.t = hiscore::table_for(hs_mem(), found.f.hits[n - 1]);
                hs.state = 4;
            }
            break;
        }
        case 4:         // a table
            if (back) {
                hs.state = 3;
            } else if (key == 's' || key == 'S') {
                char path[sizeof(sd_cwd) + 32];
                if (fsops::kind(Drive::EXTERNAL_SD_CARD, hiscore::FOLDER) != fsops::KIND_DIR) make_sd_folder(hiscore::FOLDER);
                hs_path(path, sizeof(path), g, "txt");
                char text[200];
                int tl = hiscore::write_def(text, sizeof(text), found.t.addr, found.t.len);
                UINT w = 0;
                bool ok = f_open(&save_fil, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
                if (ok) {
                    ok = f_write(&save_fil, text, (UINT) tl, &w) == FR_OK && w == (UINT) tl;
                    if (f_close(&save_fil) != FR_OK) ok = false;
                }
                if (ok) {
                    hs_bin_write(g, hs_mem() + (found.t.addr - 0x4000), found.t.len);
                    hs.restored = true;
                    hs.msg = "Saved: it is kept from now on";
                } else {
                    hs.msg = "Could not save it";
                }
                hs.state = 0;
            }
            break;
        case 5:         // "Forget it?"
            hs.state = 0;
            if (key == 'y' || key == 'Y') {
                char path[sizeof(sd_cwd) + 32];
                hs_path(path, sizeof(path), g, "txt");
                f_unlink(path);
                hs_path(path, sizeof(path), g, "bin");
                f_unlink(path);
                hs_path(path, sizeof(path), g, "bak");
                f_unlink(path);
                hs.msg = "Forgotten";
            }
            break;
        case 6:         // put back: the code has run
            hs.state = 0;
            hs.restored = true;
            hs.msg = "The saved table is back in it";
            break;
        case 7:         // S: the table has been read
            hs.state = 0;
            if (key == 254) {
                hs.msg = hs_bin_same(g, nmi_rom_data + HS_READ_DATA, len) ? "Saved already: no change" :
                    hs_bin_write(g, nmi_rom_data + HS_READ_DATA, len) ? "Saved (the old one: its .bak)" : "Could not save it";
                hs.restored = true;
            }
            break;
        }
        known = hs_def(g, addr, len);
        saved = known && hs_bin_exists(g, len);
    }
    // the screen
    slots_banner(screen, "High scores: ", g[0] != '\0' ? g : "no game");
    char t[48];
    auto put = [&](int row, int col, const char* s, uint8_t a) { help_put_text(screen, row, col, s, (int) strlen(s), a); };
    auto key_line = [&](int row, const char* k, const char* s) { put(row, 1, k, gameinfo::ATTR_KEYS); put(row, 4, s, gameinfo::ATTR_TEXT); };
    switch (hs.state) {
    case 0:
    case 5:
        if (g[0] == '\0') {
            put(2, 1, "Load a game first.", gameinfo::ATTR_TEXT);
        } else if (known) {
            snprintf(t, sizeof(t), "Its table: %u bytes at %u", (unsigned) len, (unsigned) addr);
            put(2, 1, t, gameinfo::ATTR_TEXT);
            put(3, 1, saved ? "Saved: put back each time the" : "Not saved yet.", gameinfo::ATTR_DIM);
            put(4, 1, saved ? "game is loaded." : "", gameinfo::ATTR_DIM);
        } else {
            put(2, 1, "Not found yet. Play a game", gameinfo::ATTR_TEXT);
            put(3, 1, "and put your name or score in", gameinfo::ATTR_TEXT);
            put(4, 1, "its table, then F finds it.", gameinfo::ATTR_TEXT);
        }
        if (g[0] != '\0') key_line(7, "F", "Find the table");
        if (known) key_line(8, "S", "Save the table now");
        if (saved) key_line(9, "R", "Put the saved table back");
        if (known) key_line(10, "C", "Forget it (its files go)");
        key_line(12, "X", "Back");
        if (hs.state == 5) {
            put(19, 1, "Forget where the table is?", gameinfo::ATTR_HEADING);
            put(20, 1, "Y-Yes  N-No", gameinfo::ATTR_KEYS);
        }
        break;
    case 1:
        put(2, 1, "Type a name or a score that", gameinfo::ATTR_TEXT);
        put(3, 1, "is in the game's table now", gameinfo::ATTR_TEXT);
        put(4, 1, "(what you typed in is best).", gameinfo::ATTR_TEXT);
        snprintf(t, sizeof(t), "> %s_", hs.typed);
        put(6, 1, t, gameinfo::ATTR_HEADING);
        put(8, 1, "ENTER finds it  DELETE rubs out", gameinfo::ATTR_KEYS);
        put(9, 1, "BREAK back", gameinfo::ATTR_KEYS);
        break;
    case 3: {
        snprintf(t, sizeof(t), "\"%s\": %d found", hs.typed, found.f.total);
        put(2, 1, t, gameinfo::ATTR_TEXT);
        for (int k = 0; k < found.f.count; k++) {
            const hiscore::Hit& h = found.f.hits[k];
            snprintf(t, sizeof(t), "%d", k + 1);
            put(4 + k, 1, t, gameinfo::ATTR_KEYS);
            snprintf(t, sizeof(t), "%5u", (unsigned) h.addr);
            put(4 + k, 3, t, gameinfo::ATTR_DIM);
            char v[24];
            uint32_t from = h.addr - 0x4000;
            int show = (int) (hiscore::MEM_SIZE - from < 22 ? hiscore::MEM_SIZE - from : 22);
            hiscore::show_bytes(hs_mem() + from, show, h, v, (h.enc == hiscore::BCD || h.enc == hiscore::BIN) ? 23 : 22);
            put(4 + k, 9, v, gameinfo::ATTR_TEXT);
        }
        if (found.f.total > found.f.count) put(14, 1, "(more: type more of it)", gameinfo::ATTR_DIM);
        if (found.f.count == 1) snprintf(t, sizeof(t), "1 Is it the table?");
        else snprintf(t, sizeof(t), "1-%d Which is the table?", found.f.count);
        put(16, 1, t, gameinfo::ATTR_KEYS);
        put(17, 1, "X Back", gameinfo::ATTR_KEYS);
        break;
    }
    case 4: {
        const hiscore::Hit& h = found.f.hits[hs.pick];
        if (found.t.stride != 0) {
            snprintf(t, sizeof(t), "%u bytes at %u, %u entries", (unsigned) found.t.len, (unsigned) found.t.addr, (unsigned) found.t.entries);
        } else {
            snprintf(t, sizeof(t), "Only this: %u bytes at %u", (unsigned) found.t.len, (unsigned) found.t.addr);
        }
        put(2, 1, t, gameinfo::ATTR_TEXT);
        const int row_len = found.t.stride != 0 ? found.t.stride : found.t.len;
        const int rows = (found.t.len + row_len - 1) / row_len;
        for (int r = 0; r < rows && r < 14; r++) {
            char v[32];
            int n = row_len;
            if (r * row_len + n > found.t.len) n = found.t.len - r * row_len;
            if (n > 30) n = 30;
            hiscore::show_bytes(hs_mem() + (found.t.addr - 0x4000) + r * row_len, n, h, v, (h.enc == hiscore::BCD || h.enc == hiscore::BIN) ? 31 : 31);
            put(4 + r, 1, v, gameinfo::ATTR_HEADING);
        }
        put(19, 1, "Is this the high score table?", gameinfo::ATTR_TEXT);
        key_line(20, "S", "Save it as the game's table");
        key_line(21, "X", "Back to the finds");
        break;
    }
    default:
        break;
    }
    if (hs.msg != nullptr) put(22, 1, hs.msg, gameinfo::ATTR_TEXT);
    preview_screen = screen;
    nmi_rom_data[0] = 0;
    return true;
}
#endif

// ACTION_CHECKPOINTS: K on the menu. BYTE param1 = the key (0: opened). Status 0 stays, 1 closed,
// 2 the checkpoint chosen is ready (loaded into snap_load_buffer)
__attribute__((noinline)) static void nmi_action_checkpoints() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t key = nmi_rom_data[2];
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer);
    int order[CP_COUNT];
    int count = rw_order(order);
    const int n = key >= '1' && key <= '9' ? key - '0' : 0;
    const bool back = is_key_break(key) || key == 'x' || key == 'X';
    if (key == 0) cp_state = 0;
#if PICOMENU_HISCORE
    // T: the high score table's screens (cp_state 10); the hotkey T opens them straight away
    if (key == 0 && hs_open_direct) {
        hs_open_direct = false;
        cp_state = 10;
        hs.state = 0;
        hs.direct = true;
        if (hs_action(nmi_rom_data, 0)) return;
    } else if (cp_state == 10) {
        if (hs_action(nmi_rom_data, key)) return;
        cp_state = 0;
        key = 0;                // (back on the autosaves' screen: drawn as it is)
    } else if (cp_state == 0 && (key == 't' || key == 'T')) {
        cp_state = 10;
        hs.state = 0;
        hs.direct = false;
        if (hs_action(nmi_rom_data, 0)) return;
    }
#endif
    slot_msg = nullptr;
    switch (key == 0 ? 255 : cp_state) {
    case 0:
        if (n > 0 && n <= count) {
            // carry on from it: the newer ones go, and the game's time goes back to it
            int i = order[n - 1];
            uint16_t ago = (uint16_t) (rw_ticks(rw.menu_ms) - rw.at_t[i]);
            if (!cp_folder(erase_path, sizeof(erase_path))) break;
            int len = snprintf(erase_scratch, sizeof(erase_scratch), "Autosave %d", i + 1);
            snprintf(erase_scratch + len + 1, sizeof(erase_scratch) - (size_t) len - 1, "%s", rw_ext(i));
            load_snapshot_file(1, erase_path, erase_scratch, false);
            snap_prepared = true;
            for (int k = 0; k < n - 1; k++) rw.used &= (uint8_t) ~(1 << order[k]);
            for (int k = 0; k < CP_COUNT; k++) rw.at_t[k] = (uint16_t) (rw.at_t[k] + ago);
            rw.next = (uint8_t) ((i + 1) % CP_COUNT);
            rw.loading = true;
            nmi_rom_data[0] = 2;
            return;
        } else if ((key == 'v' || key == 'V') && count > 0) {
            cp_state = 1;
        } else if ((key == 'm' || key == 'M') && count > 0) {
            cp_state = 3;
        } else if ((key == 'c' || key == 'C') && sd_card_mounted) {
            cp_state = 6;
        } else if (key == 'o' || key == 'O') {
            // O: autosave on (or off) for this game, whatever was answered when it was loaded
            char g[screengrab::NAME_MAX + 1];
            if (rw.on) {
                rw.on = false;
                slot_msg = "Autosave off for this game";
            } else if (!sd_card_mounted || slot_game_now(g) == 0) {
                slot_msg = "No game to autosave";
            } else {
                rw.on = true;
                if (rw.secs == 0) rw.secs = cfg.checkpoint_s != 0 ? cfg.checkpoint_s : 20;
                slot_msg = "Autosave on for this game";
            }
        } else if (back) {
            nmi_rom_data[0] = 1;
            return;
        }
        break;
    case 1:             // V, then a checkpoint: its screen
    case 3:             // M, then a checkpoint
        if (n > 0 && n <= count) {
            cp_pick = (uint8_t) order[n - 1];
            if (cp_state == 3) {
                cp_state = 4;
                break;
            }
            cp_state = 0;
            if (cp_folder(grab_path, sizeof(grab_path))) {
                size_t dl = strlen(grab_path);
                snprintf(grab_path + dl, sizeof(grab_path) - dl, "/Autosave %d.%s", cp_pick + 1, rw_ext(cp_pick));
                preview_screen = nullptr;
                if (find_preview(1, grab_path, rw_ext(cp_pick)) == 0 && preview_screen != nullptr) {
                    cp_state = 2;
                    nmi_rom_data[0] = 0;        // its own screen until a key is pressed
                    return;
                }
            }
            slot_msg = "No picture in that autosave";
        } else if (back) {
            cp_state = 0;
        }
        break;
    case 2:             // its screen: any key goes back to the list
        cp_state = 0;
        break;
    case 4: {           // the slot it goes to
        if (back) {
            cp_state = 0;
        } else if (n > 0) {
            char game[screengrab::NAME_MAX + 1];
            SlotEntry e[slots::COUNT];
            slot_pick = (uint8_t) n;
            if (slot_game_now(game) > 0) slots_scan(game, e);
            if (slot_game_now(game) > 0 && e[n - 1].used) {
                cp_state = 5;
            } else {
                cp_state = 0;
                slot_msg = checkpoint_to_slot(cp_pick, n);
            }
        }
        break;
    }
    case 5:             // "Save over slot n?"
        cp_state = 0;
        if (key == 'y' || key == 'Y') slot_msg = checkpoint_to_slot(cp_pick, slot_pick);
        break;
    case 6:             // "Clear the checkpoints?"
        cp_state = 0;
        if (key == 'y' || key == 'Y') slot_msg = checkpoints_erase_files() ? "Autosaves cleared" : "Could not erase them all";
        break;
    default:
        break;
    }
    count = rw_order(order);
    char game[screengrab::NAME_MAX + 1];
    slot_game_now(game);
    slots_banner(screen, "Autosave: ", game[0] != '\0' ? game : "no game");
    char t[40];
    if (count > 0) {
        for (int k = 0; k < count; k++) {
            unsigned age = ((uint16_t) (rw_ticks(rw.menu_ms) - rw.at_t[order[k]]) + 4u) / 8u;
            snprintf(t, sizeof(t), "%d", k + 1);
            help_put_text(screen, 2 + k, 1, t, 1, gameinfo::ATTR_HEADING);
            snprintf(t, sizeof(t), "%u seconds ago", age);
            help_put_text(screen, 2 + k, 3, t, (int) strlen(t), gameinfo::ATTR_TEXT);
        }
        if (!rw.on) help_put_text(screen, 9, 1, "Autosave is off for this game.", 30, gameinfo::ATTR_DIM);
    } else if (!rw.on) {
        help_put_text(screen, 2, 1, "Autosave is off for this game.", 30, gameinfo::ATTR_TEXT);
        if (cfg.checkpoint_s == 0) {
            help_put_text(screen, 4, 1, "Set it in O - Options (20, 40", 29, gameinfo::ATTR_DIM);
            help_put_text(screen, 5, 1, "or 60 s): each game loaded", 26, gameinfo::ATTR_DIM);
            help_put_text(screen, 6, 1, "then asks. O here turns it on", 29, gameinfo::ATTR_DIM);
            help_put_text(screen, 7, 1, "for this game (every 20 s).", 27, gameinfo::ATTR_DIM);
        }
    } else {
        help_put_text(screen, 2, 1, "No autosaves yet.", 17, gameinfo::ATTR_TEXT);
        snprintf(t, sizeof(t), "One is saved every %u seconds.", (unsigned) rw.secs);
        help_put_text(screen, 4, 1, t, (int) strlen(t), gameinfo::ATTR_DIM);
    }
    if (count > 0) {
        snprintf(t, sizeof(t), "1-%d", count);
        help_put_text(screen, 11, 1, t, (int) strlen(t), gameinfo::ATTR_KEYS);
        help_put_text(screen, 11, 5, "Select autosave", 15, gameinfo::ATTR_TEXT);
        help_put_text(screen, 12, 1, "V", 1, gameinfo::ATTR_KEYS);
        help_put_text(screen, 12, 5, "View an autosave's screen", 25, gameinfo::ATTR_TEXT);
        help_put_text(screen, 13, 1, "M", 1, gameinfo::ATTR_KEYS);
        help_put_text(screen, 13, 5, "Move one to a save slot", 23, gameinfo::ATTR_TEXT);
    }
    if (sd_card_mounted) {
        help_put_text(screen, 14, 1, "C", 1, gameinfo::ATTR_KEYS);
        help_put_text(screen, 14, 5, "Clear the game's autosaves", 26, gameinfo::ATTR_TEXT);
    }
    help_put_text(screen, 15, 1, "O", 1, gameinfo::ATTR_KEYS);
    help_put_text(screen, 15, 5, rw.on ? "Turn autosave off" : "Turn autosave on", rw.on ? 17 : 16, gameinfo::ATTR_TEXT);
#if PICOMENU_HISCORE
    help_put_text(screen, 16, 1, "T", 1, gameinfo::ATTR_KEYS);
    help_put_text(screen, 16, 5, "High score table", 16, gameinfo::ATTR_TEXT);
#endif
    help_put_text(screen, 18, 1, "X-Back", 6, gameinfo::ATTR_KEYS);
    const char* ask = nullptr;
    const char* keys = "X-Back";
    switch (cp_state) {
    case 1: snprintf(t, sizeof(t), "View which? 1-%d", count); ask = t; break;
    case 3: snprintf(t, sizeof(t), "Move which? 1-%d", count); ask = t; break;
    case 4: ask = "To which save slot? 1-9"; break;
    case 5: snprintf(t, sizeof(t), "Save over slot %d?", slot_pick); ask = t; keys = "Y-Yes  N-No"; break;
    case 6: ask = "Erase all of its autosaves?"; keys = "Y-Yes  N-No"; break;
    default: break;
    }
    if (ask != nullptr) {
        help_put_text(screen, 19, 1, ask, (int) strlen(ask), gameinfo::ATTR_HEADING);
        help_put_text(screen, 20, 1, keys, (int) strlen(keys), gameinfo::ATTR_KEYS);
    }
    if (slot_msg != nullptr) help_put_text(screen, 22, 1, slot_msg, (int) strlen(slot_msg), gameinfo::ATTR_TEXT);
    preview_screen = screen;
    nmi_rom_data[0] = 0;
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
        const uint8_t* glyph = menu_font()[(ch >= 32 && ch < 128 ? ch : '?') - 32];
        for (int line = 0; line < 8; line++) {
            screen[((row >> 3) << 11) | (line << 8) | ((row & 7) << 5) | (col + i)] = glyph[line];
        }
        screen[6144 + row * 32 + col + i] = ui_attr(attr);
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
    else if (is("sdfix")) {
        // what was put right on the card since power on (nothing: the line is left out)
        if (sd_read_retries != 0 || sd_read_failures != 0 || sd_remounts != 0) {
            snprintf(out, outsize, "Retries %lu Fails %lu Mounts %lu", (unsigned long) sd_read_retries,
                     (unsigned long) sd_read_failures, (unsigned long) sd_remounts);
        }
    }
    else if (is("autosave")) {
        // the pause the game had for the last autosave (since power on)
        if (cp_pause_us == 0) {
            snprintf(out, outsize, "None yet");
        } else {
#if PICOMENU_CP_OLD
            snprintf(out, outsize, "%s old: %lu ms, max %lu ms", cp_pause_128 ? "128K" : "48K",
#else
            snprintf(out, outsize, "%s: %lu ms, longest %lu ms", cp_pause_128 ? "128K" : "48K",
#endif
                     (unsigned long) ((cp_pause_us + 500) / 1000), (unsigned long) ((cp_pause_max_us + 500) / 1000));
        }
    }
    else if (is("ini1")) {
        settings::describe(cfg, out, outsize);
    }
    else if (is("ini2")) {
        settings::describe_bad(cfg, out, outsize);
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

static const uint32_t HELP_TEXT_OFFSET = 98304;     // in snap_load_buffer (Help is open: nothing else is there)
static_assert(HELP_TEXT_OFFSET + HELP_TEXT_SIZE <= SNAP_LOAD_BUFFER_SIZE, "the Help text fits");
static void help_draw_page(uint8_t* screen, int page);
static void nmi_action_help_page() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    int page = nmi_rom_data[2];
    if (page >= HELP_PAGE_COUNT) page = HELP_PAGE_COUNT - 1;
    help_draw_page(reinterpret_cast<uint8_t*>(snap_load_buffer), page);
    preview_screen = reinterpret_cast<uint8_t*>(snap_load_buffer);
    nmi_rom_data[0] = HELP_PAGE_COUNT;
}

// A page of Docs/Help.txt (now only Diagnostics, which D on Options shows) drawn on screen
static void help_draw_page(uint8_t* screen, int page) {
    memset(screen, 0, 6144);
    memset(screen + 6144, ui_attr(0x07), 768);                 // white ink, black paper, as the menu
    const uint8_t BANNER = 0x4F, TEXT = 0x47, HEADING = 0x46, KEYS = 0x45;   // bright
    // title row
    memset(screen + 6144, BANNER, 32);
    // the pages, packed in the firmware, unpacked into a part of the buffer nothing keeps
    // anything in while Help is open
    char* all = snap_load_buffer + HELP_TEXT_OFFSET;
    lzss_unpack(help_packed, reinterpret_cast<uint8_t*>(all), HELP_TEXT_SIZE);
    struct { const char* title; const char* text; } hp;
    const char* at = all;
    for (int i = 0; i < page; i++) {
        at += strlen(at) + 1;           // the title
        at += strlen(at) + 1;           // the text
    }
    hp.title = at;
    hp.text = at + strlen(at) + 1;
    help_put_text(screen, 0, 1, hp.title, strlen(hp.title), BANNER);
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
    // (each page is shown on its own: Diagnostics, the last, from Options; the hotkeys' list,
    // the first, with the button and SPACE)
    const char* keys = page == HELP_PAGE_COUNT - 1 ? "Any key: back to Options" : "Any key: back to the game";
    help_put_text(screen, 23, 1, keys, strlen(keys), KEYS);
}

// The poke lists: the .pok file in the Poke List folder of the SD card, named like the game
// last loaded (or like its name without " Save Game"), read when the list is first asked for
// Also the converter's working space while a tape is converted (C): see conv_state. It is
// read again whenever the poke list is opened.
alignas(8) static extras::PokFile pok_file;

static const char POKE_LIST_DIR[] = "/Poke List";     // in the root of the SD card

// Looks for <name>.pok (.POK too) in the Poke List folder, for the game last loaded (or its name
// without " Save Game"). True if there is one: its path in path.
static bool pok_find(char* path, size_t size) {
    if (!reload_known || !sd_card_mounted) {
        return false;
    }
    size_t full = strlen(reload_name);
    size_t shorter = extras::base_name_length(reload_name);
    for (int attempt = 0; attempt < 2; attempt++) {
        int n = (int) (attempt == 0 ? full : shorter);
        if (attempt == 1 && shorter == full) {
            break;
        }
        for (int upper = 0; upper < 2; upper++) {
            snprintf(path, size, "%s/%.*s.%s", POKE_LIST_DIR, n, reload_name, upper ? "POK" : "pok");
            if (fsops::kind(Drive::EXTERNAL_SD_CARD, path) == fsops::KIND_FILE) {
                return true;
            }
        }
    }
    return false;
}

// Reads the game's .pok (pok_find). Returns the number of cheats.
static int pok_load_for_last_game() {
    pok_file.clear();
    static char path[BROWSE_PATH_MAX];
    if (!pok_find(path, sizeof(path))) {
        return 0;
    }
    FileHandle f;
    int cheats = 0;
    if (f.open(Drive::EXTERNAL_SD_CARD, path, true)) {
        cheats = pok_file.load(f);
        f.close();
    }
    LOG("%s: %d cheats\n", path, cheats);
    return cheats;
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
static const uint32_t INFO_DATA_OFFSET = 23296;             // (instructions up to 8 KB: gameinfo::TEXT_MAX)
static const uint32_t GRAB_SCR_OFFSET = 34816;
static const uint32_t PNG_SCRATCH_OFFSET = 73728;       // reading a .png grab (menushot::read_png)
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

// The screen grabs of the game ACTION_SNAP_PREVIEW was asked about, in the order V shows them:
// by category (in the order of /Scraper/Screen Grabs/order, see screengrab.h), then by number.
// They are the .scr files named after the game in /Scraper/Screen Grabs (the loading screens the
// scraper downloads, and the grabs made here), then any left in /Screen Grabs, where grabs were
// saved before, as Standard Screen Number.
struct GrabEntry {
    uint16_t key;           // the rank of its category, its folder, its number: smaller is shown first
    uint8_t cat;            // screengrab::Category
    uint8_t from;           // bit 0: 0 SCRAPER_FOLDER, 1 OLD_FOLDER; bit 1: a .png (no .scr of the name)
    uint8_t number;
};
static int grab_count = 0;
static GrabEntry grab_list[screengrab::MAX_PICTURES];
static const char* grab_folder(int k) { return (grab_list[k].from & 1) == 0 ? screengrab::SCRAPER_FOLDER : screengrab::OLD_FOLDER; }

// The path of grab k of the game (base) into grab_path
static void grab_path_of(int k, const char* base) {
    screengrab::grab_path(grab_folder(k), base, (screengrab::Category) grab_list[k].cat, grab_list[k].number, grab_path,
        sizeof(grab_path), (grab_list[k].from & 2) ? "png" : "scr");
}

// The order file (the default order if there is none), read a piece at a time into grab_path
// Gives each line of an open file to line(text, length), through grab_path. A line longer than
// grab_path is dropped; a file over 64 times its size is not read to the end. A \r\n is one
// line end (so the line numbers are right), a \r or \n alone is one too.
template <class Line>
static void read_text_lines(FileHandle& f, Line line) {
    char* buf = grab_path;
    const size_t cap = sizeof(grab_path);
    size_t have = 0;
    bool after_cr = false;          // the last piece ended with a \r: a \n first in this one goes with it
    for (int pieces = 0; pieces < 64; pieces++) {
        int got = f.read(buf + have, cap - have);
        bool end = got <= 0;
        if (!end) have += (size_t) got;
        size_t i = 0;
        if (after_cr && have > 0 && buf[0] == '\n') i = 1;
        after_cr = false;
        for (;;) {
            size_t e = i;
            while (e < have && buf[e] != '\n' && buf[e] != '\r') e++;
            if (e == have) {                            // the rest of the line is still to come
                if (end && e > i) line(buf + i, e - i);
                break;
            }
            line(buf + i, e - i);
            if (buf[e] == '\r') {
                if (e + 1 < have) {
                    if (buf[e + 1] == '\n') e++;
                } else {
                    after_cr = true;
                }
            }
            i = e + 1;
        }
        if (end) break;
        if (i == 0 && have == cap) i = cap;             // a line as long as the buffer: dropped
        memmove(buf, buf + i, have - i);
        have -= i;
    }
}

// Reads Picomenu.ini: the SD card's, or with none there the internal flash's. A setting it
// does not have, or does not understand, is the default.
__attribute__((noinline)) static void settings_load() {
    cfg_stale = false;
    cfg = settings::Settings();
    char folder[sizeof(sd_cwd)];
    folder[0] = '\x01';            // the file has no start_folder
    settings::Parser parser(cfg, folder, sizeof(folder));
    FileHandle f;
    if (sd_card_mounted && f.open(Drive::EXTERNAL_SD_CARD, settings::FILE_NAME, true)) {
        cfg.source = 1;
    } else if (f.open(Drive::INTERNAL_FLASH, settings::FILE_NAME, true)) {
        cfg.source = 2;
    }
    if (cfg.source != 0) {
        read_text_lines(f, [&](const char* t, size_t n) { parser.line(t, n); });
        f.close();
    }
    if (cfg_folder_due) {
        cfg_folder_due = false;
        if (folder[0] != '\x01' && sd_card_mounted) {
            if (folder[0] == '\0' || fsops::kind(Drive::EXTERNAL_SD_CARD, folder) == fsops::KIND_DIR) {
                walk_stop();
                snprintf(sd_cwd, sizeof(sd_cwd), "%s", folder);
            } else {
                settings::missing_folder(cfg);
            }
        }
    }
    LOG("Picomenu.ini: %s, %u settings, %u not understood (first: line %u)\n",
        cfg.source == 1 ? "SD card" : cfg.source == 2 ? "flash" : "none", (unsigned) cfg.good,
        (unsigned) cfg.bad, (unsigned) cfg.bad_line);
}

static void settings_fresh() {
    if (cfg_stale) settings_load();
}

// ---- F in Tools: file transfer over WiFi (a Pico W: wifi.cxx, webfiles.h) ----
// The browser's requests are answered by web::Conn, which reads and writes the SD card with
// FatFs and erases and renames through these, as Browse Games does (the Waste Bin, tape notes).
// While the screen is open, snap_load_buffer is lent: the screen at its start, WebState after
// it, and the rest (from WEB_ARENA_AT) to the WiFi (lwIP and the connections).

static bool web_card_ok() {
    return sd_card_mounted && !usb_msc_pc_holds_card();
}

static bool web_erase(const char* from, bool is_dir) {
    char* path = erase_path;
    const size_t path_size = sizeof(erase_path);
    if (strlen(from) >= path_size) return false;
    snprintf(path, path_size, "%s", from);
    walk_stop();
    tape_eject_if_in(1, path);
    char* scratch = erase_scratch;
    const size_t scratch_size = sizeof(erase_scratch);
    BinResult bin = in_waste_bin(1, path) ? BIN_NONE : to_waste_bin(1, path, is_dir, scratch, scratch_size);
    if (bin == BIN_OK) {
        if (is_dir) tape_notes_follow_folder(1, path, scratch, false);
        else tape_note_follow(1, path, 1, scratch, false);
        tape_notes_retarget(1, path, 1, scratch, is_dir);
        return true;
    }
    if (bin == BIN_FAILED) return false;
    bool ok = is_dir ? fsops::remove_tree(Drive::EXTERNAL_SD_CARD, path, path_size, find_dirs, 0, ERASE_MAX_DEPTH,
        scratch, scratch_size) : fsops::remove_path(Drive::EXTERNAL_SD_CARD, path);
    if (ok) {
        if (is_dir) tape_notes_follow_folder(1, path, nullptr, false);
        else tape_note_follow(1, path, 1, nullptr, false);
        tape_labels_follow(1, path, 1, nullptr, is_dir);
    }
    return ok;
}

static bool web_rename(const char* from, const char* to, bool is_dir) {
    walk_stop();
    tape_eject_if_in(1, from);
    if (fsops::rename_path(Drive::EXTERNAL_SD_CARD, from, to) != FILEOP_OK) return false;
    if (is_dir) tape_notes_follow_folder(1, from, to, false);
    else tape_note_follow(1, from, 1, to, false);
    tape_notes_retarget(1, from, 1, to, is_dir);
    return true;
}

// What the screen shows, kept in snap_load_buffer after the screen
struct WebState {
    settings::Settings scratch;         // (reading Picomenu.ini's [WiFi] again needs one)
    settings::Wifi wifi;
    char last[40];                      // the last thing done, "Received Exolon.z80"
    uint16_t sent, received, other;
    uint8_t shown_state;                // what the screen showed: the state, and whether it has changed since
    bool changed;
    bool open;
    bool bt;                            // (the standard build) opened as B - Bluetooth tape
    bool pok;                           // (the standard build) opened as Z on the menu or the Poke screen
    const char* problem;                // why it can't start (shown instead)
};
static const uint32_t WEB_STATE_AT = 6912;
static const uint32_t WEB_ARENA_AT = 8192;
// The end of what the radio's screens may use of snap_load_buffer: in the Pico W build its last
// 24 KB hold the radios' variables (BTstack's, the ZXDB downloads': memmap_picomenu_w.ld)
#ifdef PICOMENU_BT_SHARED
static const uint32_t RADIO_ARENA_END = SNAP_LOAD_BUFFER_SIZE - PICOMENU_BT_SHARED;
#else
static const uint32_t RADIO_ARENA_END = SNAP_LOAD_BUFFER_SIZE;
#endif
static_assert(WEB_STATE_AT + sizeof(WebState) <= WEB_ARENA_AT, "the file transfer's state fits before its arena");
static WebState& web_state() { return *reinterpret_cast<WebState*>(reinterpret_cast<uint8_t*>(snap_load_buffer) + WEB_STATE_AT); }

static bool web_ini_changed = false;      // Picomenu.ini came from the browser

// Picomenu.ini from the browser is used at once, as the Spectrum's own editor's save of it is:
// the save type and theme now, Browse Games' start from the menu's next opening
static void web_ini_apply() {
    if (!web_ini_changed) return;
    web_ini_changed = false;
    walk_stop();
    settings_load();
    uint8_t* rom = rom_data + 16384;
    rom[START_SAVETYPE] = cfg.save_type;
    rom[START_THEME] = cfg.light ? 1 : 0;
    cfg_stale = true;
    cfg_folder_due = true;
    LOG("Picomenu.ini from the browser: used\n");
}

static void web_event(char what, const char* path) {
    WebState& w = web_state();
    const char* verb = what == 'S' ? "Sent" : what == 'R' ? "Received" : what == 'E' ? "Erased" : what == 'N' ? "Renamed" : "Made";
    if (what == 'S') w.sent++;
    else if (what == 'R') w.received++;
    else w.other++;
    snprintf(w.last, sizeof(w.last), "%s %s", verb, web::base_of(path));
    w.changed = true;
    // Picomenu.ini sent (Edit on the page, or a file put over it) or renamed to it: used as the
    // screen closes (web_ini_apply), and read again before it is next used
    if ((what == 'R' || what == 'N') && strcasecmp(path, settings::FILE_NAME) == 0) {
        web_ini_changed = true;
        cfg_stale = true;
    }
    LOG("Web: %s %s\n", verb, path);
}

static const web::Hooks WEB_HOOKS = { web_card_ok, web_erase, web_rename, web_event };

// The WiFi is on (its memory is snap_load_buffer's): it is stopped as soon as anything else
// could use that memory, whatever the way the screen was left (the Spectrum reset, say)
static bool web_on = false;
static void web_off() {
#if PICOMENU_NET
    net::stop();                        // (its connection and TLS memory are lwIP's)
    zx_live = false;
#endif
#ifdef WIFI_ENABLE
    if (web_on) wifi::stop();
#endif
    web_on = false;
}

// Something is being sent or received: the main loop does not sleep
static bool web_busy() {
#ifdef WIFI_ENABLE
    return wifi::state() != wifi::OFF;
#else
    return false;
#endif
}

// Picomenu.ini's [WiFi], read again
static void web_read_ini(WebState& w) {
    memset(&w.wifi, 0, sizeof(w.wifi));
    new (&w.scratch) settings::Settings();
    settings::Parser parser(w.scratch);
    parser.wifi = &w.wifi;
    FileHandle f;
    if ((sd_card_mounted && f.open(Drive::EXTERNAL_SD_CARD, settings::FILE_NAME, true)) ||
            f.open(Drive::INTERNAL_FLASH, settings::FILE_NAME, true)) {
        read_text_lines(f, [&](const char* t, size_t n) { parser.line(t, n); });
        f.close();
    }
}

static void web_draw(uint8_t* screen) {
    WebState& w = web_state();
    slots_banner(screen, "", w.bt ? "Bluetooth tape" : w.pok ? "ZXDB: Spectrum Computing" : "File transfer (WiFi)");
    char t[48];
    int row = 2;
    auto text = [&](int r, const char* s, uint8_t attr) { help_put_text(screen, r, 1, s, (int) strlen(s), attr); };
    if (w.problem != nullptr) {
        // why it can't (the lines of the text are split at '\n')
        const char* p = w.problem;
        while (*p != '\0' && row < 20) {
            const char* nl = strchr(p, '\n');
            size_t n = nl ? (size_t) (nl - p) : strlen(p);
            help_put_text(screen, row++, 1, p, (int) n, row == 3 ? gameinfo::ATTR_TEXT : gameinfo::ATTR_DIM);
            p = nl ? nl + 1 : p + n;
        }
    } else {
#ifdef WIFI_ENABLE
        wifi::State st = wifi::state();
        if (st == wifi::JOINING) {
            snprintf(t, sizeof(t), "Joining %.21s ...", w.wifi.ssid);
            text(2, t, gameinfo::ATTR_TEXT);
        } else if (st == wifi::FAILED) {
            snprintf(t, sizeof(t), "Could not join %.15s:", w.wifi.ssid);
            text(2, t, gameinfo::ATTR_TEXT);
            text(3, wifi::why(), gameinfo::ATTR_HEADING);
            text(5, "Check ssid and password in", gameinfo::ATTR_DIM);
            text(6, "[WiFi] of Picomenu.ini.", gameinfo::ATTR_DIM);
        } else if (st == wifi::UP) {
            snprintf(t, sizeof(t), "On %.27s", w.wifi.ssid);
            text(2, t, gameinfo::ATTR_TEXT);
            text(4, "On a PC or phone on the same", gameinfo::ATTR_DIM);
            text(5, "network, open in a browser:", gameinfo::ATTR_DIM);
            snprintf(t, sizeof(t), "http://%s/", wifi::name());
            text(7, t, gameinfo::ATTR_HEADING);
            snprintf(t, sizeof(t), "http://%s.local", wifi::name());
            text(8, t, gameinfo::ATTR_HEADING);
            snprintf(t, sizeof(t), "http://%s", wifi::address());
            text(9, t, gameinfo::ATTR_HEADING);
            text(10, "(whichever the browser finds)", gameinfo::ATTR_DIM);
            if (w.wifi.web_password[0] != '\0') {
                text(11, "Password: web_password (any", gameinfo::ATTR_DIM);
                text(12, "user name)", gameinfo::ATTR_DIM);
            }
            snprintf(t, sizeof(t), "Sent %u  Received %u", (unsigned) w.sent, (unsigned) w.received);
            text(14, t, gameinfo::ATTR_TEXT);
            if (w.last[0] != '\0') text(15, w.last, gameinfo::ATTR_DIM);
            text(17, "Z-Search ZXDB (Spectrum", gameinfo::ATTR_KEYS);
            text(18, "  Computing) for a game", gameinfo::ATTR_KEYS);
        }
#endif
        text(20, "The Spectrum waits here.", gameinfo::ATTR_DIM);
    }
    text(22, "X-Stop and go back", gameinfo::ATTR_KEYS);
    preview_screen = screen;
}

// The screens of the Pico W firmware only, in the standard one
#define NEEDS_PICO_W "This needs a Pico W, with the\nPico W firmware of PicoMenu\n(picomenu-...-W.uf2) on it."

#if PICOMENU_NET
static uint8_t zx_key(uint8_t key);
static void zx_draw(uint8_t* screen);
static void zx_begin(const char* game, bool pok = false, bool solo = false);
static void zx_pok_begin();
static bool zx_alone_live();
static bool zx_wifi_up();
static bool zx_typing();
#endif

// ACTION_WIFI: BYTE param1 = the key (0: opened; 255: none, has the screen changed?).
// Status 0 as it is, 1 closed, 2 shown again. Z (once the WiFi is up): ZXDB's search, its own
// screens until they are left. Opened with 0xFC (Z on the menu) or 0xFD (Z on the Poke
// screen): ZXDB's screens alone, the WiFi started for them and stopped after (status 1 then)
__attribute__((noinline)) static void nmi_action_wifi() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t key = nmi_rom_data[2];
#if PICOMENU_BT
    // B - Bluetooth tape comes the same way, opened with 0xFE
    if (key == 0xFE || (key != 0 && bt_screen)) {
        nmi_action_bt(key);
        return;
    }
    if (key == 0) bt_off();             // one radio at a time
#endif
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer);
    WebState& w = web_state();
#if PICOMENU_NET
    if (key == 0xFD || key == 0xFC) {   // Z on the Poke screen: ZXDB's pokes for the game; Z on the menu: its search
        if (key == 0xFD) zx_pok_begin();
        else zx_begin(nullptr, false, true);
        zx_draw(screen);
        nmi_rom_data[0] = 2;
        return;
    }
#endif
#if !PICOMENU_BT
    if (key >= 0xFC && key <= 0xFE) {   // B - Bluetooth tape, Z (the menu, the Poke screen): not in this build
        memset(&w, 0, sizeof(w));
        w.bt = key == 0xFE;
        w.pok = key != 0xFE;
        w.problem = NEEDS_PICO_W;
        web_draw(screen);
        nmi_rom_data[0] = 2;
        return;
    }
#endif
    if (key == 0) {
#if PICOMENU_NET
        zx_live = false;
#endif
        memset(&w, 0, sizeof(w));
        w.open = true;
        walk_stop();
        web_read_ini(w);
#ifndef WIFI_ENABLE
        w.problem = NEEDS_PICO_W;
#else
        if (!web_card_ok()) {
            w.problem = "There is no SD card (or the PC\nhas it as a USB drive).";
        } else if (w.wifi.ssid[0] == '\0') {
            w.problem = "Put the WiFi's name and password\nin Picomenu.ini on the SD card:\n\n[WiFi]\nssid = MyNetwork\npassword = secret\n\nand, to ask for one on the page:\nweb_password = another";
        } else {
#if PICOMENU_NET
            net::stop();                // (nothing of HTTPS's is left in lwIP as it starts again)
#endif
            web_on = true;
            const char* why = wifi::start(w.wifi.ssid, w.wifi.password, w.wifi.web_password,
                reinterpret_cast<uint8_t*>(snap_load_buffer) + WEB_ARENA_AT, RADIO_ARENA_END - WEB_ARENA_AT, &WEB_HOOKS);
            if (why != nullptr) LOG("WiFi: %s\n", why);
        }
        w.shown_state = (uint8_t) wifi::state();
#endif
        web_draw(screen);
        nmi_rom_data[0] = 2;
        return;
    }
#if PICOMENU_NET
    if (zx_live) {
        const bool alone = zx_alone_live();
        uint8_t r = zx_key(key == 255 ? 0 : key);
        nmi_rom_data[START_KEYREPEAT] = zx_typing() ? 1 : 0;   // (typing: a held key repeats)
        if (r == 1 && alone) {
            nmi_rom_data[0] = 1;            // back to the Poke screen or the menu (the WiFi is off: zx_info_end)
            return;
        }
        if (r == 1) {
            w.changed = false;
            web_draw(screen);               // the WiFi screen again
            nmi_rom_data[0] = 2;
            return;
        }
        if (r == 2) zx_draw(screen);
        nmi_rom_data[0] = r;
        return;
    }
    if ((key == 'z' || key == 'Z') && zx_wifi_up() && web_card_ok()) {
        zx_begin(nullptr);
        zx_draw(screen);
        nmi_rom_data[START_KEYREPEAT] = 1;  // (typing the search)
        nmi_rom_data[0] = 2;
        return;
    }
#endif
    if (is_key_break(key) || key == 'x' || key == 'X') {
        web_off();
        w.open = false;
        web_ini_apply();
        nmi_rom_data[0] = 1;
        return;
    }
    bool redraw = w.changed;
#ifdef WIFI_ENABLE
    redraw = redraw || w.shown_state != (uint8_t) wifi::state();
    w.shown_state = (uint8_t) wifi::state();
#endif
    w.changed = false;
    if (redraw) web_draw(screen);
    nmi_rom_data[0] = redraw ? 2 : 0;
}

#ifdef PICOFACE_HOST
// The simulator's "http": a request (all of it) answered as a browser connection would be,
// into out. The connection is in snap_load_buffer, as on the Pico W.
void host_http(const char* request, size_t n, std::string& out) {
    WebState& w = web_state();
    memset(&w, 0, sizeof(w));
    web_read_ini(w);
    web::Conn* c = new (reinterpret_cast<uint8_t*>(snap_load_buffer) + WEB_ARENA_AT) web::Conn();
    c->begin(&WEB_HOOKS, w.wifi.web_password);
    walk_stop();
    // fed in pieces, as TCP gives them
    for (size_t i = 0; i < n; i += 700) c->feed(reinterpret_cast<const uint8_t*>(request) + i, n - i < 700 ? n - i : 700);
    out.clear();
    for (int guard = 0; guard < 100000 && !c->finished(); guard++) {
        const uint8_t* p;
        size_t k = c->ready(&p);
        if (k > 1000) k = 1000;
        out.append(reinterpret_cast<const char*>(p), k);
        c->sent(k);
        if (k == 0 && !c->finished()) break;    // (a request not finished: it waits for more)
    }
    if (!c->finished()) c->abort();
}
#endif


// The order V shows a game's pictures in: Picomenu.ini's [Screen grab order], or with no such
// section the order file (order.txt, or order) in /Scraper/Screen Grabs
__attribute__((noinline)) static void grabs_order(screengrab::Order& order) {
    if (cfg.has_order) {        // (read again, if need be, as the menu opened: sendNmiRequest)
        order = cfg.order;
        order.finish();
        return;
    }
    FileHandle f;
    if (f.open(Drive::EXTERNAL_SD_CARD, screengrab::ORDER_FILE, true) ||
            f.open(Drive::EXTERNAL_SD_CARD, screengrab::ORDER_FILE_BARE, true)) {
        read_text_lines(f, [&](const char* t, size_t n) { order.line(t, n); });
        f.close();
    }
    order.finish();
}

// file_picture: the game's file has a screen of its own (From File), which goes in the list too
__attribute__((noinline)) static void grabs_find(const char* game, bool file_picture) {
    grab_count = 0;
    char base[screengrab::NAME_MAX + 1];
    const bool sd = sd_card_mounted && screengrab::base_name(game, base) > 0;
    screengrab::Order order;
    if (sd) {
        grabs_order(order);
    } else {
        order.finish();                 // no card: the file's own picture, as before
    }
    if (file_picture && order.shown[screengrab::FROM_FILE]) {
        grab_list[grab_count++] = { (uint16_t) (order.rank[screengrab::FROM_FILE] << 9), screengrab::FROM_FILE, 0, 0 };
    }
    if (!sd) {
        return;
    }
    for (uint8_t from = 0; from < 2; from++) {
        snprintf(grab_path, sizeof(grab_path), "/sd%s", from == 0 ? screengrab::SCRAPER_FOLDER : screengrab::OLD_FOLDER);
        if (!grab_dir.open(grab_path)) {
            continue;                   // no folder
        }
        do {
            if (grab_dir.atEnd()) break;
            screengrab::Category cat;
            int number;
            bool png = false;
            if (grab_dir.isDirectory() || !screengrab::parse_name(grab_dir.fileName(), base, &cat, &number, &png) || number > 255 ||
                    !order.shown[cat]) {
                continue;
            }
            uint16_t key = (uint16_t) ((order.rank[cat] << 9) | (from << 8) | number);
            // the same grab as a .scr and a .png (grab_format = both): shown once, from the .scr
            int same = -1;
            for (int k = 0; k < grab_count; k++) if (grab_list[k].key == key) same = k;
            if (same >= 0) {
                if (!png) grab_list[same].from &= 1;
                continue;
            }
            int at = grab_count;
            while (at > 0 && grab_list[at - 1].key > key) at--;
            if (at >= screengrab::MAX_PICTURES) continue;       // more than V shows: the last are left out
            int last = grab_count < screengrab::MAX_PICTURES ? grab_count : screengrab::MAX_PICTURES - 1;
            for (int k = last; k > at; k--) grab_list[k] = grab_list[k - 1];
            grab_list[at] = { key, (uint8_t) cat, (uint8_t) (from | (png ? 2 : 0)), (uint8_t) number };
            if (grab_count < screengrab::MAX_PICTURES) grab_count++;
        } while (grab_dir.next());
        grab_dir.close();
    }
    LOG("%d screen grabs of %s\n", grab_count, game);
}

__attribute__((noinline)) static bool grab_load(const char* game, int k, uint8_t* dest) {
    char base[screengrab::NAME_MAX + 1];
    if (k < 0 || k >= grab_count) {
        return false;
    }
    if (grab_list[k].cat == screengrab::FROM_FILE) {
        if (info_source == nullptr) return false;
        if (dest != info_source) memcpy(dest, info_source, screengrab::SCR_SIZE);
        return true;
    }
    if (screengrab::base_name(game, base) == 0) {
        return false;
    }
    grab_path_of(k, base);
    FileHandle f;
    if (!f.open(Drive::EXTERNAL_SD_CARD, grab_path, true)) {
        return false;
    }
    if (grab_list[k].from & 2) {
        // a .png PicoMenu saved: read back into a screen, through room in snap_load_buffer
        bool ok = menushot::read_png(f, dest, reinterpret_cast<uint8_t*>(snap_load_buffer) + PNG_SCRATCH_OFFSET);
        f.close();
        return ok;
    }
    int got = f.read(dest, screengrab::SCR_SIZE);
    f.close();
    return got == (int) screengrab::SCR_SIZE;
}

// The lowest number not taken for a new grab of base in category cat, in /Scraper/Screen Grabs
// (as a .scr or a .png), or -1 if all are
static int grab_free_number(const char* base, screengrab::Category cat) {
    for (int i = 0; i < screengrab::MAX_SCAN; i++) {
        screengrab::grab_path(screengrab::SCRAPER_FOLDER, base, cat, i, grab_path, sizeof(grab_path), "png");
        if (fsops::kind(Drive::EXTERNAL_SD_CARD, grab_path) != fsops::KIND_NONE) continue;
        screengrab::grab_path(screengrab::SCRAPER_FOLDER, base, cat, i, grab_path, sizeof(grab_path));
        if (fsops::kind(Drive::EXTERNAL_SD_CARD, grab_path) == fsops::KIND_NONE) {
            return i;
        }
    }
    return -1;
}

struct GrabSink {
    FileHandle* f;
    bool write(const void* p, size_t n) { return f->write(p, n) == (int) n; }
};

// Saves the screen as grab number of base in category cat, as Picomenu.ini's grab_format says:
// a .scr, a .png, or both. False if one could not be saved (it is removed).
__attribute__((noinline)) static bool grab_write(FileHandle& f, const char* base, screengrab::Category cat, int number,
                                                 const uint8_t* scr) {
    for (int png = 0; png < 2; png++) {
        if (png ? cfg.grab_format == settings::GRAB_SCR : cfg.grab_format == settings::GRAB_PNG) continue;
        screengrab::grab_path(screengrab::SCRAPER_FOLDER, base, cat, number, grab_path, sizeof(grab_path), png ? "png" : "scr");
        bool ok = f.open(Drive::EXTERNAL_SD_CARD, grab_path, false);
        if (ok) {
            if (png) {
                GrabSink sink{ &f };
                ok = menushot::write_png(sink, scr);
            } else {
                ok = f.write(scr, screengrab::SCR_SIZE) == (int) screengrab::SCR_SIZE;
            }
            ok = f.close() && ok;
        }
        if (!ok) {
            fsops::remove_path(Drive::EXTERNAL_SD_CARD, grab_path);
            return false;
        }
    }
    return true;
}

// Removes grab number of base (its .scr and its .png)
static void grab_remove(const char* base, screengrab::Category cat, int number) {
    for (int png = 0; png < 2; png++) {
        screengrab::grab_path(screengrab::SCRAPER_FOLDER, base, cat, number, grab_path, sizeof(grab_path), png ? "png" : "scr");
        fsops::remove_path(Drive::EXTERNAL_SD_CARD, grab_path);
    }
}

// Makes /Scraper/Screen Grabs if it is not there: false if it can't be
static bool grab_folder_make() {
    if (fsops::kind(Drive::EXTERNAL_SD_CARD, screengrab::SCRAPER_FOLDER) == fsops::KIND_DIR) {
        return true;
    }
    uint8_t made = make_sd_folder("/Scraper");
    if (made == fsops::OK || made == fsops::NAME_IN_USE) made = make_sd_folder(screengrab::SCRAPER_FOLDER);
    return made == fsops::OK || made == fsops::NAME_IN_USE;
}

// ---- My notes: <game>.txt in the hidden Game Notes folder (notes.h)
// The editor and the bytes of a file are kept in snap_load_buffer above the screen grab
static const uint32_t NOTES_EDITOR_OFFSET = 41984;
static_assert(GRAB_SCR_OFFSET + screengrab::SCR_SIZE <= NOTES_EDITOR_OFFSET, "the screen grab is below the notes");
static const uint32_t NOTES_FILE_OFFSET = NOTES_EDITOR_OFFSET + 1024;
static_assert(sizeof(notes::Editor) <= 1024, "the notes editor does not fit");
static_assert(NOTES_FILE_OFFSET + notes::FILE_MAX + 2 <= NOTES_EDITOR_OFFSET + 2048, "the notes file does not fit");
static_assert(gameinfo::TAPE_MAX + 2 <= 1024, "the tape summary is made in the notes file's place");
// The SD card's file handle is big and core 0's stack is only 2K, so the notes' handle is
// kept in the buffer too (never on the stack: the first save hung the Spectrum that way)
static const uint32_t NOTES_HANDLE_OFFSET = NOTES_EDITOR_OFFSET + 2048;
static_assert(NOTES_HANDLE_OFFSET + sizeof(FileHandle) <= 65536, "the notes use the recent games' room");
static_assert(INFO_DATA_OFFSET >= INFO_SCREEN_OFFSET + 6912, "the information is after its screen");
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
// Y the year, G the genre, S your own score (out of 10), C the controls, I the instructions, R
// takes back what was typed in. Each of H Y G S C I is edited in a box of its own, with a limit,
// and saved on its own.
using DetEditorBig = notes::EditorT<96, 30>;            // the controls and the instructions
using DetEditorOne = notes::EditorT<1, 30>;             // the software house, year, genre, score
static const uint32_t DETAILS_OFFSET = 45056;           // the Details of the game
static const uint32_t DET_EDITOR_OFFSET = DETAILS_OFFSET + 10240;
static const uint32_t DET_FILE_OFFSET = DET_EDITOR_OFFSET + 3584;
static const size_t DET_FILE_MAX = 9216;
static_assert(sizeof(gameinfo::Details) <= 10240, "the details do not fit");
static_assert(sizeof(DetEditorBig) <= 3584, "the details editor does not fit");
static_assert(NOTES_HANDLE_OFFSET + sizeof(FileHandle) <= DETAILS_OFFSET, "the details would cover the notes' file handle");
// (past 65536 is the recent games' list, read again each time it is used, and the .png
// grab's working space, used only while the information is being loaded)
static_assert(DET_FILE_OFFSET + DET_FILE_MAX <= PNG_SCRATCH_OFFSET, "the details' file fits");
static_assert(gameinfo::TEXT_MAX + gameinfo::CONTROLS_MAX + 200 <= DET_FILE_MAX, "a details file does not fit its buffer");
enum EditKind : uint8_t { EK_HUB, EK_NOTES, EK_FIELD };
#if PICOMENU_NET
static bool zx_due = false;                             // Z in the M menu: look the game up in ZXDB
#endif
static uint8_t edit_kind = EK_HUB;                      // which screen is up while notes_editing
static char det_field = 0;                              // h y g s c or i, while EK_FIELD
// The one-line fields (H Y G S): which of the details' parts (gameinfo::META_HOUSE ...), or -1
static int det_meta(char field) {
    static const char ONE[] = "hygs";
    const char* p = field != 0 ? strchr(ONE, field) : nullptr;
    return p != nullptr ? (int) (p - ONE) : -1;
}
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
    int f = det_meta(field);
    if (f >= 0) return info->meta(f);
    return field == 'c' ? info->controls : info->text;
}

// Starts the box for a field. False (with a message) if what is there does not fit it.
NOTES_FUNC static bool det_open_field(const gameinfo::Info* info, char field) {
    const char* cur = det_current(info, field);
    char* file = det_file();
    int f = det_meta(field);
    if (f >= 0) {
        DetEditorOne* one = new (snap_load_buffer + DET_EDITOR_OFFSET) DetEditorOne();
        // (the house and genre: the box's 30)
        one->set_limit(f == gameinfo::META_YEAR || f == gameinfo::META_SCORE ? gameinfo::META_MAX[f] + 2 : 0);
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
    int f = det_meta(field);
    if (f >= 0) {
        DetEditorOne* one = reinterpret_cast<DetEditorOne*>(snap_load_buffer + DET_EDITOR_OFFSET);
        size_t n = one->text(file, DET_FILE_MAX);
        if (n > 0 && file[n - 1] == '\n') {
            file[--n] = '\0';
        }
        // (0 to 10)
        const bool digits = file[0] >= '0' && file[0] <= '9' && (n == 1 || (file[1] >= '0' && file[1] <= '9'));
        if (f == gameinfo::META_SCORE && n > 0 && !(digits && (n == 1 || (file[0] == '1' && file[1] == '0')))) {
            notes_message = "The score is a number, 0 to 10";
            return false;
        }
        d->set_meta(f, file);
    } else {
        DetEditorBig* big = reinterpret_cast<DetEditorBig*>(snap_load_buffer + DET_EDITOR_OFFSET);
        big->text(file, DET_FILE_MAX);
        if (field == 'c') d->set_controls(file);
        else d->set_text(file);
    }
    if (!det_write(game, d)) {
        return false;
    }
    info->apply(*d);                // what is on the screen from now on
    return true;
}

NOTES_FUNC static void det_hub_draw(uint8_t* screen, const gameinfo::Info* info, const char* game) {
    memset(screen, 0, 6144);
    memset(screen + 6144, ui_attr(0x07), 768);
    memset(screen + 6144, gameinfo::ATTR_BANNER, 32);
    char title[40];
    snprintf(title, sizeof(title), "Edit: %s", game);
    help_put_text(screen, 0, 1, title, (int) strlen(title) > 30 ? 30 : (int) strlen(title), gameinfo::ATTR_BANNER);
    auto put = [&](int row, int col, const char* s, uint8_t attr) {
        help_put_text(screen, row, col, s, (int) strlen(s) > 30 - col + 1 ? 30 - col + 1 : (int) strlen(s), attr);
    };
    char s[40];
    // H Y G S: the key and what there is (the software house on a line of its own)
    static const char* const ONE[] = { "H-Software house", "Y-Year", "G-Genre", "S-My score" };
    static const uint8_t ROW[] = { 2, 4, 5, 6 };
    for (int f = 0; f < gameinfo::META_COUNT; f++) {
        put(ROW[f], 1, ONE[f], gameinfo::ATTR_HEADING);
        const char* v = info->meta(f);
        if (f == gameinfo::META_SCORE && *v != '\0') snprintf(s, sizeof(s), "%s/10", v);
        else snprintf(s, sizeof(s), "%s", *v != '\0' ? v : "(none)");
        put(f == 0 ? 3 : ROW[f], f == 0 ? 3 : 12, s, *v != '\0' ? gameinfo::ATTR_TEXT : gameinfo::ATTR_DIM);
    }
    snprintf(s, sizeof(s), "C-Controls  %u/%u", (unsigned) info->ncontrols, (unsigned) gameinfo::CONTROLS_MAX);
    put(7, 1, s, gameinfo::ATTR_HEADING);
    snprintf(s, sizeof(s), "I-Instructions  %u/%u", (unsigned) info->ntext, (unsigned) gameinfo::TEXT_MAX);
    put(8, 1, s, gameinfo::ATTR_HEADING);
    put(9, 1, info->nnotes > 0 ? "N-My notes  (there are notes)" : "N-My notes  (none yet)", gameinfo::ATTR_HEADING);
    if (det_have) {
        put(11, 1, "R-Take back what you typed in", gameinfo::ATTR_HEADING);
    }
    put(13, 1, "What you type in is kept in", gameinfo::ATTR_DIM);
    put(14, 1, "the Game Notes folder as", gameinfo::ATTR_DIM);
    put(15, 1, "<game>.info.txt and is shown", gameinfo::ATTR_DIM);
    put(16, 1, "instead of the same part of", gameinfo::ATTR_DIM);
    put(17, 1, "the Instructions file.", gameinfo::ATTR_DIM);
#if PICOMENU_NET
    put(19, 1, "Z-Look it up in ZXDB", gameinfo::ATTR_HEADING);
#endif
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
        case 'g': return "Genre";
        case 's': return "My score";
        case 'c': return "Controls";
        default: return "Instructions";
    }
}

template <class E>
NOTES_FUNC static void det_field_draw_lines(uint8_t* screen, const E& ed, bool count = true) {
    for (int i = 0; i < E::VIEW && ed.top() + i < ed.line_count(); i++) {
        const char* l = ed.line(ed.top() + i);
        help_put_text(screen, 2 + i, 1, l, (int) strlen(l), gameinfo::ATTR_TEXT);
    }
    screen[6144 + (2 + ed.row() - ed.top()) * 32 + 1 + ed.col()] = ui_attr(0xC7);       // bright white, flashing
    char letters[32];
    if (ed.limit() > 0 && count) {
        snprintf(letters, sizeof(letters), "%u of %u letters", (unsigned) ed.used(), (unsigned) ed.limit());
        help_put_text(screen, 1, 1, letters, (int) strlen(letters), gameinfo::ATTR_DIM);
    }
    const char* keys = ed.prompting() ? "Y-Save  N-Discard  other-Back" : "BREAK-Finish";
    if (notes_message != nullptr) {
        help_put_text(screen, 21, 1, notes_message, (int) strlen(notes_message), gameinfo::ATTR_KEYS);
    }
    help_put_text(screen, 23, 1, keys, (int) strlen(keys), gameinfo::ATTR_KEYS);
}

NOTES_FUNC static void det_field_draw(uint8_t* screen, const char* game) {
    memset(screen, 0, 6144);
    memset(screen + 6144, ui_attr(0x07), 768);
    memset(screen + 6144, gameinfo::ATTR_BANNER, 32);
    char title[48];
    snprintf(title, sizeof(title), "%s: %s", det_field_name(det_field), game);
    help_put_text(screen, 0, 1, title, (int) strlen(title) > 30 ? 30 : (int) strlen(title), gameinfo::ATTR_BANNER);
    if (det_meta(det_field) >= 0) {
        det_field_draw_lines(screen, *reinterpret_cast<const DetEditorOne*>(snap_load_buffer + DET_EDITOR_OFFSET), det_field != 's');
        const char* hint = det_field == 'y' ? "The year, up to 8 letters" : det_field == 's' ? "Your score, a number 0 to 10" :
            det_field == 'g' ? "The genre, up to 30 letters" : "The name, up to 30 letters";
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
    memset(screen + 6144, ui_attr(0x07), 768);
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
        screen[6144 + (2 + ed.row()) * 32 + 1 + ed.col()] = ui_attr(0xC7);       // bright white, flashing
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
            case 'h': case 'H': case 'y': case 'Y': case 'g': case 'G': case 's': case 'S':
            case 'c': case 'C': case 'i': case 'I': {
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
#if PICOMENU_NET
            case 'z': case 'Z':
                zx_due = true;                      // (nmi_action_game_info starts it)
                break;
#endif
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
        bool one = det_meta(det_field) >= 0;
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

// A software house or genre too long for its place on the first page: the ROM scrolls it there
// (both: one, then the other). Its tables (startmenu.infoscroll, infoscroll2) are filled here;
// the scrolling starts afresh when they change or the ROM was scrolling something else.
static const uint32_t INFO_GENRE_TEXT = 48;            // in INFO_HOUSE, after the house
// The ROM's scrolling from the start of the first table, if it was scrolling something else (the
// menu's Reload line) or afresh is asked for
static void info_scroll_start(bool afresh) {
    uint8_t* nmi_rom_data = rom_data + 16384;
    const uint16_t cur = (uint16_t) (nmi_rom_data[INFO_SCROLL_CUR] | (nmi_rom_data[INFO_SCROLL_CUR + 1] << 8));
    if (!afresh && (cur == INFO_SCROLL1 || cur == INFO_SCROLL2)) return;
    nmi_rom_data[INFO_SCROLL_CUR] = (uint8_t) INFO_SCROLL1;
    nmi_rom_data[INFO_SCROLL_CUR + 1] = (uint8_t) (INFO_SCROLL1 >> 8);
    nmi_rom_data[INFO_SCROLL_POS] = 0;
    nmi_rom_data[INFO_SCROLL_TIMER] = (uint8_t) INFO_SCROLL_HOLD;
    nmi_rom_data[INFO_SCROLL_TIMER + 1] = (uint8_t) (INFO_SCROLL_HOLD >> 8);
}
NOTES_FUNC static void info_scroll_set(const gameinfo::Info* info, int page) {
    static_assert(INFO_HOUSE_WIDTH == gameinfo::CTRL_WIDTH, "the ROM scrolls the software house in its column");
    static_assert(gameinfo::HOUSE_MAX < INFO_GENRE_TEXT && INFO_GENRE_TEXT + gameinfo::GENRE_MAX < INFO_HOUSE_SIZE,
                  "INFO_HOUSE holds the whole house and genre");
    uint8_t* nmi_rom_data = rom_data + 16384;
    char* text = reinterpret_cast<char*>(nmi_rom_data) + INFO_HOUSE;
    const bool first = page == 0 && info->any();
    const bool house = first && strlen(info->house) > (size_t) INFO_HOUSE_WIDTH;
    if (house) snprintf(text, INFO_GENRE_TEXT, "%s", info->house);
    bool genre = false;
    if (first) {
        gameinfo::genre_shown(info->genre, text + INFO_GENRE_TEXT, gameinfo::GENRE_MAX + 1);
        genre = strlen(text + INFO_GENRE_TEXT) > (size_t) INFO_HOUSE_WIDTH;
    }
    // the tables: the text, the width, the place (column, row), the colours (0: the pico's), the next
    uint8_t t[2][8];
    auto table = [&](uint8_t* p, uint16_t src, int row, uint16_t next) {
        p[0] = (uint8_t) src; p[1] = (uint8_t) (src >> 8);
        p[2] = INFO_HOUSE_WIDTH;
        p[3] = INFO_HOUSE_COL; p[4] = (uint8_t) row;
        p[5] = 0;
        p[6] = (uint8_t) next; p[7] = (uint8_t) (next >> 8);
    };
    const uint16_t gsrc = (uint16_t) (INFO_HOUSE + INFO_GENRE_TEXT);
    const int grow = gameinfo::genre_row(*info);
    if (house && genre) {
        table(t[0], INFO_HOUSE, INFO_HOUSE_ROW, INFO_SCROLL2);
        table(t[1], gsrc, grow, INFO_SCROLL1);
    } else if (genre) {
        table(t[0], gsrc, grow, 0);
        table(t[1], gsrc, grow, 0);
    } else {
        table(t[0], INFO_HOUSE, INFO_HOUSE_ROW, 0);         // (the house, or "": nothing scrolls)
        table(t[1], INFO_HOUSE, INFO_HOUSE_ROW, 0);
    }
    const bool same = memcmp(nmi_rom_data + INFO_SCROLL1, t[0], 8) == 0 && memcmp(nmi_rom_data + INFO_SCROLL2, t[1], 8) == 0;
    memcpy(nmi_rom_data + INFO_SCROLL1, t[0], 8);
    memcpy(nmi_rom_data + INFO_SCROLL2, t[1], 8);
    info_scroll_start(!same);
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
        // typing in the notes or a box: a held key repeats (the ROM's keywait)
        bool typing = false;
        if (edit_kind == EK_NOTES) typing = notes_edit && !editor->prompting();
        else if (edit_kind == EK_FIELD) typing = det_meta(det_field) >= 0 ?
            !reinterpret_cast<DetEditorOne*>(snap_load_buffer + DET_EDITOR_OFFSET)->prompting() :
            !reinterpret_cast<DetEditorBig*>(snap_load_buffer + DET_EDITOR_OFFSET)->prompting();
        nmi_rom_data[START_KEYREPEAT] = typing ? 1 : 0;
        nmi_rom_data[1] = 3;
        nmi_rom_data[0] = 1;
        return;
    }
    const bool fresh = !info_loaded || (page == 0 && pic == 0);
    if ((page == 0 || !info_loaded) && !(info_full && info_loaded && !fresh)) {
        info_load(info, name, mode);
    }
    const bool file_picture = mode == 0 && info_source != nullptr;
    if (fresh) {
        // I on the menu (mode 1): the game's screen grabs too, as V shows them
        if (mode == 0 || name[0] != '\0') grabs_find(name, file_picture);
        else grab_count = 0;
    }
    info_loaded = true;
    int pages = gameinfo::page_count(*info);
    // The ROM counts pages in four bits (15 at most). With more (long instructions), the page the
    // ROM has is only which way it went: the Pico keeps the real one, and tells the ROM it is on
    // its first page (the real first), its last (the real last) or one in the middle, so N and P
    // go on (and round) as they would
    static int real_page = 0;
    static int view_page = 0;
    if (pages > 15) {
        const int asked = page;
        if (ended || asked == 255) real_page = pages - 1;
        else if (fresh && asked == 0) real_page = 0;            // (a new visit, or round from the last)
        else if (asked == view_page) { /* the same page (a picture, a question) */ }
        else if (asked == (view_page + 1) % 15) real_page = (real_page + 1) % pages;
        else if (asked == (view_page + 14) % 15) real_page = (real_page + pages - 1) % pages;
        else real_page = 0;
        if (real_page >= pages) real_page = pages - 1;
        view_page = real_page == 0 ? 0 : real_page == pages - 1 ? 14 : 7;
        page = real_page;
    } else if (page >= pages) {
        page = pages - 1;
    }
    // the pictures: the file's own (From File) and the grabs, in the order file's order. With
    // none, I on the menu shows the game's screen as it was when the NMI button was pressed.
    int pictures = grab_count;
    const bool stash_picture = mode == 1 && pictures == 0 && screen_stashed;
    auto from_file = [&](int k) { return k >= 0 && k < grab_count && grab_list[k].cat == screengrab::FROM_FILE; };
    if (pic >= pictures) pic = 0;
    if (op == 5) {                  // F: the picture on the whole screen, or back
        op = 0;
        if (info_full) {
            info_full = false;
        } else if (stash_picture || pictures > 0) {
            info_full = true;
        } else {
            notes_message = "No picture to show";
        }
    }
    if (info_full) {
        const uint8_t* full = nullptr;
        if (stash_picture) {
            full = screen_stash;
        } else if (pictures > 0) {
            if (from_file(pic)) {
                full = info_source;
            } else if (grab_load(name, pic, grab)) {
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
    bool grab_shown = mode == 0 && page == 0 && pictures > 0 && !from_file(pic);
    bool asking = false;
    if (op == 1 && grab_shown) {
        asking = true;
    }
    else if (op == 2 && grab_shown) {
        char base[screengrab::NAME_MAX + 1];
        int k = pic;
        if (screengrab::base_name(name, base) > 0 && k >= 0 && k < grab_count) {
            // its .scr and its .png (grab_format = both saves the two)
            for (int png = 0; png < 2; png++) {
                grab_list[k].from = (uint8_t) ((grab_list[k].from & 1) | (png ? 2 : 0));
                grab_path_of(k, base);
                if (fsops::kind(Drive::EXTERNAL_SD_CARD, grab_path) == fsops::KIND_NONE) continue;
                LOG("Delete the grab %s\n", grab_path);
                if (to_waste_bin(1, grab_path, false, erase_path, sizeof(erase_path)) != BIN_OK) {
                    grab_path_of(k, base);
                    fsops::remove_path(Drive::EXTERNAL_SD_CARD, grab_path);
                }
            }
        }
        grabs_find(name, file_picture);
        pictures = grab_count;
        if (pic >= pictures) pic = 0;
        grab_shown = pictures > 0 && !from_file(pic);
    }

    memset(screen, 0, 6144);
    memset(screen + 6144, ui_attr(0x07), 768);
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
        if (stash_picture) {
            thumb = screen_stash;
        } else if (pictures > 0) {
            if (from_file(pic)) {
                thumb = info_source;
            } else if (grab_load(name, pic, grab)) {
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
    gameinfo::key_help(mode == 1, pages, keys, grab_shown, stash_picture || pictures > 0);
    if (asking) {
        snprintf(keys, sizeof(keys), "Delete this grab?  Y-Yes  N-No");
    }
    if (notes_message != nullptr) {
        snprintf(keys, sizeof(keys), "%s", notes_message);
    }
    int keylen = (int) strlen(keys);
    help_put_text(screen, 23, keylen >= 32 ? 0 : 1, keys, keylen, gameinfo::ATTR_KEYS);
    info_scroll_set(info, page);
    preview_screen = screen;
    nmi_rom_data[1] = asking ? 1 : 0;       // the ROM then waits for Y or N
    nmi_rom_data[3] = (uint8_t) (pages > 15 ? view_page : page);    // the page as it was kept to the pages there are
    nmi_rom_data[0] = (uint8_t) ((pages > 15 ? 15 : pages) | ((pictures > 15 ? 15 : pictures) << 4));
}
// The texts set up writes (settings.h's DEFAULT_SETTINGS and ORDER_HEADING, then sdsetup.h's
// EXAMPLE), kept packed in the firmware (asm_to_c_file.py) and unpacked where Help's text goes
static const char* setup_texts() {
    static_assert(HELP_TEXT_OFFSET + SETUP_INI_SIZE + SETUP_EXAMPLE_SIZE <= SNAP_LOAD_BUFFER_SIZE, "they fit");
    char* t = snap_load_buffer + HELP_TEXT_OFFSET;
    lzss_unpack(SETUP_TEXTS_LZ, reinterpret_cast<uint8_t*>(t), SETUP_INI_SIZE + SETUP_EXAMPLE_SIZE);
    return t;
}


// ---- C on the NMI menu: set up the SD card (sdsetup.h). The ROM shows it as the game
// information with mode 2: D asks "Set up this card?", Y does it (op 2), any other key does not.
static uint8_t sd_setup_result[sdsetup::FOLDER_COUNT + 2];     // each folder, then the Instructions file, then the grabs' order file
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
                const char* example = setup_texts() + SETUP_INI_SIZE;
                bool ok = f.write(example, SETUP_EXAMPLE_SIZE) == (int) SETUP_EXAMPLE_SIZE;
                ok = f.close() && ok;
                r = ok ? sdsetup::MADE : sdsetup::FAILED;
                if (!ok) {
                    fsops::remove_path(Drive::EXTERNAL_SD_CARD, sdsetup::EXAMPLE_PATH);
                }
            }
        }
    }
    sd_setup_result[sdsetup::FOLDER_COUNT] = r;
    // Picomenu.ini, if there is none: every setting at its default, and the screen grab order
    // (that of an order.txt already on the card, so what it set is kept)
    trace_step('S', sdsetup::FOLDER_COUNT + 1);
    r = sdsetup::THERE;
    if (fsops::kind(Drive::EXTERNAL_SD_CARD, settings::FILE_NAME) == fsops::KIND_NONE) {
        r = sdsetup::FAILED;
        FileHandle& f = *notes_handle();
        // an order file already there: its text, kept to go in the new file (one handle at a time)
        char* order = snap_load_buffer + DET_FILE_OFFSET;
        int order_len = -1;
        if (f.open(Drive::EXTERNAL_SD_CARD, screengrab::ORDER_FILE, true) ||
                f.open(Drive::EXTERNAL_SD_CARD, screengrab::ORDER_FILE_BARE, true)) {
            order_len = f.read(order, DET_FILE_MAX);
            f.close();
        }
        if (f.open(Drive::EXTERNAL_SD_CARD, settings::FILE_NAME, false)) {
            auto put = [&](const char* text, size_t n) { return f.write(text, (int) n) == (int) n; };
            bool ok = put(setup_texts(), SETUP_INI_SIZE);     // DEFAULT_SETTINGS and ORDER_HEADING
            if (ok && order_len >= 0) {
                ok = put(order, (size_t) order_len);
                if (ok && order_len > 0 && order[order_len - 1] != '\n') ok = put("\r\n", 2);
            } else if (ok) {
                ok = put(settings::DEFAULT_ORDER, sizeof(settings::DEFAULT_ORDER) - 1);
            }
            ok = f.close() && ok;
            r = ok ? sdsetup::MADE : sdsetup::FAILED;
            if (!ok) {
                fsops::remove_path(Drive::EXTERNAL_SD_CARD, settings::FILE_NAME);
            }
        }
        if (r == sdsetup::FAILED) {
            set_last_error("Cannot make", "Picomenu.ini");
        } else {
            cfg_stale = true;
        }
    }
    sd_setup_result[sdsetup::FOLDER_COUNT + 1] = r;
    trace_step('Z', 0);             // finished
    LOG("SD card set up\n");
}

NOTES_FUNC static void sd_setup_draw(uint8_t* screen, int state, bool card) {
    memset(screen, 0, 6144);
    memset(screen + 6144, ui_attr(0x07), 768);
    memset(screen + 6144, gameinfo::ATTR_BANNER, 32);
    // the result's heading says how it went: created, failed, or already there
    bool failed = false, made_any = false;
    for (int i = 0; state == 2 && card && i <= sdsetup::FOLDER_COUNT + 1; i++) {
        failed = failed || sd_setup_result[i] == sdsetup::FAILED;
        made_any = made_any || sd_setup_result[i] == sdsetup::MADE;
    }
    const char* title = state != 2 || !card ? "Set up SD card" : failed ? "SD card creation failed" : made_any ? "SD card created" : "SD card already set up";
    help_put_text(screen, 0, 1, title, (int) strlen(title), gameinfo::ATTR_BANNER);
    auto line = [&](int row, const char* text, uint8_t attr = gameinfo::ATTR_TEXT) {
        help_put_text(screen, row, 1, text, (int) strlen(text), attr);
    };
    const char* keys = "X-Back";
    if (!card) {
        line(2, "No SD card found.");
        line(4, "Put the card in, then try");
        line(5, "again.");
    } else if (state == 2) {
        for (int i = 0; i <= sdsetup::FOLDER_COUNT + 1; i++) {
            const bool inst = i >= sdsetup::FOLDER_COUNT;
            const char* name = i == sdsetup::FOLDER_COUNT ? "Instructions.txt" : inst ? "Picomenu.ini" :
                sdsetup::FOLDERS[i].label != nullptr ? sdsetup::FOLDERS[i].label : sdsetup::FOLDERS[i].path + 1;
            char text[40];
            const uint8_t r = sd_setup_result[i];
            const char* how = r == sdsetup::MADE ? "made" : r == sdsetup::FAILED ? "FAILED" :
                i > sdsetup::FOLDER_COUNT ? "kept" : inst ? "kept yours" : "there already";
            snprintf(text, sizeof(text), "%s%s", name, inst ? "" : "/");
            line(2 + i, text);
            help_put_text(screen, 2 + i, i > sdsetup::FOLDER_COUNT ? 25 : 18, how, (int) strlen(how), r == sdsetup::FAILED ? gameinfo::ATTR_KEYS : gameinfo::ATTR_HEADING);
        }
        if (failed) {
            line(15, "SD card creation failed.");
            line(16, "Is the card full or locked?");
            line(17, "See Options, D-Diagnostics.");
        } else {
            line(15, made_any ? "SD card created." : "Nothing new was needed.");
            line(16, "Put your game information");
            line(17, "in Scraper/Instructions.txt.");
        }
    } else {
        line(2, "Makes these on the SD card.");
        line(3, "Nothing there is replaced.");
        for (int i = 0; i < sdsetup::FOLDER_COUNT; i++) {
            char name[24];
            snprintf(name, sizeof(name), "%s/", sdsetup::FOLDERS[i].label != nullptr ? sdsetup::FOLDERS[i].label : sdsetup::FOLDERS[i].path + 1);
            line(5 + i, name, gameinfo::ATTR_HEADING);
            help_put_text(screen, 5 + i, 15, sdsetup::FOLDERS[i].what, (int) strlen(sdsetup::FOLDERS[i].what), gameinfo::ATTR_TEXT);
        }
        line(16, "An example Instructions.txt and");
        line(17, "Picomenu.ini (the settings)");
        line(18, "are added if there are none.");
        keys = state == 1 ? "Set up this card?  Y-Yes  N-No" : "D-Set up  X-Back";
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

#if PICOMENU_NET
// ---- Z: ZXDB, the database of Spectrum software Spectrum Computing keeps, through the ZXInfo
// API (zxdb.h; HTTPS: net.h) ----
// From the WiFi screen (F in Tools), once the WiFi is up: a title typed in is searched for, a
// title picked shows its details and its tape and snapshot files, and one of those (only for a
// title ZXDB calls "Available") is downloaded to /Downloads on the SD card and unzipped. L gets
// the title's loading screen, saved as a screen grab ("<game> Loading Screen.scr", as the
// others are). From the game information's M menu, Z looks the game up: its software house and
// year go in its details, and (only if L is pressed) its loading screen is saved for it. From
// the Poke screen (P), Z looks the game last loaded up for its pokes: the title's .pok is saved
// as Poke List/<game>.pok, the name the Poke list looks for (never over one that is there).
// Nothing is fetched unless a key asks for it; each request names PicoMenu (netget::USER_AGENT).
// The state is in snap_load_buffer after the tape labels (the WiFi has the part before them);
// a loading screen comes into the screen at its start (it is then shown as it is).

enum ZxMode : uint8_t { ZX_TYPE, ZX_JOIN, ZX_ASK, ZX_RESULTS, ZX_ENTRY, ZX_UNZIP, ZX_PICTURE, ZX_PICKED, ZX_NOTE, ZX_FINISH };
enum ZxWant : uint8_t { ZW_SEARCH, ZW_ENTRY, ZW_FILE, ZW_SCREEN, ZW_TEXT };

struct ZxState {
    zxdb::Flat flat;
    zxdb::Search search;
    zxdb::Entry entry;
    unzip::Member member;
    FileHandle file, out;
    char url[netget::URL_MAX];
    char path[96];                          // the file being written (removed if it fails)
    char typed[32];
    char name[48];                          // the file being downloaded, for the screen
    char said[48];                          // what was done ("Saved /Downloads/GreenBeret.tzx")
    char game[gameinfo::GAME_MAX + 1];      // the information's game (Z in its M menu)
    char base[screengrab::NAME_MAX + 1];    // its name for its screen grabs
    char house[gameinfo::HOUSE_MAX + 1];
    char year[gameinfo::YEAR_MAX + 1];
    char genre[gameinfo::GENRE_MAX + 1];
    const char* msg;
    uint32_t got;                           // the loading screen's bytes so far
    uint32_t shown_kb;
    uint32_t shown_step;                    // the stage and seconds shown while asking
    char detail[64];                        // how far a request that failed got (net::detail)
    char io[64];                            // and what its connection did after the request (net::io)
    uint16_t page;
    uint8_t mode, back, want, shown_state;
    int8_t hit;
    bool wifi_asked;                        // (the lookups: the WiFi started, at a poll after the first)
    uint32_t join_ms, up_ms;                // when the lookup began, when the WiFi was up (0: not yet)
    bool info;                              // the information's lookup
    bool pok;                               // Z on the Poke screen: the game's .pok
    bool solo;                              // Z on the menu: the search, with the WiFi started for it
    uint8_t asking;                         // 1-8 or L pressed on a title: "Match search selection?" asked (the key)
    bool match;                             // Y to it: the file or screen named after the title picked
    bool file_open, zipped;
    // the information's lookup: what S fetches besides the house and year (I, L), and what came
    bool get_text, get_scr, text_done, scr_done, text_got, scr_saved;
};
static const uint32_t ZX_STATE_AT = 94208;      // after the tape labels (static_assert there)
// The WiFi's work (lwIP, the TLS handshake in its callbacks, the file transfer) runs on a stack of
// its own while the WiFi is on: core 0's is 2 KB (4 KB before it reaches core 1's), and a TLS
// handshake checking an ECDSA signature inside lwIP's packet handling needs more
static const uint32_t NET_STACK_SIZE = 5632;     // (the deepest seen: about 3.5 KB)
static const uint32_t NET_STACK_AT = RADIO_ARENA_END - NET_STACK_SIZE;
static_assert(ZX_STATE_AT + sizeof(ZxState) <= NET_STACK_AT, "ZXDB's state fits before the WiFi's stack");
static_assert(NET_STACK_AT % 8 == 0, "the WiFi's stack is aligned");
static const char ZX_FOLDER[] = "/Downloads";
static const char ZX_ZIP[] = "/Downloads/~zxdb.zip";    // the download, until it is unzipped
static const char ZX_TEXT[] = "/Game Notes/~zxdb.txt";  // a game's instructions, until they go in its details
static const uint32_t ZX_TEXT_MAX = 9215;               // (DET_FILE_MAX: the rest is not kept)
static const uint32_t ZX_FILE_MAX = 2u << 20;           // (a tape or snapshot is far smaller)

static ZxState& zx() { return *reinterpret_cast<ZxState*>(snap_load_buffer + ZX_STATE_AT); }
static uint32_t now_ms_zx() { return to_ms_since_boot(get_absolute_time()); }
#ifdef WIFI_ENABLE
static const uint32_t ZX_START_MS = 300, ZX_SETTLE_MS = 1000;   // the WiFi started, the search after it is up
#else
static const uint32_t ZX_START_MS = 0, ZX_SETTLE_MS = 0;        // (the simulator's pretend network: at once)
#endif
static bool zx_typing() { return zx_live && zx().mode == ZX_TYPE; }
static bool zx_alone_live() { return zx_live && (zx().pok || zx().solo); }

#ifdef WIFI_ENABLE
// While a ZXDB screen is up: the main loop going round is fed to the hang watch (bt.h; a hang
// there is kept as "Hang PC ..." for the B screen; not the watchdog, whose 8 s is less than a
// TLS handshake's sums can take in one go), and the steps are noted
// for the crash recorder's "Last:" line: 0x50 the lookup began, 0x51 the WiFi started, 0x52 up,
// 0x53-0x57 a request (search, entry, file, screen, text), 0x58 + its stage (DNS, TCP, TLS,
// waiting, getting)
static void zx_watch() {
    static bool watching = false;
    static uint8_t noted = 0xFF;
    if (zx_live != watching) {
        watching = zx_live;
        bt::hang_watch(watching);
    }
    if (!watching) return;
    bt::hang_feed();
    uint8_t stage = net::state() == net::BUSY ? (uint8_t) net::stage() : 0xFF;
    if (stage != noted) {
        noted = stage;
        if (stage != 0xFF) bt::note((uint8_t) (0x58 + stage));
    }
}

static void net_work() {
    wifi::poll();
    net::poll();
}

// fn run with the stack pointer at the top of the WiFi's stack (in snap_load_buffer, lent to the
// WiFi while it is on), and back. r4 keeps the old one (the callee saves it).
static void on_net_stack(void (*fn)()) {
    uint8_t* top = reinterpret_cast<uint8_t*>(snap_load_buffer) + NET_STACK_AT + NET_STACK_SIZE;
    asm volatile(
        "mov r4, sp\n"
        "mov sp, %[top]\n"
        "blx %[fn]\n"
        "mov sp, r4\n"
        :
        : [top] "l" (top), [fn] "l" (fn)
        : "r0", "r1", "r2", "r3", "r4", "r12", "lr", "memory", "cc");
}
#endif

// The WiFi, for the information's lookup (the WiFi screen has it on already). Null, or why not
static const char* zx_wifi_start() {
    if (!web_card_ok()) return "There is no SD card (or the PC\nhas it as a USB drive).";
#ifdef WIFI_ENABLE
    if (web_on && wifi::state() != wifi::FAILED) return nullptr;
#if PICOMENU_BT
    bt_off();                               // one radio at a time
#endif
    WebState& w = web_state();
    memset(&w, 0, sizeof(w));
    web_read_ini(w);
    if (w.wifi.ssid[0] == '\0') {
        return "Put the WiFi's name and password\nin Picomenu.ini on the SD card:\n\n[WiFi]\nssid = MyNetwork\npassword = secret";
    }
    net::stop();                            // (nothing of lwIP's is kept as it starts again)
    walk_stop();                            // (as F does: no folder of the card left open)
    web_on = true;
    bt::note(0x51);
    const char* why = wifi::start(w.wifi.ssid, w.wifi.password, w.wifi.web_password,
        reinterpret_cast<uint8_t*>(snap_load_buffer) + WEB_ARENA_AT, RADIO_ARENA_END - WEB_ARENA_AT, &WEB_HOOKS);
    return why;
#else
    return nullptr;                         // (the simulator's pretend network is always there)
#endif
}

static bool zx_wifi_up() {
#ifdef WIFI_ENABLE
    return web_on && wifi::state() == wifi::UP;
#else
    return true;
#endif
}

// Memory for unzipping: lwIP's heap (the TLS memory has been let go: net::stop)
static void* zx_alloc(size_t n) { return net::alloc(n); }
static void zx_free(void* p) { if (p != nullptr) net::release(p); }

// The file being written is let go (and removed: it is not finished)
static void zx_file_drop() {
    ZxState& z = zx();
    if (!z.file_open) return;
    z.file.close();
    fsops::remove_path(Drive::EXTERNAL_SD_CARD, z.path);
    z.file_open = false;
}

// The answer's body, as it comes (from lwIP's callbacks)
static bool zx_body(const uint8_t* p, size_t n, void*) {
    ZxState& z = zx();
    switch (z.want) {
        case ZW_SEARCH:
        case ZW_ENTRY:
            z.flat.feed(p, n);
            return true;
        case ZW_FILE:
            if (net::received() > ZX_FILE_MAX) return false;
            return z.file.write(p, n) == (int) n;
        case ZW_TEXT: {
            // the start of the instructions (only that much fits the details); the rest let go
            if (z.got >= ZX_TEXT_MAX) return true;
            size_t take = n < ZX_TEXT_MAX - z.got ? n : (size_t) (ZX_TEXT_MAX - z.got);
            z.got += (uint32_t) take;
            return z.file.write(p, take) == (int) take;
        }
        default:
            if (z.got + n > screengrab::SCR_SIZE) return false;    // not a .scr
            memcpy(snap_load_buffer + z.got, p, n);                // (the screen)
            z.got += (uint32_t) n;
            return true;
    }
}

// Asks for z.url; back is where to go if it fails or is stopped
__attribute__((noinline)) static void zx_ask(ZxWant want, uint8_t back) {
    ZxState& z = zx();
    z.want = want;
    z.back = back;
    z.msg = nullptr;
    z.detail[0] = '\0';
    z.shown_kb = 0;
    z.shown_step = 0;
    if (want == ZW_SEARCH) z.flat.begin(&z.search, z.pok);
    else if (want == ZW_ENTRY) z.flat.begin(&z.entry);
    else if (want == ZW_SCREEN || want == ZW_TEXT) z.got = 0;
    LOG("ZXDB: %s\n", z.url);
#ifdef WIFI_ENABLE
    bt::note((uint8_t) (0x53 + (want < 4 ? want : 4)));    // (0x53 search, 0x54 entry, 0x55 file, 0x56 screen, 0x57 text)
#endif
    const char* why = net::get(z.url, zx_body, nullptr);
    if (why != nullptr) {
        zx_file_drop();
        z.msg = why;
        snprintf(z.detail, sizeof(z.detail), "%s", net::detail());
        z.io[0] = '\0';
        z.mode = back;
        return;
    }
    z.mode = ZX_ASK;
}

__attribute__((noinline)) static void zx_search(uint16_t page, uint8_t back) {
    ZxState& z = zx();
    if (!zxdb::search_url(z.typed, page, z.url, sizeof(z.url))) {
        z.msg = "Too long to search for";
        z.mode = back;
        return;
    }
    z.page = page;
    zx_ask(ZW_SEARCH, back);
}

// "Name (2).tzx" if "Name.tzx" is there already (to 9)
static bool zx_free_path(const char* name, char* out, size_t size) {
    snprintf(out, size, "%s/%s", ZX_FOLDER, name);
    if (fsops::kind(Drive::EXTERNAL_SD_CARD, out) == fsops::KIND_NONE) return true;
    const char* dot = strrchr(name, '.');
    int stem = dot != nullptr ? (int) (dot - name) : (int) strlen(name);
    for (int i = 2; i <= 9; i++) {
        snprintf(out, size, "%s/%.*s (%d)%s", ZX_FOLDER, stem, name, i, dot != nullptr ? dot : "");
        if (fsops::kind(Drive::EXTERNAL_SD_CARD, out) == fsops::KIND_NONE) return true;
    }
    return false;
}

// The title picked in the search as a file's name (Y to "Match search selection?"): "Bomb Jack".
// Letters FAT does not allow, and ? (a letter the Spectrum can't show) are left out; blanks are
// not doubled; at most 40 letters. False if nothing is left.
static bool zx_match_name(char* out, size_t size) {
    ZxState& z = zx();
    const char* t = z.search.hits[z.hit].title;
    size_t k = 0;
    for (; *t != '\0' && k + 1 < size && k < 40; t++) {
        unsigned char c = (unsigned char) *t;
        if (c < 32 || c > 126 || strchr("\\/:*?\"<>|", c) != nullptr) continue;
        if (c == ' ' && (k == 0 || out[k - 1] == ' ')) continue;
        out[k++] = (char) c;
    }
    while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == '.')) k--;
    out[k] = '\0';
    return k > 0;
}

// A file's name, as it is (name), or after the title picked (z.match): "Bomb Jack.tap" for
// "BOMBJAC1.TAP" (its extension, small letters)
static void zx_file_name(const char* name, char* out, size_t size) {
    char m[44];
    const char* dot = strrchr(name, '.');
    if (zx().match && dot != nullptr && zx_match_name(m, sizeof(m))) {
        int n = snprintf(out, size, "%s%s", m, dot);
        for (char* c = out + (n > 0 ? (int) strlen(m) : 0); *c != '\0'; c++) {
            if (*c >= 'A' && *c <= 'Z') *c = (char) (*c + 32);
        }
    } else {
        snprintf(out, size, "%s", name);
    }
}

// "GreenBeret.tzx.zip" -> "GreenBeret" (the name a game downloaded goes by: its grabs')
static void zx_stem(const char* file, char* out, size_t size) {
    snprintf(out, size, "%s", file);
    char* dot = strrchr(out, '.');
    if (dot != nullptr && strcasecmp(dot, ".zip") == 0) { *dot = '\0'; dot = strrchr(out, '.'); }
    if (dot != nullptr && dot != out) *dot = '\0';
}

// 1-8 on a title: its file downloaded
__attribute__((noinline)) static void zx_download(int i) {
    ZxState& z = zx();
    const zxdb::File& f = z.entry.files[i];
    if (!zxdb::file_url(f.path, z.url, sizeof(z.url))) {
        z.msg = "PicoMenu can't get that file";
        return;
    }
    uint8_t made = make_sd_folder(ZX_FOLDER);
    if (made != fsops::OK && made != fsops::NAME_IN_USE) {
        z.msg = "Could not make /Downloads";
        return;
    }
    const char* b = zxdb::base_of(f.path);
    size_t n = strlen(b);
    z.zipped = n > 4 && strcasecmp(b + n - 4, ".zip") == 0;
    char named[64];
    zx_file_name(b, named, sizeof(named));
    if (z.zipped) snprintf(z.path, sizeof(z.path), "%s", ZX_ZIP);
    else if (!zx_free_path(named, z.path, sizeof(z.path))) {
        z.msg = "Too many with that name";
        return;
    }
    snprintf(z.name, sizeof(z.name), "%s", b);
    walk_stop();
    if (!z.file.open(Drive::EXTERNAL_SD_CARD, z.path, false)) {
        z.msg = "Could not write to the SD card";
        return;
    }
    z.file_open = true;
    zx_ask(ZW_FILE, ZX_ENTRY);
}

// The download unzipped into /Downloads (and the zip removed)
__attribute__((noinline)) static void zx_unzip() {
    ZxState& z = zx();
    net::stop();                            // (its TLS memory, lwIP's, let go: room for this)
    unzip::Work* w = static_cast<unzip::Work*>(zx_alloc(sizeof(unzip::Work)));
    uint8_t* window = static_cast<uint8_t*>(zx_alloc(unzip::WINDOW));
    const char* why = nullptr;
    walk_stop();
    if (w == nullptr || window == nullptr) {
        why = "Not enough memory to unzip";
    } else if (!z.file.open(Drive::EXTERNAL_SD_CARD, ZX_ZIP, true)) {
        why = "Could not read the download";
    } else {
        char named[64];
        if (!unzip::find(z.file, z.member, w->in, sizeof(w->in))) {
            why = "No tape or snapshot in the zip";
        } else if (zx_file_name(z.member.name, named, sizeof(named)), !zx_free_path(named, z.path, sizeof(z.path))) {
            why = "Too many with that name";
        } else if (!z.out.open(Drive::EXTERNAL_SD_CARD, z.path, false)) {
            why = "Could not write to the SD card";
        } else {
            bool ok = unzip::extract(z.file, z.member, z.out, *w, window);
            ok = z.out.close() && ok;
            if (!ok) {
                fsops::remove_path(Drive::EXTERNAL_SD_CARD, z.path);
                why = "The zip is damaged";
            }
        }
        z.file.close();
    }
    zx_free(window);
    zx_free(w);
    fsops::remove_path(Drive::EXTERNAL_SD_CARD, ZX_ZIP);
    if (why == nullptr) {
        snprintf(z.said, sizeof(z.said), "Saved %s", zxdb::base_of(z.path));
        LOG("ZXDB: saved %s\n", z.path);
    } else {
        snprintf(z.said, sizeof(z.said), "%s", why);
    }
    z.mode = ZX_ENTRY;
}

// The loading screen that came (it is in the screen) saved as a grab of the game
__attribute__((noinline)) static void zx_save_screen() {
    ZxState& z = zx();
    char base[screengrab::NAME_MAX + 1];
    char m[44];
    if (z.info) {
        snprintf(base, sizeof(base), "%s", z.base);
    } else if (z.match && zx_match_name(m, sizeof(m))) {
        if (screengrab::base_name(m, base) == 0) snprintf(base, sizeof(base), "Screen");     // (the title picked)
    } else {
        // the name the title's game will have when it is downloaded (its first file's)
        char stem[64];
        zx_stem(z.entry.nfiles > 0 ? zxdb::base_of(z.entry.files[0].path) : z.entry.info.title, stem, sizeof(stem));
        if (screengrab::base_name(stem, base) == 0) snprintf(base, sizeof(base), "Screen");
    }
    int number = -1;
    walk_stop();
    if (!grab_folder_make()) {
        snprintf(z.said, sizeof(z.said), "Could not make Scraper/Screen Grabs");
    } else if ((number = grab_free_number(base, screengrab::LOADING)) < 0) {
        snprintf(z.said, sizeof(z.said), "Too many loading screens saved");
    } else if (!grab_write(z.out, base, screengrab::LOADING, number, reinterpret_cast<const uint8_t*>(snap_load_buffer))) {
        snprintf(z.said, sizeof(z.said), "Could not save the screen");
    } else {
        screengrab::saved_message(base, screengrab::LOADING, number, z.said, sizeof(z.said));
        z.scr_saved = true;
        LOG("ZXDB: loading screen %s\n", grab_path);
    }
}

// S on a title for the information: what was chosen with I and L fetched, one after the other,
// then ZX_FINISH (the details are saved: zx_info_end)
__attribute__((noinline)) static void zx_picked_next() {
    ZxState& z = zx();
    const zxdb::Hit& h = z.search.hits[z.hit];
    if (z.get_text && !z.text_done && h.instr[0] != '\0') {
        z.text_done = true;
        if (!zxdb::file_url(h.instr, z.url, sizeof(z.url))) {
            z.msg = "PicoMenu can't get its instructions";
            z.mode = ZX_PICKED;
            return;
        }
        snprintf(z.path, sizeof(z.path), "%s", ZX_TEXT);
        walk_stop();
        if (!z.file.open(Drive::EXTERNAL_SD_CARD, z.path, false)) {
            z.msg = "Could not write to the SD card";
            z.mode = ZX_PICKED;
            return;
        }
        z.file_open = true;
        zx_ask(ZW_TEXT, ZX_PICKED);
        return;
    }
    if (z.get_scr && !z.scr_done && h.scr[0] != '\0') {
        z.scr_done = true;
        if (zxdb::file_url(h.scr, z.url, sizeof(z.url))) {
            zx_ask(ZW_SCREEN, ZX_PICKED);
            return;
        }
    }
    z.mode = ZX_FINISH;
}

// A line of instructions that the details file would read as something else ("Controls", "Year:
// 1984", a ; comment) is changed a little so it stays text
static void zx_text_line(char* line) {
    const char* t = gameinfo::skip_blanks(line);
    const char* rest;
    if (*t == ';') {
        *const_cast<char*>(t) = ':';
    } else if (gameinfo::meta_key(t, &rest) >= 0) {
        char* colon = strchr(line, ':');
        if (colon != nullptr) *colon = '-';
    } else if (gameinfo::details_heading(t, "controls", &rest) || gameinfo::details_heading(t, "instructions", &rest)) {
        size_t n = strlen(line);
        if (n + 1 < 32) { line[n] = '.'; line[n + 1] = '\0'; }
    }
}

// The instructions that came (ZX_TEXT) as the details' instructions: plain letters, lines of 30 at
// most (as the I editor has them), one blank line at a time, as much as fits (TEXT_MAX: 8 KB; the
// I editor on the Spectrum takes 96 lines, so longer ones are edited in the browser, T-F).
// False: none.
__attribute__((noinline)) static bool zx_import_text(gameinfo::Details* d) {
    ZxState& z = zx();
    char* raw = det_file();
    int got = 0;
    walk_stop();
    if (z.out.open(Drive::EXTERNAL_SD_CARD, ZX_TEXT, true)) {
        got = z.out.read(raw, DET_FILE_MAX - 1);
        z.out.close();
    }
    if (got <= 0) return false;
    // \r\n and \r as \n, tabs as spaces, nothing but plain letters, no more than one blank line
    size_t k = 0;
    int newlines = 2;                       // (none at the start)
    for (int i = 0; i < got; i++) {
        unsigned char c = (unsigned char) raw[i];
        if (c == '\r') {
            if (i + 1 < got && raw[i + 1] == '\n') continue;
            c = '\n';
        }
        if (c == '\n') {
            if (newlines >= 2) continue;
            newlines++;
            raw[k++] = '\n';
            continue;
        }
        if (c == '\t') c = ' ';
        if (c < 32) continue;
        if (c >= 127) c = '?';
        newlines = 0;
        raw[k++] = (char) c;
    }
    while (k > 0 && raw[k - 1] == '\n') k--;
    raw[k] = '\0';
    if (k == 0) return false;
    // wrapped to 30, into the .png reading's room (free now: the information is loaded again after)
    static_assert(PNG_SCRATCH_OFFSET + gameinfo::TEXT_MAX + 8 <= 82944, "the instructions' room");
    char* out = reinterpret_cast<char*>(snap_load_buffer + PNG_SCRATCH_OFFSET);
    size_t n = 0;
    gameinfo::Wrap w(raw, 30);
    char line[32];
    bool cut = false;
    while (w.next(line)) {
        zx_text_line(line);
        size_t len = strlen(line);
        if (n + len + 1 + 4 > gameinfo::TEXT_MAX) { cut = true; break; }
        memcpy(out + n, line, len);
        n += len;
        out[n++] = '\n';
    }
    if (cut) { memcpy(out + n, "...\n", 4); n += 4; }
    out[n] = '\0';
    d->set_text(out);
    return true;
}

// A request has finished (net::state DONE or FAILED)
__attribute__((noinline)) static void zx_answered() {
    ZxState& z = zx();
    if (net::state() == net::FAILED) {
        LOG("ZXDB: failed: %s (%s)\n", net::why(), net::detail());
        zx_file_drop();
        z.msg = net::why();
        snprintf(z.detail, sizeof(z.detail), "%s", net::detail());
        snprintf(z.io, sizeof(z.io), "%s", net::io());
        z.mode = z.back;
        return;
    }
    switch (z.want) {
        case ZW_SEARCH:
            z.flat.end();
            z.mode = ZX_RESULTS;
            if (z.search.n == 0) z.msg = "Nothing found";
            break;
        case ZW_ENTRY: {
            z.flat.end();
            zxdb::tidy_files(z.entry);
            const zxdb::Hit& h = z.search.hits[z.hit];
            if (z.entry.info.title[0] == '\0') snprintf(z.entry.info.title, sizeof(z.entry.info.title), "%s", h.title);
            if (z.entry.info.scr[0] == '\0') snprintf(z.entry.info.scr, sizeof(z.entry.info.scr), "%s", h.scr);
            if (z.entry.info.avail[0] == '\0') snprintf(z.entry.info.avail, sizeof(z.entry.info.avail), "%s", h.avail);
            z.said[0] = '\0';
            z.asking = 0;
            z.mode = ZX_ENTRY;
            break;
        }
        case ZW_FILE:
            z.file_open = false;
            if (!z.file.close()) {
                fsops::remove_path(Drive::EXTERNAL_SD_CARD, z.path);
                z.msg = "Could not write to the SD card";
                z.mode = ZX_ENTRY;
            } else if (z.pok) {
                LOG("ZXDB: pokes saved as %s\n", z.path);
                z.msg = "Pokes saved in Poke List:\n\nL on the Poke screen lists them";
                z.mode = ZX_NOTE;
            } else if (z.zipped) {
                z.mode = ZX_UNZIP;              // (done at the next poll: the screen says so first)
            } else {
                snprintf(z.said, sizeof(z.said), "Saved %s", zxdb::base_of(z.path));
                z.mode = ZX_ENTRY;
            }
            break;
        case ZW_TEXT:
            z.file_open = false;
            z.text_got = z.file.close();
            if (!z.text_got) {
                z.msg = "Could not write to the SD card";
                z.mode = ZX_PICKED;
            } else {
                zx_picked_next();
            }
            break;
        default:
            if (z.got != screengrab::SCR_SIZE) {
                z.msg = "That was not a loading screen";
                z.mode = z.back;
            } else {
                zx_save_screen();
                z.mode = ZX_PICTURE;
            }
            break;
    }
}

// The information's lookup has ended: the WiFi off, the house and year saved (save), the M menu
// shown again
__attribute__((noinline)) static void zx_info_end(bool save) {
    ZxState& z = zx();
    zx_live = false;
    web_off();
    notes_message = save ? z.said : nullptr;
    if (save) {
        gameinfo::Details* d = det_data();
        det_read(z.game, d);
        if (z.house[0] != '\0') d->set_house(z.house);
        if (z.year[0] != '\0') d->set_year(z.year);
        if (z.genre[0] != '\0') d->set_meta(gameinfo::META_GENRE, z.genre);
        const bool text = z.text_got && zx_import_text(d);
        const bool shot = z.scr_saved;
        if (det_write(z.game, d)) {
            det_have = det_saved = true;
            // (the M menu's line has room for 30)
            snprintf(z.said, sizeof(z.said), "%s", text && shot ? "Saved details, text and screen" :
                text ? "Saved details and instructions" : shot ? "Saved details and screen" :
                z.get_scr ? "Saved details (no screen)" : "House, year and genre saved");
            LOG("ZXDB: details saved for %s: %s, %s, %s%s%s\n", z.game, z.house, z.year, z.genre, text ? ", instructions" : "",
                shot ? ", screen" : "");
        } else {
            snprintf(z.said, sizeof(z.said), "Could not save the details");
        }
    }
    fsops::remove_path(Drive::EXTERNAL_SD_CARD, ZX_TEXT);
    info_loaded = false;                    // (its memory was the WiFi's)
}

static void zx_text(uint8_t* screen, int row, const char* s, uint8_t attr) {
    help_put_text(screen, row, 1, s, (int) strlen(s), attr);
}

__attribute__((noinline)) static void zx_draw(uint8_t* screen) {
    ZxState& z = zx();
    if (z.mode == ZX_PICTURE || (z.mode == ZX_ASK && z.want == ZW_SCREEN)) {
        preview_screen = screen;            // the loading screen, as it comes
        return;
    }
    slots_banner(screen, "", z.info ? "ZXDB: look this game up" : z.pok ? "ZXDB: pokes for this game" : "ZXDB: Spectrum Computing");
    char t[48];
    const char* keys = "X-Back";
    switch (z.mode) {
        case ZX_JOIN:
            zx_text(screen, 2, "Joining the WiFi ...", gameinfo::ATTR_TEXT);
            keys = "X-Stop";
            break;
        case ZX_NOTE: {
            int row = 2;
            for (const char* p = z.msg != nullptr ? z.msg : ""; *p != '\0' && row < 20;) {
                const char* nl = strchr(p, '\n');
                size_t n = nl ? (size_t) (nl - p) : strlen(p);
                help_put_text(screen, row, 1, p, (int) n, row == 2 ? gameinfo::ATTR_TEXT : gameinfo::ATTR_DIM);
                row++;
                p = nl ? nl + 1 : p + n;
            }
            keys = "Any key-Back";
            break;
        }
        case ZX_TYPE:
            zx_text(screen, 2, "Search for a title:", gameinfo::ATTR_HEADING);
            snprintf(t, sizeof(t), "%s_", z.typed);
            zx_text(screen, 4, t, gameinfo::ATTR_TEXT);
            zx_text(screen, 6, "Type part of its name, then", gameinfo::ATTR_DIM);
            zx_text(screen, 7, "ENTER. DELETE takes a letter", gameinfo::ATTR_DIM);
            zx_text(screen, 8, "back.", gameinfo::ATTR_DIM);
            zx_text(screen, 17, "Titles from ZXDB (ODbL), through", gameinfo::ATTR_DIM);
            zx_text(screen, 18, "the ZXInfo API; files from", gameinfo::ATTR_DIM);
            zx_text(screen, 19, "Spectrum Computing, archive.org", gameinfo::ATTR_DIM);
            keys = "ENTER-Search  BREAK-Back";
            break;
        case ZX_ASK:
            if (z.want == ZW_SEARCH) snprintf(t, sizeof(t), "Searching for %.17s ...", z.typed);
            else if (z.want == ZW_ENTRY) snprintf(t, sizeof(t), "Asking about %.17s ...", z.search.hits[z.hit].title);
            else if (z.want == ZW_FILE) snprintf(t, sizeof(t), "Downloading %.19s", z.name);
            else if (z.want == ZW_TEXT) snprintf(t, sizeof(t), "Getting its instructions ...");
            else snprintf(t, sizeof(t), "Getting the loading screen ...");
            zx_text(screen, 2, t, gameinfo::ATTR_TEXT);
            snprintf(t, sizeof(t), "From %.26s", net::host());
            zx_text(screen, 3, t, gameinfo::ATTR_DIM);
            if (net::stage() < net::GET) {
                static const char* const STAGE[] = { "Finding the server", "Connecting", "Securing the connection",
                    "Waiting for the answer" };
                snprintf(t, sizeof(t), "%s ... %us", STAGE[net::stage()], (unsigned) (net::elapsed_ms() / 1000));
                zx_text(screen, 5, t, gameinfo::ATTR_DIM);
                // (securing it is sums the Pico takes some seconds over, the screen standing still)
                zx_text(screen, 7, "This can take 10 seconds or so.", gameinfo::ATTR_DIM);
            } else if (z.want == ZW_FILE) {
                int64_t len = net::length();
                if (len > 0) snprintf(t, sizeof(t), "%u KB of %u KB", (unsigned) (net::received() / 1024), (unsigned) ((len + 1023) / 1024));
                else snprintf(t, sizeof(t), "%u KB", (unsigned) (net::received() / 1024));
                zx_text(screen, 5, t, gameinfo::ATTR_HEADING);
            }
            keys = "X-Stop";
            break;
        case ZX_UNZIP:
            snprintf(t, sizeof(t), "Unzipping %.21s ...", z.name);
            zx_text(screen, 2, t, gameinfo::ATTR_TEXT);
            keys = "";
            break;
        case ZX_RESULTS: {
            const zxdb::Search& s = z.search;
            int pages = (int) ((s.total + zxdb::PAGE - 1) / zxdb::PAGE);
            if (pages > 1) snprintf(t, sizeof(t), "%u found, page %u of %d", (unsigned) s.total, (unsigned) z.page + 1, pages);
            else snprintf(t, sizeof(t), "%u found for %.18s", (unsigned) s.total, z.typed);
            zx_text(screen, 2, t, gameinfo::ATTR_HEADING);
            for (int i = 0; i < s.n; i++) {
                snprintf(t, sizeof(t), "%d %-24.24s %.4s", (i + 1) % 10, s.hits[i].title, s.hits[i].year);
                zx_text(screen, 4 + i, t, gameinfo::ATTR_TEXT);
            }
            if (z.info) zx_text(screen, 15, "Pick the game to take its", gameinfo::ATTR_DIM);
            if (z.info) zx_text(screen, 16, "house, year and genre from.", gameinfo::ATTR_DIM);
            if (z.pok) zx_text(screen, 15, "Pick the game to get its pokes.", gameinfo::ATTR_DIM);
            zx_text(screen, 21, pages > 1 ? "1-0 Pick  N/P-Page  T-Type" : "1-0 Pick  T-Type again", gameinfo::ATTR_KEYS);
            keys = "X-Back";
            break;
        }
        case ZX_ENTRY:
        case ZX_PICKED: {
            const zxdb::Hit& h = z.mode == ZX_ENTRY ? z.entry.info : z.search.hits[z.hit];
            zx_text(screen, 2, h.title, gameinfo::ATTR_HEADING);
            snprintf(t, sizeof(t), "%.31s", h.house[0] != '\0' ? h.house : "(no software house)");
            zx_text(screen, 3, t, gameinfo::ATTR_TEXT);
            snprintf(t, sizeof(t), "%.4s  %.25s", h.year[0] != '\0' ? h.year : "----", h.genre);
            zx_text(screen, 4, t, gameinfo::ATTR_DIM);
            if (z.mode == ZX_PICKED && z.pok) {
                if (h.instr[0] == '\0') {
                    zx_text(screen, 6, "ZXDB has no pokes for it.", gameinfo::ATTR_TEXT);
                } else {
                    zx_text(screen, 6, "Download its pokes as", gameinfo::ATTR_TEXT);
                    zx_text(screen, 7, "Poke List/", gameinfo::ATTR_DIM);
                    snprintf(t, sizeof(t), "%.27s.pok?", z.game);
                    zx_text(screen, 8, t, gameinfo::ATTR_HEADING);
                    zx_text(screen, 21, "Y-Yes  N-No", gameinfo::ATTR_KEYS);
                }
                keys = "X-Back to the list";
                break;
            }
            if (z.mode == ZX_PICKED) {
                zx_text(screen, 6, "S saves its software house,", gameinfo::ATTR_DIM);
                zx_text(screen, 7, "year and genre in its details,", gameinfo::ATTR_DIM);
                zx_text(screen, 8, "with what is chosen below:", gameinfo::ATTR_DIM);
                if (h.instr[0] != '\0') {
                    snprintf(t, sizeof(t), "I-Instructions       %s", z.get_text ? "Yes" : "No");
                    zx_text(screen, 10, t, z.get_text ? gameinfo::ATTR_TEXT : gameinfo::ATTR_HEADING);
                } else {
                    zx_text(screen, 10, "(no instructions in ZXDB)", gameinfo::ATTR_DIM);
                }
                if (h.scr[0] != '\0') {
                    snprintf(t, sizeof(t), "L-Loading screen     %s", z.get_scr ? "Yes" : "No");
                    zx_text(screen, 11, t, z.get_scr ? gameinfo::ATTR_TEXT : gameinfo::ATTR_HEADING);
                } else {
                    zx_text(screen, 11, "(no loading screen in ZXDB)", gameinfo::ATTR_DIM);
                }
                if (h.instr[0] != '\0') {
                    zx_text(screen, 13, "The instructions replace what", gameinfo::ATTR_DIM);
                    zx_text(screen, 14, "its details had (I in M).", gameinfo::ATTR_DIM);
                }
                zx_text(screen, 21, "S-Save", gameinfo::ATTR_KEYS);
                keys = "X-Back to the list";
                break;
            }
            const bool avail = zxdb::available(h.avail);
            if (!avail) {
                zx_text(screen, 6, "Not to be downloaded: ZXDB says", gameinfo::ATTR_DIM);
                snprintf(t, sizeof(t), "\"%.28s\"", h.avail[0] != '\0' ? h.avail : "not known");
                zx_text(screen, 7, t, gameinfo::ATTR_TEXT);
            } else if (z.entry.nfiles == 0) {
                zx_text(screen, 6, "No tape or snapshot to download", gameinfo::ATTR_DIM);
            } else {
                zx_text(screen, 6, "Download to /Downloads:", gameinfo::ATTR_DIM);
                for (int i = 0; i < z.entry.nfiles && i < 8; i++) {
                    const zxdb::File& f = z.entry.files[i];
                    char kind[12];
                    snprintf(kind, sizeof(kind), "%s", f.kind);
                    char* sp = strchr(kind, ' ');
                    if (sp != nullptr) *sp = '\0';          // "Tape image" -> "Tape"
                    snprintf(t, sizeof(t), "%d %-20.20s %.8s", i + 1, zxdb::base_of(f.path), kind);
                    zx_text(screen, 7 + i, t, gameinfo::ATTR_TEXT);
                }
            }
            if (z.said[0] != '\0') zx_text(screen, 17, z.said, gameinfo::ATTR_HEADING);
            if (z.asking != 0) {
                // the name it would have: the title picked
                char m[44];
                zx_text(screen, 17, "Match search selection?", gameinfo::ATTR_HEADING);
                if (zx_match_name(m, sizeof(m))) {
                    snprintf(t, sizeof(t), "(\"%.27s\")", m);
                    zx_text(screen, 18, t, gameinfo::ATTR_TEXT);
                }
                zx_text(screen, 21, "Y-Yes  N-No (as ZXDB names it)", gameinfo::ATTR_KEYS);
                keys = "X-Back";
                break;
            }
            snprintf(t, sizeof(t), "%s%s", avail && z.entry.nfiles > 0 ? "1-8 Download  " : "", h.scr[0] != '\0' ? "L-Loading screen" : "");
            zx_text(screen, 21, t, gameinfo::ATTR_KEYS);
            keys = "X-Back to the list";
            break;
        }
        default:
            break;
    }
    if (z.msg != nullptr && z.mode != ZX_NOTE) {
        // (under the typing on the search screen, which has the credits at the foot)
        const int row = z.mode == ZX_TYPE ? 10 : 18;
        zx_text(screen, row, z.msg, gameinfo::ATTR_KEYS);
        if (z.detail[0] != '\0') zx_text(screen, row + 1, z.detail, gameinfo::ATTR_DIM);
        if (z.detail[0] != '\0' && z.io[0] != '\0') zx_text(screen, row + 2, z.io, gameinfo::ATTR_DIM);
    }
    zx_text(screen, 22, keys, gameinfo::ATTR_KEYS);
    preview_screen = screen;
}

// Y on a title for the pokes: its .pok downloaded as Poke List/<game>.pok
__attribute__((noinline)) static void zx_pok_download() {
    ZxState& z = zx();
    const zxdb::Hit& h = z.search.hits[z.hit];
    if (!zxdb::file_url(h.instr, z.url, sizeof(z.url))) {
        z.msg = "PicoMenu can't get its pokes";
        return;
    }
    uint8_t made = make_sd_folder(POKE_LIST_DIR);
    walk_stop();
    if ((made != fsops::OK && made != fsops::NAME_IN_USE) ||
            (size_t) snprintf(z.path, sizeof(z.path), "%s/%s.pok", POKE_LIST_DIR, z.game) >= sizeof(z.path) ||
            !z.file.open(Drive::EXTERNAL_SD_CARD, z.path, false)) {
        z.msg = "Could not write to the SD card";
        return;
    }
    snprintf(z.name, sizeof(z.name), "%s", zxdb::base_of(h.instr));
    z.file_open = true;
    zx_ask(ZW_FILE, ZX_PICKED);
}

// Z on the Poke screen: the game last loaded looked up for its pokes; not if it has a .pok
// already (one the Poke list would read: pok_find)
static void zx_pok_begin() {
    ZxState& z = zx();
    const bool known = reload_known && reload_name[0] != '\0';
    if (known && !pok_find(z.path, sizeof(z.path))) {
        zx_begin(reload_name, true);
        return;
    }
    zx_begin(nullptr, true);                // (no WiFi: just the note)
    z.msg = known ? "Poke file already exists" : "Load a game first: its pokes\nare looked up by its name.";
    z.mode = ZX_NOTE;
}

// Starts a ZXDB screen: from the WiFi screen, or for the game information (game: its name)
static void zx_begin(const char* game, bool pok, bool solo) {
    ZxState& z = zx();
    memset(&z.typed, 0, sizeof(z.typed));
    z.said[0] = z.house[0] = z.year[0] = z.genre[0] = '\0';
    z.detail[0] = '\0';
    z.msg = nullptr;
    z.file_open = false;
    z.search.n = 0;
    z.search.total = 0;
    z.hit = 0;
    z.page = 0;
    z.info = game != nullptr && !pok;
    z.pok = pok;
    z.solo = solo;
    z.asking = 0;
    z.match = false;
    z.mode = ZX_TYPE;
    zx_live = true;
    if (game == nullptr && !solo) return;
    if (game != nullptr) {
        snprintf(z.game, sizeof(z.game), "%s", game);
        if (screengrab::base_name(game, z.base) == 0) snprintf(z.base, sizeof(z.base), "Screen");
        zxdb::name_to_search(game, z.typed, sizeof(z.typed));
    }
    // the WiFi is started at a later poll (not in the request that may have just stopped the
    // Bluetooth), and the search waits a second after it is up
    z.mode = ZX_JOIN;
    z.wifi_asked = false;
    z.join_ms = now_ms_zx();
    z.up_ms = 0;
#ifdef WIFI_ENABLE
    bt::note(0x50);
#endif
}

// A key (0: none, the screen polled): 0 as it is, 2 shown again, 1 closed
__attribute__((noinline)) static uint8_t zx_key(uint8_t key) {
    ZxState& z = zx();
    const uint8_t was = z.mode;
    bool redraw = false;
    const bool leave = z.mode == ZX_TYPE ? is_key_break(key) : is_key_back(key);     // (X is typed in the search)
    if (key != 0) {
        z.msg = nullptr;
        z.detail[0] = '\0';
        redraw = true;
    }
    switch (z.mode) {
        case ZX_JOIN: {
            if (leave) { zx_info_end(false); return 1; }
            const uint32_t now = now_ms_zx();
            if (!z.wifi_asked) {
                if (now - z.join_ms < ZX_START_MS) break;
                z.wifi_asked = true;
                const char* why = zx_wifi_start();
                if (why != nullptr) {
                    z.msg = why;
                    z.mode = ZX_NOTE;
                }
                break;
            }
#ifdef WIFI_ENABLE
            if (wifi::state() == wifi::FAILED) {
                snprintf(z.said, sizeof(z.said), "%s", wifi::why());
                z.msg = z.said;
                z.mode = ZX_NOTE;
                break;
            }
#endif
            if (zx_wifi_up()) {
                if (z.up_ms == 0) {
                    z.up_ms = now | 1;
#ifdef WIFI_ENABLE
                    bt::note(0x52);
#endif
                } else if (now - z.up_ms >= ZX_SETTLE_MS) {
                    if (z.typed[0] != '\0') zx_search(0, ZX_TYPE);
                    else z.mode = ZX_TYPE;  // (Z on the menu: a title to type)
                }
            }
            break;
        }
        case ZX_NOTE:
            if (key != 0) {
                if (z.info || z.pok || z.solo) { zx_info_end(false); return 1; }
                z.mode = ZX_TYPE;
            }
            break;
        case ZX_TYPE: {
            size_t len = strlen(z.typed);
            if (leave) {
                if (z.info || z.pok || z.solo) { zx_info_end(false); return 1; }
                zx_live = false;
                return 1;
            } else if (key == notes::KEY_DELETE) {
                if (len > 0) z.typed[len - 1] = '\0';
            } else if (key == notes::KEY_ENTER || key == 13) {
                if (len > 0) zx_search(0, ZX_TYPE);
            } else if (key >= ' ' && key < 127 && len + 1 < sizeof(z.typed)) {
                z.typed[len] = (char) key;
                z.typed[len + 1] = '\0';
            }
            break;
        }
        case ZX_ASK:
            if (leave) {
                net::stop();
                zx_file_drop();
                z.msg = "Stopped";
                z.mode = z.back;
                break;
            }
            if (net::state() == net::DONE || net::state() == net::FAILED) {
                zx_answered();
                redraw = true;
            } else if (z.want == ZW_FILE && (uint32_t) (net::received() / 1024) != z.shown_kb) {
                z.shown_kb = (uint32_t) (net::received() / 1024);
                redraw = true;                  // (the loading screen comes into the screen: not drawn)
            } else if (z.want != ZW_SCREEN) {
                uint32_t step = (uint32_t) net::stage() * 10000 + net::elapsed_ms() / 1000;
                if (step != z.shown_step) {
                    z.shown_step = step;
                    redraw = true;              // where it has got to, and the seconds
                }
            }
            break;
        case ZX_UNZIP:
            zx_unzip();
            redraw = true;
            break;
        case ZX_RESULTS: {
            int pages = (int) ((z.search.total + zxdb::PAGE - 1) / zxdb::PAGE);
            if (leave || key == 't' || key == 'T') {
                z.mode = ZX_TYPE;
            } else if (key >= '0' && key <= '9') {
                int i = key == '0' ? 9 : key - '1';
                if (i < z.search.n) {
                    z.hit = (int8_t) i;
                    if (z.pok) {
                        z.mode = ZX_PICKED;     // (download its .pok? Y or N)
                    } else if (z.info) {
                        const zxdb::Hit& h = z.search.hits[i];
                        snprintf(z.house, sizeof(z.house), "%s", h.house);
                        snprintf(z.year, sizeof(z.year), "%s", h.year);
                        snprintf(z.genre, sizeof(z.genre), "%s", h.genre);
                        z.get_text = z.get_scr = false;
                        z.mode = ZX_PICKED;
                    } else if (zxdb::entry_url(z.search.hits[i].id, z.url, sizeof(z.url))) {
                        zx_ask(ZW_ENTRY, ZX_RESULTS);
                    }
                }
            } else if ((key == 'n' || key == 'N') && pages > 1) {
                zx_search((uint16_t) ((z.page + 1) % pages), ZX_TYPE);
            } else if ((key == 'p' || key == 'P') && pages > 1) {
                zx_search((uint16_t) ((z.page + pages - 1) % pages), ZX_TYPE);
            }
            break;
        }
        case ZX_ENTRY:
            if (z.asking != 0) {
                // "Match search selection?": Y names it after the title picked, N as ZXDB has it
                if (key == 'y' || key == 'Y' || key == 'n' || key == 'N') {
                    z.match = key == 'y' || key == 'Y';
                    const uint8_t k = z.asking;
                    z.asking = 0;
                    z.said[0] = '\0';
                    if (k == 'l') {
                        if (zxdb::file_url(z.entry.info.scr, z.url, sizeof(z.url))) zx_ask(ZW_SCREEN, ZX_ENTRY);
                    } else {
                        zx_download(k - '1');
                    }
                } else if (key != 0) {
                    z.asking = 0;               // (any other key: not done)
                }
            } else if (leave) {
                z.mode = ZX_RESULTS;
            } else if (key >= '1' && key <= '8' && key - '1' < z.entry.nfiles && zxdb::available(z.entry.info.avail)) {
                z.asking = key;
            } else if ((key == 'l' || key == 'L') && z.entry.info.scr[0] != '\0') {
                z.asking = 'l';
            }
            break;
        case ZX_PICKED: {
            const zxdb::Hit& h = z.search.hits[z.hit];
            if (z.pok) {
                if (leave || key == 'n' || key == 'N') z.mode = ZX_RESULTS;
                else if ((key == 'y' || key == 'Y') && h.instr[0] != '\0') zx_pok_download();
                break;
            }
            if (leave) {
                z.mode = ZX_RESULTS;
            } else if (key == 's' || key == 'S') {
                z.said[0] = '\0';
                z.text_done = z.scr_done = z.text_got = z.scr_saved = false;
                zx_picked_next();               // (I and L's, then saved)
            } else if ((key == 'i' || key == 'I') && h.instr[0] != '\0') {
                z.get_text = !z.get_text;
            } else if ((key == 'l' || key == 'L') && h.scr[0] != '\0') {
                z.get_scr = !z.get_scr;
            }
            break;
        }
        case ZX_PICTURE:
            if (key != 0) {
                if (z.info) zx_picked_next();   // (the next chosen, or saved)
                else z.mode = ZX_ENTRY;
            }
            break;
        default:
            break;
    }
    if (z.mode == ZX_FINISH) {
        zx_info_end(true);
        return 1;
    }
    if (z.mode != was) redraw = true;
    return redraw ? 2 : 0;
}

// The information's lookup, in op 6 (the ROM sends each key, and 0 while none is pressed)
__attribute__((noinline)) static void zx_info_step(uint8_t key) {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer);
    uint8_t r = zx_key(key);
    if (r == 1) {
        nmi_game_info_show(false);          // the M menu again (the ROM goes back to op 3)
        return;
    }
    zx_draw(screen);                        // (the ROM shows it again each time, in op 6)
    nmi_rom_data[START_KEYREPEAT] = zx().mode == ZX_TYPE ? 1 : 0;   // (typing: a held key repeats)
    nmi_rom_data[1] = 6;
    nmi_rom_data[0] = 1;
}
#endif

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
static void nmi_batch_info(int op, uint8_t key);
static void nmi_action_game_info() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    // no software house to scroll, unless the first page says so. All of it is cleared: the ROM
    // keeps its scroll position and would show the rest of an old name from there (over a
    // picture shown full screen)
    memset(nmi_rom_data + INFO_HOUSE, 0, INFO_HOUSE_SIZE);
    info_scroll_start(false);               // (every screen shown as the information: SD setup, the converter too)
    int op = nmi_rom_data[5];
    if (nmi_rom_data[2] == 2) {         // C on the menu: set up the SD card
        notes_editing = notes_edit = false;
        nmi_sd_setup(op);
        return;
    }
    if (nmi_rom_data[2] == 3) {         // the batch converter, running
        notes_editing = notes_edit = false;
        nmi_batch_info(op, nmi_rom_data[6]);
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
#if PICOMENU_NET
    if (zx_live && zx().info) {
        if (op == 6) {
            zx_info_step(nmi_rom_data[6]);      // Z in the M menu: the ZXDB lookup's screen
            return;
        }
        zx_live = false;                        // (left some other way)
        web_off();
    }
#endif
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
#if PICOMENU_NET
        if (zx_due) {
            zx_due = false;                     // Z in the M menu: the lookup starts (in op 6)
            zx_begin(name);
            zx_info_step(0);
            return;
        }
#endif
    }
    nmi_game_info_show(ended);
}

// ACTION_SCREEN_GRAB: saves the game's screen (as it was when the menu opened) in
// /Scraper/Screen Grabs on the SD card (made if it is not there), named after the game last
// loaded (without " Save Game"), or "Screen" if there is none, and the category:
// <game> Loading Screen.scr, or <game> Loading Screen1.scr ... if the name is taken (the lowest
// number free), and so on; <game>.scr, <game>1.scr ... for Standard Screen Number.
// WORD param1 = where to put the message "Saved as <name>"
// BYTE param2 = the category: 0 Standard Screen Number (a double press), 1 Loading Screen,
//               2 Menu Screen, 3 Game Screen, 4 Ending Screen (G asks)
// Status 0 saved, 1 no SD card (or the PC has it), 2 could not save, 3 too many grabs of
// the game, 4 there is no screen to save.
// Saves the stashed screen as a grab of the game last loaded, in category cat. The message
// "Saved as <name>" goes in message (at least 36 bytes). Status as ACTION_SCREEN_GRAB's.
static uint8_t grab_save(screengrab::Category cat, char* message) {
    if (!sd_card_mounted) {
        return 1;
    }
    if (!screen_stashed) {
        return 4;
    }
    char base[screengrab::NAME_MAX + 1];
    if (!reload_known || game_base(reload_dir, reload_name, base) == 0) {      // a slot: its game's
        snprintf(base, sizeof(base), "Screen");
    }
    if (!grab_folder_make()) {
        set_last_error("Cannot make", "Scraper/Screen Grabs");
        return 2;
    }
    int number = grab_free_number(base, cat);
    if (number < 0) {
        return 3;
    }
    FileHandle f;
    if (!grab_write(f, base, cat, number, screen_stash)) {
        set_last_error("Cannot save", "screen grab");
        return 2;
    }
    screengrab::saved_message(base, cat, number, message, 36);
    LOG("Screen grab %s\n", grab_path);
    return 0;
}

static void nmi_action_screen_grab() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t dest = nmi_rom_data[2] | (nmi_rom_data[3] << 8);
    char* message = reinterpret_cast<char*>(nmi_rom_data) + dest;
    screengrab::Category cat = nmi_rom_data[4] < screengrab::GRAB_CATEGORIES ? (screengrab::Category) nmi_rom_data[4] : screengrab::STANDARD;
    if (dest > 16384 - 40) {
        nmi_rom_data[0] = 2;
        return;
    }
    nmi_rom_data[0] = grab_save(cat, message);
}

// ACTION_GRAB_MENU (G on the menu): which category, 1-5, then it is saved and the result shown;
// BREAK or X goes back. BYTE param1 = the key (0: opened). Status 0 the screen stays, 1 closed
static uint8_t grab_menu_state = 0;         // 0 asking, 1 saved (any key closes)
__attribute__((noinline)) static void nmi_action_grab_menu() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t key = nmi_rom_data[2];
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer);
    static char result[40];
    if (key == 0) {
        grab_menu_state = 0;
    } else if (grab_menu_state == 1 || key == 0x18 || key == 'x') {
        nmi_rom_data[0] = 1;
        return;
    } else if (key >= '1' && key <= '5') {
        // 1 Loading Screen ... 4 Ending Screen, 5 Standard Screen Number
        screengrab::Category cat = key == '5' ? screengrab::STANDARD : (screengrab::Category) (key - '0');
        uint8_t status = grab_save(cat, result);
        static const char* const WHY[] = { "", "No SD card (or the PC has it)", "Could not save the grab",
            "Too many grabs", "No screen to save" };
        if (status != 0) snprintf(result, sizeof(result), "%s", WHY[status < 5 ? status : 2]);
        grab_menu_state = 1;
    }
    memset(screen, 0, 6144);
    memset(screen + 6144, ui_attr(0x07), 768);
    memset(screen + 6144, gameinfo::ATTR_BANNER, 32);
    help_put_text(screen, 0, 1, "Screen grab", 11, gameinfo::ATTR_BANNER);
    if (grab_menu_state == 0) {
        static const char* const LINES[] = { "Save it as:", "", "1  Loading Screen", "2  Menu Screen",
            "3  Game Screen", "4  Ending Screen", "5  Standard Screen Number", "   (<game>.scr, <game>1.scr...)" };
        for (int k = 0; k < 8; k++) {
            help_put_text(screen, 2 + k, 1, LINES[k], (int) strlen(LINES[k]), k == 0 ? gameinfo::ATTR_HEADING : gameinfo::ATTR_TEXT);
        }
        help_put_text(screen, 12, 1, "In /Scraper/Screen Grabs", 24, gameinfo::ATTR_DIM);
        help_put_text(screen, 23, 1, menu_alone() ? "1-5 Save  X-Exit" : "1-5 Save  X-Back", 16, gameinfo::ATTR_KEYS);
    } else {
        help_put_text(screen, 3, 1, result, (int) strlen(result), gameinfo::ATTR_TEXT);
        help_put_text(screen, 23, 1, "Press any key", 13, gameinfo::ATTR_KEYS);
    }
    preview_screen = screen;
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
    memset(screen + 6144, ui_attr(0x07), 768);
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
#ifdef WIFI_ENABLE
        case fwupdate::WRONG_BOARD: return "Not the Pico W firmware";
#else
        case fwupdate::WRONG_BOARD: return "It is the Pico W firmware";
#endif
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
#ifdef WIFI_ENABLE
        e = fw_checker.finish(true);
#else
        e = fw_checker.finish(false);
#endif
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
    memset(screen + 6144, ui_attr(0x07), 768);
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

static void nmi_action_tape_position();
static void nmi_action_tape_insert();
static void nmi_action_text_file();
static void nmi_action_assets();
static void nmi_action_snap_hide();
static void nmi_action_waste_bin();
static void nmi_action_bin_empty();
static void nmi_action_grab_menu();
static void nmi_action_slots();
static void nmi_action_slot_ask();
static void nmi_action_options();
static void nmi_action_batch();


static bool list_pool_busy = false;         // a screen (Options, the batch's folders) keeps its state in list_pool
static bool menu_shot_piece();
// The NMI ROM's fast copy for saving (gencopy in nmirom.asm): SAVE128_GEN_BLOCK / 2 of "POP HL /
// LD (nn),HL", each putting two bytes in the data area at the start of spare_space, then
// "LD SP,(gencopy_sp) / POP HL / RET". Written each time a save begins (the area is shared).
static void gencopy_write(uint8_t* nmi_rom_data) {
    uint8_t* c = nmi_rom_data + SAVE128_GEN_CODE;
    for (uint32_t k = 0; k < SAVE128_GEN_BLOCK / 2; k++) {
        uint16_t to = (uint16_t) (SAVE128_GEN_DATA + 2 * k);
        *c++ = 0xE1;                            // POP HL
        *c++ = 0x22;                            // LD (to),HL
        *c++ = (uint8_t) (to & 0xFF);
        *c++ = (uint8_t) (to >> 8);
    }
    *c++ = 0xED;                                // LD SP,(gencopy_sp)
    *c++ = 0x7B;
    *c++ = (uint8_t) (SAVE128_GEN_SP & 0xFF);
    *c++ = (uint8_t) (SAVE128_GEN_SP >> 8);
    *c++ = 0xE1;                                // POP HL (HL moved on)
    *c = 0xC9;                                  // RET
}
static_assert(SAVE128_GEN_CODE + SAVE128_GEN_BLOCK * 2 + 6 <= 16384, "gencopy's code fits");

// The hash of the end of the 128K bank paged in (ACTION_SNA_HASH), as the NMI ROM sends it
static uint16_t save_hash1 = 0, save_hash2 = 0;

// ---- Hotkeys (Picomenu.ini's nmi_button = hotkeys) ----
// The button sends the NMI as soon as it goes down, with hotkeys (nmirom.asm) in place of the
// menu: a key pressed while it is held opens that straight away (ACTION_HOTKEY), and closing it
// goes back to the game; let go with no key, it is the menu as always. With the menu up a press
// saves the menu's screen (as a double press does with the original button). There is no long
// press reset (1 and 4 are hotkeys) and no hold or double press.
static bool hotkey_nmi = false;         // the next NMI is the button's: hotkeys goes in
bool button_press_action(bool held);

// ACTION_HOTKEY: the key (from the ROM's key table) and what it opens
__attribute__((noinline)) static void nmi_action_hotkey(uint8_t* nmi_rom_data) {
    static const struct { char key; uint16_t at; } KEYS[] = {
        { 'b', HK_BROWSE }, { 'f', HK_BROWSE }, { 'z', HK_ZXDB }, { 'i', HK_INFO }, { 's', HK_SAVE },
        { 'g', HK_GRAB }, { '1', HK_RESET128 }, { '4', HK_RESET48 }, { 'v', HK_TAPE },
        { 'a', HK_AUTOSAVE }, { 'p', HK_POKE } };
    uint8_t k = nmi_rom_data[2];
    if (k >= 'A' && k <= 'Z') k = (uint8_t) (k + 32);
    uint8_t status = k == 'h' ? 2 : k == ' ' ? 3 : 0;
#if PICOMENU_HISCORE
    if (k == 't') {
        // T: the high score table (A - Autosave's T)
        status = 1;
        nmi_rom_data[3] = (uint8_t) (HK_AUTOSAVE & 0xFF);
        nmi_rom_data[4] = (uint8_t) (HK_AUTOSAVE >> 8);
        hs_open_direct = true;
    }
#endif
    for (const auto& e : KEYS) {
        if (e.key == k) {
            status = 1;
            nmi_rom_data[3] = (uint8_t) (e.at & 0xFF);
            nmi_rom_data[4] = (uint8_t) (e.at >> 8);
        }
    }
    if (k == 'f') {
        // Browse Games in the Favourites folder (A in Browse Games copies a game there): the SD
        // card's, else the flash's; neither, Browse Games where it would start
        for (uint8_t d = 1; ; d = 0) {
            if ((d == 0 || sd_card_mounted) && fsops::kind(fsops::drive_of(d), FAVOURITES_DIR) == fsops::KIND_DIR) {
                snprintf(d == 1 ? sd_cwd : flash_cwd, sizeof(sd_cwd), "%s", FAVOURITES_DIR);
                nmi_rom_data[START_DRIVE] = d;
                break;
            }
            if (d == 0) break;
        }
    }
    nmi_rom_data[0] = status;
}

// The button (debounced) went down or up, with Picomenu.ini's nmi_button = hotkeys
static void hotkeys_button(bool down) {
    rom_data[16384 + HK_HELD] = down ? 1 : 0;
    if (!down) return;
    if (rom_state.nmi_active || (rom_state.flags & 2) != 0) {
        button_press_action(false);         // the menu's screen saved (as a .png)
        return;
    }
    hotkey_nmi = true;
    if (!sendNmiRequest(0, 0, 0)) hotkey_nmi = false;
    rom_data[16384 + HK_HELD] = 1;          // (sendNmiRequest unpacked the ROM afresh)
}

#ifdef PICOFACE_HOST
void host_hotkeys_button(bool down) { hotkeys_button(down); }     // the simulator's hkbutton
#endif

// ---- The autosave's copy (cpsave in nmirom.asm) ----
// cpsave (put in place of the menu's code for an autosave, see sendNmiRequest) asks for the
// copy a block at a time: the code written at CP_CODE copies up to CP_DATA_SIZE bytes of the
// Spectrum's memory into CP_DATA with POP and PUSH, 14 bytes a group, and the block is put in
// snap_load_buffer when it has run (ACTION_CP_NEXT). The memory is in pieces (cp_seg): a 48K
// snapshot is 0x4000 to 0xFFFF after the .sna header; a 128K one is banks 5 and 2, the bank
// paged in at 0xC000 as it is, then the others paged in there, each where KIND_PAGED has it.
static struct {
    struct Seg { uint16_t from; uint8_t page; uint32_t to, len; } seg[8];    // page: 0xFF none
    uint8_t segs, at;
    uint32_t done;                          // bytes of seg[at] copied
    uint32_t block_to, block_len;           // the block the code copies now: where it goes
    int8_t bank;                            // the 128K bank at 0xC000, -1 for a 48K snapshot
    uint8_t port;                           // the 7FFD value for the game (128K)
} cp;

static int8_t cp_bank_now() { return cp.bank; }
static uint8_t cp_port_now() { return cp.port; }

// Writes the copy of the next block at CP_CODE (from cp.seg[cp.at], cp.done): false if all is sent
static bool cp_write_block(uint8_t* nmi_rom_data) {
    while (cp.at < cp.segs && cp.done >= cp.seg[cp.at].len) {
        cp.at++;
        cp.done = 0;
    }
    if (cp.at >= cp.segs) return false;
    const auto& g = cp.seg[cp.at];
    uint32_t n = g.len - cp.done;
    if (n > CP_DATA_SIZE / 14 * 14) n = CP_DATA_SIZE / 14 * 14;
    uint16_t from = (uint16_t) (g.from + cp.done);
    uint8_t* c = nmi_rom_data + CP_CODE;
    auto word = [&](uint16_t w) { *c++ = (uint8_t) (w & 0xFF); *c++ = (uint8_t) (w >> 8); };
    if (cp.done == 0 && g.page != 0xFF) {
        *c++ = 0x3E; *c++ = g.page;                 // LD A,page
        *c++ = 0x01; word(0x7FFD);                  // LD BC,0x7FFD
        *c++ = 0xED; *c++ = 0x79;                   // OUT (C),A
    }
    *c++ = 0xED; *c++ = 0x73; word(CP_SP);          // LD (CP_SP),SP
    static const uint8_t POPS[7] = { 0xF1, 0xC1, 0xD1, 0xE1, 0xC1, 0xD1, 0xE1 };   // AF BC DE HL, then BC' DE' HL'
    uint16_t to = CP_DATA;
    for (uint32_t left = n; left > 0; ) {
        uint32_t pairs = left >= 14 ? 7 : left / 2;
        *c++ = 0x31; word(from);                    // LD SP,from
        for (uint32_t i = 0; i < pairs; i++) {
            if (i == 4) *c++ = 0xD9;                // EXX
            *c++ = POPS[i];
        }
        *c++ = 0x31; word((uint16_t) (to + 2 * pairs));     // LD SP,to + the group
        for (uint32_t i = pairs; i-- > 0; ) {
            *c++ = (uint8_t) (POPS[i] + 4);         // PUSH (the last popped first)
            if (i == 4) *c++ = 0xD9;                // EXX
        }
        from = (uint16_t) (from + 2 * pairs);
        to = (uint16_t) (to + 2 * pairs);
        left -= 2 * pairs;
    }
    *c++ = 0xED; *c++ = 0x7B; word(CP_SP);          // LD SP,(CP_SP)
    *c = 0xC9;                                      // RET
    cp.block_to = g.to + cp.done;
    cp.block_len = n;
    cp.done += n;
    return true;
}
static_assert(CP_CODE + 7 + 4 + (CP_DATA_SIZE / 14) * CP_GROUP_CODE + 5 <= CP_LIMIT, "the copy's code fits");

// ACTION_CP_BEGIN: the header, and the copy of the first block
static void nmi_action_cp_begin(uint8_t* nmi_rom_data) {
    uint16_t headeroffset = (uint16_t) ((nmi_rom_data[3] << 8) | nmi_rom_data[2]);
    uint8_t bank = nmi_rom_data[4];
    uint8_t* buf = reinterpret_cast<uint8_t*>(snap_load_buffer);
    sna_in_buffer = true;
    snap_is_paged = false;
    cp.segs = cp.at = 0;
    cp.done = 0;
    if (bank > 7) {
        save_is_128k = false;
        cp.bank = -1;
        memcpy(buf, nmi_rom_data + headeroffset, SNA_HEADER_SIZE);
        cp.seg[cp.segs++] = { 0x4000, 0xFF, SNA_HEADER_SIZE, 49152 };
        current_sna_write_offset = SNA_FILE_SIZE;
    } else {
        save_is_128k = true;
        cp.bank = (int8_t) bank;
        memcpy(save128_info, nmi_rom_data + headeroffset, sizeof(save128_info));
        // the port's value as it will be saved (its screen and ROM bits are a guess, see
        // choose_7ffd): its screen bit while the other banks are paged in, so the game's screen
        // stays on the TV
        cp.port = snapload::choose_7ffd(buf, save128_info, &loaded_paging, bank);
        const uint8_t keep = cp.port & 0x18;
        auto add = [&](uint16_t from, uint8_t page, uint32_t b) {
            cp.seg[cp.segs++] = { from, page, (uint32_t) (snapload::bank_memory(buf, b) - buf), snapload::PAGE_SIZE };
        };
        add(0x4000, (uint8_t) (keep | bank), 5);
        add(0x8000, 0xFF, 2);
        if (bank != 5 && bank != 2) add(0xC000, 0xFF, bank);
        for (uint8_t b : snapload::PAGE_ORDER) {
            if (b != 5 && b != 2 && b != bank) add(0xC000, (uint8_t) (keep | b), b);
        }
        current_sna_write_offset = snapload::BUFFER128_SIZE;
    }
    cp_write_block(nmi_rom_data);
}

// ACTION_CP_NEXT: the block is in CP_DATA. Status 0 with the next one's copy written, or 1 when
// all is sent (param1 the port's value, param2 1 to write it)
static void nmi_action_cp_next(uint8_t* nmi_rom_data) {
    uint8_t* buf = reinterpret_cast<uint8_t*>(snap_load_buffer);
    memcpy(buf + cp.block_to, nmi_rom_data + CP_DATA, cp.block_len);
    if (cp_write_block(nmi_rom_data)) {
        nmi_rom_data[0] = 0;
        return;
    }
    if (cp.bank >= 0) {
        uint8_t port = snapload::finish_save128(buf, save128_info, &loaded_paging, cp.bank);
        nmi_rom_data[2] = port;
        nmi_rom_data[3] = 1;
    } else {
        nmi_rom_data[3] = 0;
    }
    nmi_rom_data[0] = 1;
}

// A snapshot being sent (an autosave, or a save from the menu): the main loop serves each of
// its requests as soon as it comes (not after its 1 ms sleep and the other things it does) until
// the snapshot is saved, or nothing has come for SAVE_STREAM_IDLE_US
static bool save_stream = false;
static uint64_t save_stream_at = 0;
static const uint64_t SAVE_STREAM_IDLE_US = 100000;
static void save_stream_start() {
#if PICOMENU_CP_OLD
    return;                                 // (a test build: Rev 2.5.0's way)
#endif
    save_stream = true;
    save_stream_at = time_us_64();
}

void processNmiRequest() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t action = nmi_rom_data[1];
    LOG("Action to process: %d\n", action);
#if PICOMENU_BT
    if (bt_on && !bt_keeps(action)) bt_off();      // its memory is the end of snap_load_buffer
#endif
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
        sna_in_buffer = true;
        uint16_t headeroffset =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
        LOG("Start SNA save, head destination: %X\n", headeroffset);
        snap_is_paged = false;
        save_is_128k = false;
        memcpy(snap_load_buffer, nmi_rom_data + headeroffset, SNA_HEADER_SIZE);
        current_sna_write_offset = SNA_HEADER_SIZE;
        gencopy_write(nmi_rom_data);
        save_stream_start();
    }  else if (action == ACTION_SNA_HASH) {
        uint16_t offset =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
        uint16_t count =  (nmi_rom_data[5] << 8) |  nmi_rom_data[4];
        for (uint32_t i = 0; i < count && offset + i < 16384; i++) {
            save_hash1 = (uint16_t) (save_hash1 + nmi_rom_data[offset + i]);
            save_hash2 = (uint16_t) (save_hash2 + save_hash1);
        }
        save128_info[snapload::SV_S1] = (uint8_t) (save_hash1 & 0xFF);
        save128_info[snapload::SV_S1 + 1] = (uint8_t) (save_hash1 >> 8);
        save128_info[snapload::SV_S2] = (uint8_t) (save_hash2 & 0xFF);
        save128_info[snapload::SV_S2 + 1] = (uint8_t) (save_hash2 >> 8);
    }  else if (action == ACTION_SNA_BEGIN_WRITE128) {
        sna_in_buffer = true;
        uint16_t headeroffset =  (nmi_rom_data[3] << 8) |  nmi_rom_data[2];
        LOG("Start 128K save, head destination: %X\n", headeroffset);
        snap_is_paged = false;
        save_is_128k = true;
        memcpy(save128_info, nmi_rom_data + headeroffset, sizeof(save128_info));
        if (nmi_menu_open) {
            // the menu turned the sound chip's volumes down (ayswap): the game's are kept apart
            memcpy(save128_info + SAVE128_AY_AT + 8, nmi_rom_data + NMI_AYVOLS, 3);
        }
        current_sna_write_offset = snapload::HEADER_AREA;
        save_hash1 = save_hash2 = 0;
        gencopy_write(nmi_rom_data);
        save_stream_start();
#if PICOMENU_HISCORE
    }  else if (action == ACTION_HS_CHECK) {
        nmi_action_hs_check(nmi_rom_data);
        return;
#endif
    }  else if (action == ACTION_HOTKEY) {
        nmi_action_hotkey(nmi_rom_data);
        return;
    }  else if (action == ACTION_CP_BEGIN) {
        nmi_action_cp_begin(nmi_rom_data);
    }  else if (action == ACTION_CP_NEXT) {
        nmi_action_cp_next(nmi_rom_data);
        return;
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
        if (rom_data[16384 + START_MENUSHOT] != 0 && menu_shot_piece()) {
            return;                         // a menu grab: the screen is not the game's (status set)
        }
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
    }  else if (action == ACTION_TAPE_POSITION) {
        nmi_action_tape_position();
        return;
    }  else if (action == ACTION_TAPE_INSERT) {
        nmi_action_tape_insert();
        return;
    }  else if (action == ACTION_TEXT_FILE) {
        nmi_action_text_file();
        return;
    }  else if (action == ACTION_ASSETS) {
        nmi_action_assets();
        return;
    }  else if (action == ACTION_SNAP_HIDE) {
        nmi_action_snap_hide();
        return;
    }  else if (action == ACTION_WASTE_BIN) {
        nmi_action_waste_bin();
        return;
    }  else if (action == ACTION_BIN_EMPTY) {
        nmi_action_bin_empty();
        return;
    }  else if (action == ACTION_GRAB_MENU) {
        nmi_action_grab_menu();
        return;
    }  else if (action == ACTION_SLOTS) {
        nmi_action_slots();
        return;
    }  else if (action == ACTION_WIFI) {
        nmi_action_wifi();
        return;
    }  else if (action == ACTION_CHECKPOINTS) {
        nmi_action_checkpoints();
        return;
    }  else if (action == ACTION_CHECKPOINT_ASK) {
        nmi_action_checkpoint_ask();
        return;
    }  else if (action == ACTION_SLOT_ASK) {
        nmi_action_slot_ask();
        return;
    }  else if (action == ACTION_OPTIONS) {
        nmi_action_options();
        return;
    }  else if (action == ACTION_BATCH) {
        nmi_action_batch();
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
        save_stream = false;
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

#ifdef PICOFACE_HOST
extern "C" void host_step(void);       // the simulator runs the Spectrum a little
#endif
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
#ifdef PICOFACE_HOST
            host_step();
#endif
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

// The ROM files (48.rom, 128.rom ...) are looked for in the root of the flash, then the SD
// card's ROMS folder (which Set up SD card makes, hidden), then the root of the card
static bool rom_file_open(FileHandle& f, const char* name) {
    if (f.open(Drive::INTERNAL_FLASH, name, true)) return true;
    if (!sd_card_mounted) return false;
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", ROMS_FOLDER, name);
    return f.open(Drive::EXTERNAL_SD_CARD, path, true) || f.open(Drive::EXTERNAL_SD_CARD, name, true);
}
static const uint32_t TAPE_MBOX = 0x3FF8;        // mailbox, replaces the (c) glyph
static const uint32_t TAPE_LDBYTES = 0x0556;     // LD-BYTES
static const uint32_t TAPE_HOOK_ADDR = 0x0562;   // the hook: LD-BYTES after its first 12 bytes

// Mailbox layout: +0 state (1 = request posted, 2 = ok, 3 = failed),
// +1 flag, +2/+3 destination, +4/+5 length, +6 mode (1 load, 0 verify)

// Z80 hook, at 0x0562.  The first 12 bytes of LD-BYTES (0x0556-0x0561: INC D, EX AF,AF',
// DEC D, DI, the border, PUSH SA/LD-RET) are left as they are, so a game that does those
// itself and jumps to 0x0562 (Out Run's and Gauntlet's level loaders do) finds the hook
// too; A' and F' then hold the flag byte and the carry (load or verify) either way.
// Assembled with sjasmplus (tools/picosim/tests/tapehook.asm):
//     EX AF,AF' / DI / LD (3FF9),A / LD (3FFA),IX / LD (3FFC),DE / SBC A,A / AND 1 /
//     LD (3FFE),A / ADD IX,DE / LD A,1 / LD (3FF8),A / wait: LD A,(3FF8) / CP 2 / JR C,wait /
//     CP 3 / PUSH AF / LD HL,glyph / LD DE,3FF8 / LD BC,8 / LDIR / LD DE,0 / POP AF / RET
// It ends with the 8 byte glyph that the hook writes back over the mailbox when it is
// finished.  It stays below LD-EDGE-2 (0x05E3).
static constexpr uint8_t TAPE_HOOK[] = {
    0x08, 0xF3, 0x32, 0xF9, 0x3F, 0xDD, 0x22, 0xFA, 0x3F, 0xED, 0x53, 0xFC,
    0x3F, 0x9F, 0xE6, 0x01, 0x32, 0xFE, 0x3F, 0xDD, 0x19, 0x3E, 0x01, 0x32,
    0xF8, 0x3F, 0x3A, 0xF8, 0x3F, 0xFE, 0x02, 0x38, 0xF9, 0xFE, 0x03, 0xF5,
    0x21, 0x96, 0x05, 0x11, 0xF8, 0x3F, 0x01, 0x08, 0x00, 0xED, 0xB0, 0x11,
    0x00, 0x00, 0xF1, 0xC9, 0x3C, 0x42, 0x99, 0xA1, 0xA1, 0x99, 0x42, 0x3C
};
static_assert(TAPE_HOOK_ADDR + sizeof(TAPE_HOOK) - 8 == 0x0596, "the hook's LD HL,glyph");
static_assert(TAPE_HOOK_ADDR + sizeof(TAPE_HOOK) <= 0x05E3, "LD-EDGE-2 must stay");

// The hook for a tape put in (T): the same, with a third answer, 4: the block comes from the EAR
// socket (B - Bluetooth tape plays it), so the ROM's own LD-BYTES is to load it. A', F', IX and
// DE are put back, what 0x0562-0x056A do is done in the hook (0x0562 is where the bus handler
// takes over), and its RET goes on at 0x056B (LD-BREAK) in the internal ROM. It is only served
// with the internal ROM (rom_data holds it at 0x0563, after a NOP): in the patched 48K ROM the
// longer hook would cover LD-BYTES's own code from 0x056B, so that one keeps TAPE_HOOK.
// (tools/picosim/tests/tapehook.asm, assembled at 0x0562: its addresses are moved on by one)
static constexpr uint8_t TAPE_IN_HOOK[] = {
    0x08, 0xF3, 0x32, 0xF9, 0x3F, 0xDD, 0x22, 0xFA, 0x3F, 0xED, 0x53, 0xFC,
    0x3F, 0x9F, 0xE6, 0x01, 0x32, 0xFE, 0x3F, 0x3E, 0x01, 0x32, 0xF8, 0x3F,
    0x3A, 0xF8, 0x3F, 0xFE, 0x02, 0x38, 0xF9, 0xFE, 0x04, 0x28, 0x1D, 0xDD,
    0x19, 0xFE, 0x03, 0xF5, 0x21, 0x9A, 0x05, 0x11, 0xF8, 0x3F, 0x01, 0x08,
    0x00, 0xED, 0xB0, 0x11, 0x00, 0x00, 0xF1, 0xC9, 0x3C, 0x42, 0x99, 0xA1,
    0xA1, 0x99, 0x42, 0x3C, 0x3A, 0xFE, 0x3F, 0x1F, 0x14, 0x3A, 0xF9, 0x3F,
    0x08, 0x15, 0xD5, 0x21, 0x9A, 0x05, 0x11, 0xF8, 0x3F, 0x01, 0x08, 0x00,
    0xED, 0xB0, 0xD1, 0x21, 0x6B, 0x05, 0xE5, 0xDB, 0xFE, 0x1F, 0xE6, 0x20,
    0xF6, 0x02, 0x4F, 0xBF, 0x18, 0xD1
};
static const uint32_t TAPE_IN_HOOK_RET = 0x37;          // its RET (the bus handler hands back there)
static const uint32_t TAPE_IN_HOOK_GLYPH = 0x38;
static const uint32_t TAPE_IN_HOOK_LDHL[2] = { 0x28, 0x4B };    // its two LD HL,glyph
static_assert(TAPE_IN_HOOK[TAPE_IN_HOOK_RET] == 0xC9 && TAPE_IN_HOOK[TAPE_IN_HOOK_GLYPH] == 0x3C, "RET, then the glyph");
static_assert(TAPE_IN_HOOK[0x28] == 0x21 && TAPE_IN_HOOK[0x29] == 0x9A && TAPE_IN_HOOK[0x4B] == 0x21 &&
    TAPE_IN_HOOK[0x4C] == 0x9A, "LD HL,glyph (0x059A as assembled)");
static_assert(TAPE_IN_HOOK[0x57] == 0x21 && TAPE_IN_HOOK[0x58] == 0x6B && TAPE_IN_HOOK[0x59] == 0x05, "LD HL,056B");
static_assert(TAPE_HOOK_ADDR + 1 + sizeof(TAPE_IN_HOOK) <= 0x05E3, "below LD-EDGE-2");

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
// E on the tape position screen: the tape is taken out (its file closed, so it can be erased
// or moved), but the trap's ROM stays, as the game is using it: a load then fails at once
// (as with no tape; BREAK stops it) until another tape is loaded
static bool tape_ejected = false;
static bool tape_trap_running() { return tape_service_active; }
static FileHandle tape_file;
static bool tape_file_open = false;
static uint8_t tape_drive = 0;
static char tape_name[BROWSE_PATH_MAX];
static TzxState tape_tz;      // where the tape being served is (a preview of another .tzx has its own)
static uint32_t tape_done = 0;  // data blocks the tape has gone past since its start
#if PICOMENU_HISCORE
// a tape is being loaded: the high score table is not put back until it has stopped (no block
// for 5 s, after one at least), as the game sets up its own table once it is in
static bool tape_busy() {
    if (bt_playing) return true;
    if (!tape_service_active || tape_ejected) return false;
    return (tape_done == 0 && !hs_bt_played) || to_ms_since_boot(get_absolute_time()) - tape_block_ms < 5000;
}
#endif
static bool tape_goto(uint32_t block);
// Tape in (T in Browse Games): see tape_in_start()
static const uint32_t TAPE_IN_ENTRY = TAPE_HOOK_ADDR;   // as in piohandler.S
static const uint32_t TAPE_IN_EXIT = TAPE_HOOK_ADDR + 1 + TAPE_IN_HOOK_RET;   // TAPE_IN_HOOK's RET
static_assert(TAPE_IN_EXIT == 0x059A, "piohandler.S's comments have TAPE_IN_EXIT 0x059A");
static_assert(TAPE_HOOK[sizeof(TAPE_HOOK) - 9] == 0xC9, "the hook ends with RET before its glyph");
static_assert(TAPE_HOOK[36] == 0x21 && TAPE_HOOK[37] == 0x96 && TAPE_HOOK[38] == 0x05, "LD HL,glyph");
static const uint32_t TAPE_IN_ENTRY_SHIFTED = (0u << 14 | TAPE_IN_ENTRY) << 17;   // an M1 read
// Recording (W on the tape position screen): SAVE goes through SA-BYTES, where the Pico takes
// over in the same way and serves TAPE_SAVE_HOOK (tools/picosim/tests/savehook.asm). It hands
// the block (flag, from, length; mailbox mode 2) to the Pico, which writes it on the end of the
// recording, then jumps to SA/LD-RET (0x053F) in the Spectrum's ROM. Playing and recording are
// one at a time, as on a cassette deck: the bus handler watches one of the two.
static bool tape_rec = false;                           // recording (tape_name is the new file)
static const uint32_t TAPE_SAVE_ENTRY = 0x04C2;         // SA-BYTES
static constexpr uint8_t TAPE_SAVE_HOOK[] = {
    0x00, 0xF3, 0x32, 0xF9, 0x3F, 0xDD, 0x22, 0xFA, 0x3F, 0xED, 0x53, 0xFC, 0x3F, 0x3E, 0x02, 0x32,
    0xFE, 0x3F, 0x3E, 0x01, 0x32, 0xF8, 0x3F, 0x3A, 0xF8, 0x3F, 0xFE, 0x02, 0x38, 0xF9, 0xC3, 0x3F,
    0x05
};
static const uint32_t TAPE_SAVE_EXIT = TAPE_SAVE_ENTRY + sizeof(TAPE_SAVE_HOOK) - 1;  // its JP's last byte
static_assert(TAPE_SAVE_EXIT == 0x04E2 && TAPE_SAVE_EXIT < TAPE_IN_ENTRY, "the two hooks apart");
static_assert(TAPE_SAVE_HOOK[sizeof(TAPE_SAVE_HOOK) - 3] == 0xC3, "it ends with JP 053F");

// The bus handler takes the new values when the NMI ROM exits: playing (a tape put in with T)
static void tape_in_watch(bool on) {
    rom_state.tape_exit = (uint32_t) TAPE_IN_EXIT << 18;
    rom_state.tape_entry = on ? TAPE_IN_ENTRY_SHIFTED : 1;
}
// ... or recording
static void tape_rec_watch() {
    rom_state.tape_exit = (uint32_t) TAPE_SAVE_EXIT << 18;
    rom_state.tape_entry = (0u << 14 | TAPE_SAVE_ENTRY) << 17;     // an M1 read
}

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
    tape_done = 0;
    return tape_file_open;
}

// sd_check: a tape playing from the card keeps its file open (allowed); when the card has to be
// mounted again, the file is closed first and opened again after, where it was
// (where the tape was: kept on sd_check's stack, the RAM for statics being all used)
struct TapeLost {
    bool lost;
    uint32_t pos, done;
    TzxState tz;
};
static bool tape_on_card_open() { return tape_file_open && tape_drive == 1; }
static void tape_card_lost(TapeLost& t) {
    t.lost = tape_on_card_open();
    if (!t.lost) return;
    t.pos = tape_file.tell();
    t.done = tape_done;
    t.tz = tape_tz;
    tape_file.close();
    tape_file_open = false;
}
static void tape_card_back(const TapeLost& t) {
    if (!t.lost || !sd_card_mounted) return;
    tape_file_open = tape_file.open(Drive::EXTERNAL_SD_CARD, tape_name, true);
    if (tape_file_open && tape_file.seek(t.pos)) {
        tape_done = t.done;
        tape_tz = t.tz;
    } else {
        tape_open_file();               // from its start, then
    }
}

// Reads and patches the 48K ROM into rom_data[0..16383]
static bool tape_load_patched_rom(bool first_block_is_code, uint8_t* scratch = nullptr) {
    // The ROM is read into the part of the snapshot buffer that the 48K
    // memory image does not use, instead of a buffer of its own (or into scratch: a 128K
    // snapshot fills the buffer)
    static const uint32_t ROM_SIZE = 16384;
    static const uint32_t ROM_SCRATCH = 65536;
    static_assert(ROM_SCRATCH + ROM_SIZE <= SNAP_LOAD_BUFFER_SIZE, "no room for the ROM");
    static_assert(ROM_SCRATCH >= SNA_FILE_SIZE, "the ROM would overlap the memory image");
    uint8_t* rom = scratch != nullptr ? scratch : reinterpret_cast<uint8_t*>(snap_load_buffer) + ROM_SCRATCH;
    FileHandle f;
    bool opened = rom_file_open(f, TAPE_ROM_NAME);
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
    if (memcmp(rom + TAPE_LDBYTES, TAPE_ROM_LDBYTES_SIG, sizeof(TAPE_ROM_LDBYTES_SIG)) != 0 ||
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
static int level_loaders_patch(uint8_t* mem, uint32_t n, uint32_t base, uint16_t* at, int max);
static bool level_loaders_scan_pending = false;     // the Spectrum's memory to be looked at (level_loaders_poll)
static bool tape_serve_block(uint8_t flag, uint16_t dest, uint16_t length, bool load) {
    tz = tape_tz;
    bool ok = tape_serve_block_at(flag, dest, length, load);
    tape_tz = tz;
    return ok;
}

static bool tape_serve_block_at(uint8_t flag, uint16_t dest, uint16_t length, bool load) {
    const bool tzx = tape_is_tzx(tape_name);
    uint32_t blen;
    bool more = tape_next_block(tape_file, tzx, blen);
    if (more) tape_done++;
    tape_block_ms = to_ms_since_boot(get_absolute_time());
    if (!more) {
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
        // a game's own copy of the ROM loader in it (its level loader): made to use the trap
        if (load && cfg.level_loaders) level_loaders_patch(target, keep, d, nullptr, 0);
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

static bool tape_record_block(uint8_t flag, uint16_t from, uint16_t length);
// Called from the main loop
static void tape_service_poll() {
    volatile uint8_t* mbox = rom_data + TAPE_MBOX;
    if (!tape_service_active || mbox[0] != 1) {
        return;
    }
    if (tape_ejected) {
        mbox[0] = 3;                // no tape
        return;
    }
    uint8_t flag = mbox[1];
    uint16_t dest = mbox[2] | (mbox[3] << 8);
    uint16_t length = mbox[4] | (mbox[5] << 8);
    if (mbox[6] == 2) {                 // a SAVE (the record hook)
        bool ok = tape_rec && tape_record_block(flag, dest, length);
        mbox[0] = ok ? 2 : 3;
        return;
    }
#if PICOMENU_BT
    if (bt_takes_load()) {
        mbox[0] = 4;                    // B - Bluetooth tape has it: the ROM loads from EAR
        return;
    }
#endif
    bool load = mbox[6] != 0;
    bool ok = tape_serve_block(flag, dest, length, load);
    mbox[0] = ok ? 2 : 3;
}

#ifdef PICOFACE_HOST
// the simulator's main loop: a tape block asked for
void host_tape_poll() { tape_service_poll(); }
#endif

static void tape_service_stop() {
#if PICOMENU_BT
    bt_playing = false;
    bt_tape_ready = false;
    bt_owns = false;
#endif
    tape_in_watch(false);
    tape_in = false;
    tape_rec = false;
    if (tape_service_active) {
        tape_service_active = false;
        tape_ejected = false;
        if (tape_file_open) {
            tape_file.close();
            tape_file_open = false;
        }
        rom_state.flags_on_nmi_exit = 0;       // back to the internal ROM
        rom_state.writableStartAddress = 32768;
    }
}

static void tape_eject() {
    if (tape_rec) {                     // stop recording: the file is complete as it is
        LOG("Tape: recording to %s stopped\n", tape_name);
        tape_service_stop();
        return;
    }
    if (tape_service_active && !tape_ejected) {
#if PICOMENU_BT
        bt_playing = false;
        bt_tape_ready = false;
        bt_owns = false;
#endif
        if (tape_file_open) {
            tape_file.close();
            tape_file_open = false;
        }
        tape_ejected = true;
        if (tape_in) {
            tape_in_watch(false);       // the Spectrum's own ROM loads from its EAR socket again
        }
        LOG("Tape: ejected\n");
    }
}

static void tape_eject_if_in(uint8_t drive, const char* path) {
    if (!tape_service_active || tape_ejected || (drive == 1) != (tape_drive == 1)) return;
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;        // a folder, given with or without its '/'
    if (strncasecmp(tape_name, path, n) == 0 && (tape_name[n] == '\0' || tape_name[n] == '/' || n == 1)) {
        tape_eject();
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
    tape_ejected = false;
    rom_state.flags_on_nmi_exit = 1;                    // serve our ROM when the NMI exits
    rom_state.writableStartAddress = TAPE_MBOX;         // only the mailbox is writable
    return true;
}

// ---- Tape in (T in Browse Games) ----
// The tape is put in for the Spectrum's own ROMs, with no reset: 128 BASIC, the Tape Loader,
// 48 BASIC, or a game already running. The Spectrum's ROM runs as normal, and the bus handler
// (piohandler.S) watches for the fetch at 0x0562 in LD-BYTES: there it serves rom_data, which
// holds TAPE_IN_HOOK (the trap's hook with a NOP before it), and hands back to the internal ROM
// when the hook's RET is read. The hook asks for the block through the mailbox as the 48K
// trap's does, so tape_service_poll() serves both. On a 128K the editor ROM has text at
// 0x0556-0x05A0, so the fetch only happens in ROM 1 (48 BASIC), in LD-BYTES.
// The 48K way (a tape loaded with its number) is unchanged: the Pico serves its patched 48K
// ROM all the time, with the hook in it, and the bus handler's watch stays off.

// The hook in rom_data: a NOP (served from a constant, as the NMI's first byte is), the trap's
// hook, its glyph. Nothing else in rom_data is ever served while the internal ROM is in use.
static void tape_in_hook_to_rom() {
    memset(rom_data, 0, 16384);
    uint8_t* hook = rom_data + TAPE_IN_ENTRY;
    hook[0] = 0x00;
    memcpy(hook + 1, TAPE_IN_HOOK, sizeof(TAPE_IN_HOOK));
    for (uint32_t at : TAPE_IN_HOOK_LDHL) {
        hook[1 + at + 1] = (uint8_t) ((TAPE_IN_EXIT + 1) & 0xFF);  // LD HL,glyph: it is one on
    }
    memcpy(rom_data + TAPE_SAVE_ENTRY, TAPE_SAVE_HOOK, sizeof(TAPE_SAVE_HOOK));
}


// Puts the tape filename (on drive: 1 the SD card) in. The NMI menu is up. nullptr if it is in,
// else why not
static const char* tape_in_start(uint8_t drive, const char* filename, uint32_t block) {
    if (rom_state.flags_on_nmi_exit != 0 && !tape_service_active) {
        return "Not with a changed ROM";            // C - Change ROM: Internal Rom first
    }
    tape_service_stop();                            // a 48K tape too: its ROM is replaced
    if (filename != tape_name) {
        snprintf(tape_name, sizeof(tape_name), "%s", filename);
    }
    tape_drive = drive;
    if (!tape_open_file()) {
        LOG("Tape in: could not open %s\n", tape_name);
        return "Cannot open the tape";
    }
    tape_in_hook_to_rom();
    tape_service_active = true;
    tape_ejected = false;
    tape_in = true;
    rom_state.flags_on_nmi_exit = 0;                // the internal ROM
    rom_state.writableStartAddress = 32768;
    tape_goto(block);
    tape_in_watch(true);
    level_loaders_scan_pending = true;              // once the menu has gone
#if PICOMENU_HISCORE
    hs_tape_game(tape_name);
#endif
    LOG("Tape in: %s from block %u\n", tape_name, (unsigned) tape_done);
    return nullptr;
}

// ---- Level loaders (multi-load, stage B) ----
// Some games load their levels with their own copy of the ROM's LD-BYTES in RAM (Out Run's
// Speedlock releases, at 0x8795), often faster than the ROM and without a BREAK test. The Pico
// only sees fetches from the ROM, so the tape trap (and tape in, T) never hears of such a load:
// the game waits at PRESS PLAY. Such a copy is found by the way LD-BYTES starts reading the tape:
//     LD HL,<its SA/LD-RET> / PUSH HL / IN A,(FE) / RRA / AND 20 ...
// with its own LD-EDGE close by (LD A,7F / IN A,(FE) / RRA / [RET NC] / XOR C / AND 20). The
// IN A,(FE) / RRA (DB FE 1F) is made JP 0x0562, the place in the ROM's LD-BYTES after the same
// steps, which is where the trap's hook is: the game has already set A'F' (the flag and load or
// verify), IX and DE and pushed its own return, so the hook loads the block from the tape file
// and returns there with the carry flag as LD-BYTES would. The bytes before (the border, the
// game's own variables) still run.
// Done (with level_loaders = patch in Picomenu.ini) to each block the trap loads, and to the
// whole of the Spectrum's memory as it can see it just after a tape is put in with T or a
// snapshot carries on with its tape (the Pico reads it and writes the 3 bytes back, with NMIs).
// Without a tape in, 0x0562 is the ROM's own LD-BYTES, which loads at the normal speed.
static const uint8_t LL_READ[] = { 0xDB, 0xFE, 0x1F, 0xE6, 0x20 };     // IN A,(FE) / RRA / AND 20
static const uint8_t LL_JUMP[] = { 0xC3, (uint8_t) (TAPE_HOOK_ADDR & 0xFF), (uint8_t) (TAPE_HOOK_ADDR >> 8) };
static const int LL_EDGE_NEAR = 256;        // how far from it its LD-EDGE may be

// LD A,7F / IN A,(FE) / RRA / [RET NC] / XOR C / AND 20 at mem[i]
static bool ll_edge_at(const uint8_t* m, uint32_t n, uint32_t i) {
    if (i + 9 > n || m[i] != 0x3E || m[i + 1] != 0x7F || m[i + 2] != 0xDB || m[i + 3] != 0xFE || m[i + 4] != 0x1F) return false;
    uint32_t k = i + 5;
    if (m[k] == 0xD0) k++;
    return k + 3 <= n && m[k] == 0xA9 && m[k + 1] == 0xE6 && m[k + 2] == 0x20;
}

// The level loaders in mem (n bytes, mem[0] at address base) are patched; at[] (if not nullptr)
// gets the addresses of the 3 bytes changed, up to max of them. The number patched.
static int level_loaders_patch(uint8_t* mem, uint32_t n, uint32_t base, uint16_t* at, int max) {
    int found = 0;
    for (uint32_t i = 0; i + 4 + sizeof(LL_READ) <= n; i++) {
        if (mem[i] != 0x21 || mem[i + 3] != 0xE5 || memcmp(mem + i + 4, LL_READ, sizeof(LL_READ)) != 0) continue;
        uint16_t ret = (uint16_t) (mem[i + 1] | (mem[i + 2] << 8));
        if (ret < 0x4000 && ret != 0x053F) continue;            // its return: in RAM, or SA/LD-RET
        uint32_t from = i > (uint32_t) LL_EDGE_NEAR ? i - LL_EDGE_NEAR : 0;
        uint32_t to = i + LL_EDGE_NEAR < n ? i + LL_EDGE_NEAR : n;
        bool edge = false;
        for (uint32_t k = from; k < to && !edge; k++) edge = ll_edge_at(mem, n, k);
        if (!edge) continue;
        uint32_t addr = base + i + 4;
        if (addr < 0x4000 || addr + sizeof(LL_JUMP) > 0x10000) continue;
        memcpy(mem + i + 4, LL_JUMP, sizeof(LL_JUMP));
        LOG("Level loader at %04X (return %04X): it now loads from the tape trap\n", (unsigned) addr, ret);
        if (at != nullptr && found < max) at[found] = (uint16_t) addr;
        found++;
    }
    return found;
}

// After T or a snapshot that carries on with its tape: the Spectrum's memory is read (once the
// menu has gone) and its level loaders patched. Called from the main loop.
static void level_loaders_poll() {
    if (!level_loaders_scan_pending || rom_state.nmi_active || (rom_state.flags & 0x2) != 0) return;
    level_loaders_scan_pending = false;
    if (!cfg.level_loaders || !tape_service_active || tape_ejected || tape_rec || sna_in_buffer) return;
    if (loaded_paging.valid && (loaded_paging.port & 0x30) == 0) {
        // a 128K game with the editor ROM paged: 0x0562 is not LD-BYTES there
        LOG("Level loaders: not looked for (the 128K editor ROM is paged)\n");
        return;
    }
    bool loggingWasEnabled = enable_logging;
    enable_logging = false;
    bool got = sendNmiAndWaitForCompletion(STARTUP_ACTION_SAVE_MEMORY, 0x4000, 0xC000);
    enable_logging = loggingWasEnabled;
    if (!got) return;
    uint8_t* ram = reinterpret_cast<uint8_t*>(snap_load_buffer) + SNA_HEADER_SIZE;    // address 16384 is ram[0]
    uint16_t at[4];
    int n = level_loaders_patch(ram, 0xC000, 0x4000, at, 4);
    for (int k = 0; k < n && k < 4; k++) {
        enable_logging = false;
        sendNmiAndWaitForCompletion(STARTUP_ACTION_LOAD_MEMORY, at[k], (uint16_t) sizeof(LL_JUMP));
        enable_logging = loggingWasEnabled;
    }
    LOG("Level loaders: %d patched\n", n);
}
#ifdef PICOFACE_HOST
void host_level_loaders_poll() { if (!bt_on) level_loaders_poll(); }
#endif

// ---- Recording (W on the tape position screen) ----
// A new .tap in RECORDINGS_DIR on the SD card, named when W is pressed. Each SAVE (or anything
// that saves with the ROM's SA-BYTES) adds its block on the end at once, until E stops it. A name
// already in use is refused, so a recording never writes over a tape.
static const char RECORDINGS_DIR[] = "/Recordings";
static uint32_t tape_rec_blocks = 0;
static bool tape_rec_failed = false;            // a block could not be written

// Starts recording to RECORDINGS_DIR/<typed>.tap. The NMI menu is up. nullptr if it has
// started, else why not
static const char* tape_record_start(const char* typed) {
    if (rom_state.flags_on_nmi_exit != 0 && !tape_service_active) {
        return "Not with a changed ROM";
    }
    if (!sd_card_mounted) {
        return "No SD card";
    }
    char name[40];
    snprintf(name, sizeof(name), "%s", typed);
    if (!tidy_new_name(name)) {
        return "Type a name for the tape";
    }
    char* path = grab_path;                     // free here
    int n = snprintf(path, sizeof(grab_path), "%s/%s.tap", RECORDINGS_DIR, name);
    if (n < 0 || n >= (int) sizeof(tape_name)) {
        return "That name is too long";
    }
    if (fsops::kind(Drive::EXTERNAL_SD_CARD, RECORDINGS_DIR) != fsops::KIND_DIR &&
            make_sd_folder(RECORDINGS_DIR) != FILEOP_OK) {
        return "Cannot make /Recordings";
    }
    if (fsops::kind(Drive::EXTERNAL_SD_CARD, path) != fsops::KIND_NONE) {
        return "That name is already in use";
    }
    FileHandle f;                               // the new, empty tape
    if (!f.open(Drive::EXTERNAL_SD_CARD, path, false)) {
        return "Cannot make the file";
    }
    f.close();
    tape_service_stop();                        // playing and recording are one at a time
    snprintf(tape_name, sizeof(tape_name), "%s", path);
    tape_drive = 1;
    tape_in_hook_to_rom();
    tape_service_active = true;
    tape_ejected = false;
    tape_in = true;                             // the Spectrum's own ROM; resets keep it
    tape_rec = true;
    tape_rec_blocks = 0;
    tape_rec_failed = false;
    rom_state.flags_on_nmi_exit = 0;
    rom_state.writableStartAddress = 32768;
    tape_rec_watch();
    LOG("Tape: recording to %s\n", tape_name);
    return nullptr;
}

// A block SA-BYTES was asked to save: the flag, then length bytes from `from`, then the
// checksum, as a .tap block on the end of the recording. The bytes at 16384 and up come from the
// Spectrum (through the snapshot buffer, at their address - 16384); any below are the ROM's, which
// the Pico can't read, so they are taken from 48.rom (zeros if there is none).
static bool tape_record_block(uint8_t flag, uint16_t from, uint16_t length) {
    uint32_t start = from, end = (uint32_t) from + length;
    if (end > 65536) end = 65536;               // as far as the Z80 can see
    uint8_t* buf = reinterpret_cast<uint8_t*>(snap_load_buffer);
    uint8_t* ram = buf + SNA_HEADER_SIZE;       // address 16384 is ram[0]
    uint8_t* rom = buf + 65536;                 // past the 48K image (as tape_load_patched_rom)
    static_assert(SNA_HEADER_SIZE + 49152 <= 65536 && 65536 + 16384 <= SNAP_LOAD_BUFFER_SIZE, "room");
    if (end > 16384) {
        uint32_t s = start > 16384 ? start : 16384;
        bool loggingWasEnabled = enable_logging;
        enable_logging = false;
        bool got = sendNmiAndWaitForCompletion(STARTUP_ACTION_SAVE_MEMORY, (uint16_t) s, (uint16_t) (end - s));
        enable_logging = loggingWasEnabled;
        if (!got) return false;
    }
    if (start < 16384) {
        memset(rom, 0, 16384);
        FileHandle r;
        if (rom_file_open(r, TAPE_ROM_NAME)) {
            r.read(rom, 16384);
            r.close();
        }
    }
    FileHandle f;
    if (!f.openAppend(Drive::EXTERNAL_SD_CARD, tape_name)) {
        tape_rec_failed = true;
        return false;
    }
    uint32_t n = end - start;
    uint8_t head[3] = { (uint8_t) ((n + 2) & 0xFF), (uint8_t) ((n + 2) >> 8), flag };
    uint8_t sum = flag;
    bool ok = f.write(head, 3) == 3;
    if (ok && start < 16384) {
        uint32_t e = end < 16384 ? end : 16384;
        for (uint32_t a = start; a < e; a++) sum ^= rom[a];
        ok = f.write(rom + start, e - start) == (int) (e - start);
    }
    if (ok && end > 16384) {
        uint32_t s = start > 16384 ? start : 16384;
        for (uint32_t a = s; a < end; a++) sum ^= ram[a - 16384];
        ok = f.write(ram + (s - 16384), end - s) == (int) (end - s);
    }
    ok = ok && f.write(&sum, 1) == 1;
    ok = f.close() && ok;
    if (ok) tape_rec_blocks++;
    else tape_rec_failed = true;
    LOG("Tape: recorded block flag %02X, %u bytes from %04X%s\n", flag, (unsigned) n, (unsigned) start, ok ? "" : " (not written)");
    return ok;
}

// T in Browse Games, then the tape's number
// WORD param1 = the list entry's name, BYTE param2 = the drive, WORD param3 = where the reason
// goes if it can't be put in. Status 0 (it is in: the menu goes back to the program), or 1
static void nmi_action_tape_insert() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t offset = (nmi_rom_data[3] << 8) | nmi_rom_data[2];
    uint8_t drive = nmi_rom_data[4] == 1 ? 1 : 0;
    uint16_t why_at = (nmi_rom_data[6] << 8) | nmi_rom_data[5];
    const char* why = nullptr;
#if PICOMENU_BT
    bool to_bt = false;
#endif
    int i = file_entry_for(drive, offset);
    char* path = grab_path;                         // free here (no grab, save or recent list now)
    static_assert(sizeof(grab_path) >= BROWSE_PATH_MAX, "a whole path");
    if (i < 0) {
        why = "Not found";
    } else {
        const char* name = list_entry_name[i];
        const char* ext = name + strlen(name) + 1;
        int n = snprintf(path, sizeof(grab_path), "%s/%s.%s", list_entry_dir[i], name, ext);
        if (strcasecmp(ext, "tap") != 0 && strcasecmp(ext, "tzx") != 0) {
            why = "That is not a .tap or .tzx";
        } else if (n < 0 || n >= (int) sizeof(grab_path)) {
            why = "That path is too long";
        } else {
#if PICOMENU_BT
            // A Pico W: the tape is for the trap (it loads at once) or B - Bluetooth tape (in real time).
            // Asked first (status 2, the question in why_at; the NMI ROM asks it on the key row); the key
            // comes back in param6 with the same request. BREAK: not put in; another key: asked again.
            if (!tape_asking) {
                if (why_at < 16384 - SNAPLIST_PATH_SIZE) {
                    snprintf(reinterpret_cast<char*>(nmi_rom_data + why_at), SNAPLIST_PATH_SIZE, "%s", "Tape: Trap (T) or Bluetooth (B)");
                }
                tape_asking = true;
                nmi_rom_data[0] = 2;
                return;
            }
            uint8_t answer = nmi_rom_data[7];
            if (answer == 'b' || answer == 'B') {
                to_bt = true;
            } else if (answer == 0x18) {
                tape_asking = false;
                why = "No tape put in";
                if (why_at < 16384 - SNAPLIST_PATH_SIZE) {
                    snprintf(reinterpret_cast<char*>(nmi_rom_data + why_at), SNAPLIST_PATH_SIZE, "%s", why);
                }
                nmi_rom_data[0] = 1;
                return;
            } else if (answer != 't' && answer != 'T') {
                nmi_rom_data[0] = 2;                        // (the question is still there)
                return;
            }
            tape_asking = false;
#endif
            walk_stop();                            // the folder may be read again; nothing is open
            why = tape_in_start(drive, path, 0);
            if (why == nullptr) {
                remember_last_game(drive, list_entry_dir[i], name);    // Q names its saves after it
                persist_last_game();
                game_running = true;
#if PICOMENU_BT
                if (to_bt) bt_browse_takes();       // B: the Bluetooth's (LOAD "" plays it)
#endif
            }
        }
    }
    if (why != nullptr && why_at < 16384 - SNAPLIST_PATH_SIZE) {
        snprintf(reinterpret_cast<char*>(nmi_rom_data + why_at), SNAPLIST_PATH_SIZE, "%s", why);
    }
    nmi_rom_data[0] = why == nullptr ? 0 : 1;
}

// Empties the Waste Bin (and makes it again, empty): nullptr, or why not
static const char* waste_bin_empty() {
    char* path = erase_path;
    char* scratch = erase_scratch;
    walk_stop();
    if (!sd_card_mounted) return "No SD card (or the PC has it)";
    const char* why = nullptr;
    snprintf(path, sizeof(erase_path), "%s", WASTE_BIN);
    tape_notes_follow_folder(1, WASTE_BIN, nullptr, false);     // the tape notes of what is in it
    tape_labels_follow(1, WASTE_BIN, 1, nullptr, true);         // and the labels of its tapes
    bool ok = fsops::kind(Drive::EXTERNAL_SD_CARD, WASTE_BIN) != fsops::KIND_DIR ||
        fsops::remove_tree(Drive::EXTERNAL_SD_CARD, path, sizeof(erase_path), find_dirs, 0, ERASE_MAX_DEPTH, scratch, sizeof(erase_scratch));
    LOG("Empty the Waste Bin: %d\n", ok);
    if (!ok) {
        set_last_error("Cannot empty", "the Waste Bin");
        why = "Some of it could not be erased";
    }
    if (!waste_bin_make()) why = "Cannot make the Waste Bin";
    if (in_waste_bin(1, sd_cwd)) {
        snprintf(sd_cwd, sizeof(sd_cwd), "%s", WASTE_BIN);     // a folder in it has gone
    }
    return why;
}

// ACTION_BIN_EMPTY (B in the Waste Bin): asks, and empties it on Y. BYTE param1 = the key
// (0: the screen is opened). Status 0 the screen stays, 1 it is closed
static uint8_t bin_empty_state = 0;         // 0 asking, 1 done (any key closes)
__attribute__((noinline)) static void nmi_action_bin_empty() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t key = nmi_rom_data[2];
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer);
    const char* result = nullptr;
    if (key == 0) {
        bin_empty_state = 0;
    } else if (bin_empty_state == 1 || key == 'n' || key == 0x18 || key == 'x') {
        nmi_rom_data[0] = 1;
        return;
    } else if (key == 'y') {
        const char* why = waste_bin_empty();
        result = why != nullptr ? why : "The Waste Bin is empty.";
        bin_empty_state = 1;
    }
    memset(screen, 0, 6144);
    memset(screen + 6144, ui_attr(0x07), 768);
    memset(screen + 6144, gameinfo::ATTR_BANNER, 32);
    help_put_text(screen, 0, 1, "Waste Bin", 9, gameinfo::ATTR_BANNER);
    if (bin_empty_state == 0) {
        help_put_text(screen, 3, 1, "Are you sure you want to empty", 30, gameinfo::ATTR_TEXT);
        help_put_text(screen, 4, 1, "the waste bin?", 14, gameinfo::ATTR_TEXT);
        help_put_text(screen, 6, 1, "Everything in it is erased for", 30, gameinfo::ATTR_DIM);
        help_put_text(screen, 7, 1, "good.", 5, gameinfo::ATTR_DIM);
        help_put_text(screen, 23, 1, "Y-Yes  N-No", 11, gameinfo::ATTR_KEYS);
    } else {
        help_put_text(screen, 3, 1, result, (int) strlen(result), gameinfo::ATTR_TEXT);
        help_put_text(screen, 23, 1, "Press any key", 13, gameinfo::ATTR_KEYS);
    }
    preview_screen = screen;
    nmi_rom_data[0] = 0;
}

// ACTION_WASTE_BIN: BYTE param1 = 0 open it (W on the menu: Browse Games then lists it, on the
// SD card), 2 leave it (Browse Games has its folder before W again). WORD param2 = where a message goes (a line).
// Status 0 done, 1 not (the message says why)
__attribute__((noinline)) static void nmi_action_waste_bin() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t op = nmi_rom_data[2];
    uint16_t msg_at = (nmi_rom_data[4] << 8) | nmi_rom_data[3];
    const char* why = nullptr;
    walk_stop();
    if (!sd_card_mounted) {
        why = "No SD card (or the PC has it)";
    } else if (op == 2) {
        bin_leave();
    } else if (!waste_bin_make()) {
        why = "Cannot make the Waste Bin";
    } else {
        if (!bin_open && !in_waste_bin(1, sd_cwd)) {
            snprintf(bin_return_cwd, sizeof(bin_return_cwd), "%s", sd_cwd);
        } else if (!bin_open) {
            bin_return_cwd[0] = '\0';           // it was in the bin already: back to the root
        }
        bin_open = true;
        snprintf(sd_cwd, sizeof(sd_cwd), "%s", WASTE_BIN);      // Browse Games lists it
        snap_filter[0] = '\0';
    }
    if (why != nullptr && msg_at < 16384 - SNAPLIST_PATH_SIZE) {
        snprintf(reinterpret_cast<char*>(nmi_rom_data + msg_at), SNAPLIST_PATH_SIZE, "%s", why);
    }
    nmi_rom_data[0] = why == nullptr ? 0 : 1;
}

// ---- H in Browse Games: hide a file or folder, or show it again (ACTION_SNAP_HIDE) ----
// On the SD card it is the hidden attribute (as Windows sets it), on the flash a littlefs
// attribute. Hidden ones are listed only with L-All, dimmed. Those always hidden (they can't be
// loaded, or are system ones) stay so.
// WORD param1 = the list entry's name, BYTE param2 = the drive, WORD param3 = where a message
// goes (a line). Status 0: changed (the list is read again), 1: not, the message says why.
__attribute__((noinline)) static void nmi_action_snap_hide() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t offset = (nmi_rom_data[3] << 8) | nmi_rom_data[2];
    uint8_t drive = nmi_rom_data[4] == 1 ? 1 : 0;
    uint16_t msg_at = (nmi_rom_data[6] << 8) | nmi_rom_data[5];
    const char* why = nullptr;
    int i = any_entry_for(offset);
    char* path = grab_path;                             // free here
    if (i < 0) {
        why = "Not found";
    } else {
        const char* name = list_entry_name[i];
        uint8_t flags = entry_flags(name);
        size_t len = strlen(name);
        bool dir = len > 0 && name[len - 1] == '/';
        if (dir) dir_entry_path(i, path, sizeof(grab_path));
        else entry_path(i, nullptr, path, sizeof(grab_path));
        bool hide = !(flags & ENTRY_HIDDEN_SET);
        if (flags & ENTRY_ALWAYS) {
            why = "Always hidden: can't be loaded";
        } else if (drive == 1) {
            FRESULT r = f_chmod(path, hide ? AM_HID : 0, AM_HID);
            if (r != FR_OK) why = "Could not change it";
        } else {
            uint8_t one = 1;
            int r = hide ? pico_setattr(path, FLASH_HIDDEN_ATTR, &one, 1) : pico_removeattr(path, FLASH_HIDDEN_ATTR);
            if (r < 0) why = "Could not change it";
        }
        LOG("%s %s on drive %d: %s\n", hide ? "Hide" : "Show", path, drive, why != nullptr ? why : "done");
        if (why == nullptr) {
            walk_stop();                                // the list is read again, with the change
        }
    }
    if (why != nullptr && msg_at < 16384 - SNAPLIST_PATH_SIZE) {
        snprintf(reinterpret_cast<char*>(nmi_rom_data + msg_at), SNAPLIST_PATH_SIZE, "%s", why);
    }
    nmi_rom_data[0] = why == nullptr ? 0 : 1;
}

// ---- A .txt file (L-All lists them in Browse Games; ENTER or V opens one): ACTION_TEXT_FILE ----
// The pico shows the file a page at a time (21 lines of 32 letters, long lines wrapped at a
// space), N and P turning the pages; E edits it, on the SD card only, in the details' editor
// (96 lines of 30 letters, 3 KB at most): a bigger file, or one with a longer line, can only be
// looked at, as the editor would have to change it to hold it. X or BREAK closes it.
// The screen is drawn at the start of snap_load_buffer (as the tape position's); the path and
// where each page starts are kept above it, the file being edited in the details' room.
static const uint32_t TXT_PATH_OFFSET = 8192;
static const uint32_t TXT_PAGES_OFFSET = TXT_PATH_OFFSET + 512;
static const int TXT_PAGES_MAX = 1024;                  // P goes back this many pages at most
static const uint32_t TXT_READ_OFFSET = TXT_PAGES_OFFSET + TXT_PAGES_MAX * 4;
static const size_t TXT_READ_SIZE = 1024;               // more than a page can use (21 lines of 32, and their ends)
static_assert(BROWSE_PATH_MAX <= 512, "the text file's path does not fit");
static_assert(TXT_READ_OFFSET + TXT_READ_SIZE <= 16384, "the text viewer would cover the game information's screen");
static const int TXT_ROWS = 21;
static const size_t TXT_EDIT_MAX = DET_FILE_MAX - 72;   // what the editor may grow it to (its file buffer is DET_FILE_MAX)
static int txt_page = 0;                                // the page shown (pages[txt_page] is where it starts)
static int txt_known = 1;                               // pages[0 .. txt_known-1] are known
static bool txt_last = false;                           // the page shown is the last
static uint8_t txt_drive = 1;
static bool txt_editing = false;
static bool txt_crlf = false;                           // its lines end with \r\n: saved the same way
static const char* txt_msg = nullptr;                   // shown once above the keys

static char* txt_path() { return snap_load_buffer + TXT_PATH_OFFSET; }
static uint32_t* txt_pages() { return reinterpret_cast<uint32_t*>(snap_load_buffer + TXT_PAGES_OFFSET); }
static DetEditorBig* txt_editor() { return reinterpret_cast<DetEditorBig*>(snap_load_buffer + DET_EDITOR_OFFSET); }

// Draws the page starting at file position `from`; returns where the next page starts, and
// whether this page reached the end of the file
NOTES_FUNC static uint32_t txt_draw_page(uint8_t* screen, uint32_t from, bool* at_end) {
    char* buf = snap_load_buffer + TXT_READ_OFFSET;
    int n = 0;
    FileHandle& f = *notes_handle();
    *at_end = true;
    if (f.open(txt_drive == 1 ? Drive::EXTERNAL_SD_CARD : Drive::INTERNAL_FLASH, txt_path(), true)) {
        if (f.seek(from)) {
            n = f.read(buf, TXT_READ_SIZE);
            if (n < 0) n = 0;
        }
        *at_end = from + (uint32_t) n >= (uint32_t) f.size();
        f.close();
    } else {
        help_put_text(screen, 2, 1, "Cannot read the file", 20, gameinfo::ATTR_KEYS);
        return from;
    }
    int pos = 0;
    for (int row = 0; row < TXT_ROWS && pos < n; row++) {
        char line[33];
        int len = 0, start = pos;
        bool ended = false;                 // the line ended with its end of line (taken)
        while (pos < n && len < 32) {
            char c = buf[pos];
            if (c == '\r' || c == '\n') {
                pos++;
                if (c == '\r' && pos < n && buf[pos] == '\n') pos++;
                ended = true;
                break;
            }
            if (c == '\t') c = ' ';
            if ((unsigned char) c < 32 || (unsigned char) c > 126) c = '?';
            line[len++] = c;
            pos++;
        }
        if (!ended && len == 32 && pos < n && buf[pos] != '\r' && buf[pos] != '\n' && buf[pos] != ' ') {
            // a word cut by the edge: the line breaks after its last space, if it has one
            int sp = len - 1;
            while (sp > 0 && line[sp] != ' ') sp--;
            if (sp > 0) {
                len = sp + 1;
                pos = start + len;
            }
        }
        if (!ended && pos < n && buf[pos] == ' ' && len == 32) {
            pos++;                          // a space at the edge is the line's end
        }
        help_put_text(screen, 1 + row, 0, line, len, gameinfo::ATTR_TEXT);
    }
    if (pos < n) *at_end = false;
    return from + (uint32_t) pos;
}

NOTES_FUNC static void txt_draw(uint8_t* screen) {
    memset(screen, 0, 6144);
    memset(screen + 6144, ui_attr(0x07), 768);
    memset(screen + 6144, gameinfo::ATTR_BANNER, 32);
    const char* path = txt_path();
    const char* name = strrchr(path, '/');
    name = name != nullptr ? name + 1 : path;
    char title[48];
    if (txt_editing) {
        snprintf(title, sizeof(title), "Edit: %s", name);
        help_put_text(screen, 0, 1, title, (int) strlen(title) > 30 ? 30 : (int) strlen(title), gameinfo::ATTR_BANNER);
        det_field_draw_lines(screen, *txt_editor());
        help_put_text(screen, 20, 1, "Move: cursor keys or CAPS+5-8", 29, gameinfo::ATTR_DIM);
        help_put_text(screen, 22, 1, "ENTER new line  DELETE deletes", 30, gameinfo::ATTR_DIM);
        return;
    }
    char pg[12];
    snprintf(pg, sizeof(pg), "Pg %d", txt_page + 1);
    int pl = (int) strlen(pg);
    int room = 30 - pl - 1, tl = (int) strlen(name);
    help_put_text(screen, 0, 1, name, tl > room ? room : tl, gameinfo::ATTR_BANNER);
    help_put_text(screen, 0, 31 - pl, pg, pl, gameinfo::ATTR_BANNER);
    uint32_t* pages = txt_pages();
    bool at_end = true;
    uint32_t next = txt_draw_page(screen, pages[txt_page], &at_end);
    txt_last = at_end || next == pages[txt_page];
    if (!txt_last && txt_page + 1 < TXT_PAGES_MAX && txt_page + 1 >= txt_known) {
        pages[txt_page + 1] = next;
        txt_known = txt_page + 2;
    }
    if (txt_msg != nullptr) {
        help_put_text(screen, 22, 1, txt_msg, (int) strlen(txt_msg), gameinfo::ATTR_KEYS);
        txt_msg = nullptr;
    }
    const char* keys = txt_drive == 1 ? "N-Next P-Prev E-Edit X-Back" : "N-Next P-Prev X-Back";
    help_put_text(screen, 23, 1, keys, (int) strlen(keys), gameinfo::ATTR_KEYS);
}

// E: the file into the editor, if it fits it
NOTES_FUNC static void txt_edit_start() {
    if (txt_drive != 1) {
        txt_msg = "Only files on the SD card";
        return;
    }
    char* file = snap_load_buffer + DET_FILE_OFFSET;
    FileHandle& f = *notes_handle();
    if (!f.open(Drive::EXTERNAL_SD_CARD, txt_path(), true)) {
        txt_msg = "Cannot read the file";
        return;
    }
    int size = f.size();
    int got = size <= (int) TXT_EDIT_MAX ? f.read(file, TXT_EDIT_MAX) : -1;
    f.close();
    if (size > (int) TXT_EDIT_MAX) {
        txt_msg = "Too big to edit (3 KB at most)";
        return;
    }
    if (got < 0) {
        txt_msg = "Cannot read the file";
        return;
    }
    DetEditorBig* ed = new (txt_editor()) DetEditorBig();
    if (!ed->load(file, (size_t) got)) {
        txt_msg = "Lines too long to edit here";
        return;
    }
    ed->set_limit(TXT_EDIT_MAX);
    txt_crlf = memchr(file, '\r', (size_t) got) != nullptr;
    txt_editing = true;
}

// Saves the edited text over the file (as the notes are saved: written straight to it)
static bool txt_saved_ini = false;     // the last save was of Picomenu.ini
NOTES_FUNC static bool txt_save() {
    char* file = snap_load_buffer + DET_FILE_OFFSET;
    size_t n = txt_editor()->text(file, DET_FILE_MAX);
    FileHandle& f = *notes_handle();
    if (!f.open(Drive::EXTERNAL_SD_CARD, txt_path(), false)) {
        set_last_error("Cannot save", "text file");
        return false;
    }
    bool ok = true;
    size_t at = 0;
    while (ok && at < n) {                  // a line at a time, with \r\n if the file had them
        const char* nl = (const char*) memchr(file + at, '\n', n - at);
        size_t len = nl != nullptr ? (size_t) (nl - (file + at)) : n - at;
        if (len > 0) ok = f.write(file + at, len) == (int) len;
        if (ok && nl != nullptr) ok = txt_crlf ? f.write("\r\n", 2) == 2 : f.write("\n", 1) == 1;
        at += len + (nl != nullptr ? 1 : 0);
    }
    ok = f.close() && ok;
    if (!ok) set_last_error("Cannot save", "text file");
    txt_saved_ini = ok && strcasecmp(txt_path(), settings::FILE_NAME) == 0;
    if (txt_saved_ini) {
        // Picomenu.ini was changed: it is used at once. The menu that is open gets the new save
        // type and theme now, and the list may be sorted another way; show_all, details,
        // start_drive and start_folder are how Browse Games starts, so the menu's next opening.
        settings_load();
        walk_stop();
        uint8_t* rom = rom_data + 16384;
        rom[START_SAVETYPE] = cfg.save_type;
        rom[START_THEME] = cfg.light ? 1 : 0;
        cfg_stale = true;
        cfg_folder_due = true;
    }
    return ok;
}

// O-Options (in Tools): Picomenu.ini's settings and the screen grab order, changed on the
// Spectrum (options.h). While the screen is open, the list's memory (free after walk_stop)
// holds the file as it was, the file as it will be written, and what is shown (before: as the
// file has it; after: as changed).
static const size_t OPT_IN_MAX = 12 * 1024;
static const size_t OPT_OUT_MAX = 16 * 1024;
static const size_t OPT_TYPED_MAX = sizeof(settings::Wifi::password);
// the WiFi page of Options (W): in the Pico W build (and the simulator, which tests it)
#if defined(WIFI_ENABLE) || defined(PICOFACE_HOST)
#define PICOMENU_WIFI_UI 1
#else
#define PICOMENU_WIFI_UI 0
#endif
static_assert(OPT_IN_MAX + OPT_OUT_MAX + 2 * sizeof(options::Edit) + OPT_TYPED_MAX + 16 <= LIST_POOL_SIZE, "Options fits in the list's memory");
static char* opt_in() { return list_pool; }
static char* opt_out() { return list_pool + OPT_IN_MAX; }
static options::Edit* opt_before() { return reinterpret_cast<options::Edit*>(list_pool + OPT_IN_MAX + OPT_OUT_MAX); }
static options::Edit* opt_after() { return opt_before() + 1; }
static char* opt_typed() { return reinterpret_cast<char*>(opt_after() + 1); }   // what is being typed (W)
static size_t opt_in_len = 0;
static uint8_t opt_page = 0;            // 0 the settings, 1 the screen grab order, 2 WiFi
static uint8_t opt_cursor[3] = { 0, 0, 0 };
static uint8_t opt_state = 0;           // 0 changing, 1 "Save changes?", 2 a message: any key closes,
                                        // 3 Diagnostics, 4 typing a WiFi value (opt_typed)
static const char* opt_msg = nullptr;
static bool opt_on_sd = true;           // where the file is (or will be)

static const uint8_t OPT_ATTR_CURSOR = 0x68;   // black on bright cyan (the theme leaves it)

// Reads the file (and, if it has no screen grab order, order.txt) into what is shown
__attribute__((noinline)) static bool opt_open() {
    walk_stop();                                    // the list's memory is ours
    list_pool_busy = true;
    info_loaded = false;
    opt_on_sd = sd_card_mounted && cfg.source != 2;
    opt_in_len = 0;
    FileHandle& f = *notes_handle();
    Drive drive = opt_on_sd ? Drive::EXTERNAL_SD_CARD : Drive::INTERNAL_FLASH;
    if (f.open(drive, settings::FILE_NAME, true)) {
        int size = f.size();
        int got = size >= 0 && size <= (int) OPT_IN_MAX ? f.read(opt_in(), OPT_IN_MAX) : -1;
        f.close();
        if (got < 0) {
            opt_msg = size > (int) OPT_IN_MAX ? "Picomenu.ini is too big (12 KB)" : "Cannot read Picomenu.ini";
            return false;
        }
        opt_in_len = (size_t) got;
    }
    options::Edit* b = new (opt_before()) options::Edit();
    char folder[options::FOLDER_MAX];
    folder[0] = '\x01';                             // the file has no start_folder
    settings::Parser parser(b->s, folder, sizeof(folder));
    parser.wifi = &b->wifi;
    parser.text(opt_in(), opt_in_len);
    if (folder[0] != '\x01') snprintf(b->start_folder, sizeof(b->start_folder), "%s", folder[0] == '\0' ? "/" : folder);
    screengrab::Order order;
    if (b->s.has_order) {
        order = b->s.order;
    } else if (sd_card_mounted) {
        FileHandle& g = *notes_handle();
        if (g.open(Drive::EXTERNAL_SD_CARD, screengrab::ORDER_FILE, true) ||
                g.open(Drive::EXTERNAL_SD_CARD, screengrab::ORDER_FILE_BARE, true)) {
            read_text_lines(g, [&](const char* t, size_t n) { order.line(t, n); });
            g.close();
        }
    }
    b->set_order(order);
    new (opt_after()) options::Edit(*b);
    return true;
}

// Writes the changes into the file and uses them at once (as E on Picomenu.ini does)
__attribute__((noinline)) static bool opt_save() {
    size_t n = 0;
    if (!options::rewrite(opt_in(), opt_in_len, *opt_before(), *opt_after(), opt_out(), OPT_OUT_MAX, &n)) {
        opt_msg = "Picomenu.ini would be too big";
        return false;
    }
    bool folder_changed = options::changed(*opt_before(), *opt_after(), options::START_FOLDER);
    FileHandle& f = *notes_handle();
    Drive drive = opt_on_sd ? Drive::EXTERNAL_SD_CARD : Drive::INTERNAL_FLASH;
    bool ok = f.open(drive, settings::FILE_NAME, false);
    if (ok) {
        ok = f.write(opt_out(), n) == (int) n;
        ok = f.close() && ok;
    }
    if (!ok) {
        set_last_error("Cannot save", "Picomenu.ini");
        opt_msg = "Could not save Picomenu.ini";
        return false;
    }
    walk_stop();                    // (the list's memory was ours: nothing is cached in it)
    settings_load();
    uint8_t* rom = rom_data + 16384;
    rom[START_SAVETYPE] = cfg.save_type;
    rom[START_THEME] = cfg.light ? 1 : 0;
    cfg_stale = true;
    if (folder_changed) cfg_folder_due = true;     // Browse Games starts there from the next menu
    return true;
}

static void opt_draw(uint8_t* screen) {
    const options::Edit& a = *opt_after();
    const options::Edit& b = *opt_before();
    memset(screen, 0, 6144);
    memset(screen + 6144, ui_attr(0x07), 768);
    memset(screen + 6144, gameinfo::ATTR_BANNER, 32);
    const char* title = opt_page == 0 ? "Options (Picomenu.ini)" : opt_page == 1 ? "Options: Screen grab order" : "Options: WiFi and Bluetooth";
    help_put_text(screen, 0, 1, title, (int) strlen(title), gameinfo::ATTR_BANNER);
    char v[40];
    if (opt_page == 0) {
        for (int i = 0; i < options::MAIN_COUNT; i++) {
            int row = 1 + i;
            bool here = i == opt_cursor[0];
            uint8_t attr = here ? OPT_ATTR_CURSOR : gameinfo::ATTR_TEXT;
            if (here) memset(screen + 6144 + row * 32, OPT_ATTR_CURSOR, 32);
            if (options::changed(b, a, (uint8_t) i)) help_put_text(screen, row, 0, "*", 1, here ? attr : gameinfo::ATTR_HEADING);
            const char* key = options::ITEMS[i].key;
            help_put_text(screen, row, 1, key, (int) strlen(key), attr);
            options::value_text(a, (uint8_t) i, v, sizeof(v));
            if (v[0] == '\0') snprintf(v, sizeof(v), "-");
            size_t len = strlen(v);
            const char* shown = v;
            char cut[20];
            if (len > 16) {             // a long folder: its end
                snprintf(cut, sizeof(cut), "..%s", v + len - 14);
                shown = cut;
            }
            help_put_text(screen, row, 15, shown, (int) strlen(shown), here ? attr : gameinfo::ATTR_HEADING);
        }
        const char* hint = options::ITEMS[opt_cursor[0]].hint;
        static_assert(1 + options::MAIN_COUNT <= 21, "the items above the hint");
        help_put_text(screen, 21, 1, hint, (int) strlen(hint), gameinfo::ATTR_DIM);
        help_put_text(screen, 22, 1, "Q/A-Up/Down  C-Change  X-Back", 29, gameinfo::ATTR_KEYS);
#if PICOMENU_WIFI_UI
        help_put_text(screen, 23, 1, "N-Order W-WiFi/BT D-Diagnostics", 31, gameinfo::ATTR_KEYS);
#else
        help_put_text(screen, 23, 1, "N-Grab order  D-Diagnostics", 27, gameinfo::ATTR_KEYS);
#endif
#if PICOMENU_WIFI_UI
    } else if (opt_page == 2) {
        // the network's name and passwords for F - File transfer (a Pico W), typed (the
        // passwords shown as * except while they are typed); then B - Bluetooth tape's settings,
        // chosen as on the settings page
        help_put_text(screen, 2, 1, "WiFi: F - File transfer", 23, gameinfo::ATTR_DIM);
        help_put_text(screen, 8, 1, "Bluetooth: B - Bluetooth tape", 29, gameinfo::ATTR_DIM);
        for (int k = 0; k < options::WIFI_COUNT; k++) {
            uint8_t id = (uint8_t) (options::WIFI_FIRST + k);
            int row = options::typed(id) ? 3 + k : 9 + (id - options::BT_VOLUME);
            bool here = k == opt_cursor[2];
            uint8_t attr = here ? OPT_ATTR_CURSOR : gameinfo::ATTR_TEXT;
            if (here) memset(screen + 6144 + row * 32, OPT_ATTR_CURSOR, 32);
            if (options::changed(b, a, id)) help_put_text(screen, row, 0, "*", 1, here ? attr : gameinfo::ATTR_HEADING);
            const char* key = options::ITEMS[id].key;
            help_put_text(screen, row, 1, key, (int) strlen(key), attr);
            char val[options::FOLDER_MAX];
            options::value_text(a, id, val, sizeof(val));
            size_t len = strlen(val);
            if (len == 0) {
                snprintf(val, sizeof(val), "%s", id == options::WIFI_WEB_PASSWORD ? "(none)" : "-");
            } else if (id == options::WIFI_PASSWORD || id == options::WIFI_WEB_PASSWORD) {
                size_t stars = len < 16 ? len : 16;
                memset(val, '*', stars);
                val[stars] = '\0';
            }
            len = strlen(val);
            const char* shown = val + (len > 16 ? len - 16 : 0);
            help_put_text(screen, row, 15, shown, (int) strlen(shown), here ? attr : gameinfo::ATTR_HEADING);
        }
        const char* hint = options::ITEMS[options::WIFI_FIRST + opt_cursor[2]].hint;
        help_put_text(screen, 15, 1, hint, (int) strlen(hint), gameinfo::ATTR_DIM);
        if (opt_state == 4) {
            char t[40];
            snprintf(t, sizeof(t), "Type the %s:", options::ITEMS[options::WIFI_FIRST + opt_cursor[2]].key);
            help_put_text(screen, 17, 1, t, (int) strlen(t), gameinfo::ATTR_HEADING);
            const char* typed = opt_typed();
            size_t tl = strlen(typed);
            snprintf(t, sizeof(t), "%s_", typed + (tl > 29 ? tl - 29 : 0));
            help_put_text(screen, 18, 1, t, (int) strlen(t), gameinfo::ATTR_TEXT);
            help_put_text(screen, 20, 1, "ENTER-Done  DELETE  BREAK-Back", 30, gameinfo::ATTR_KEYS);
            help_put_text(screen, 21, 1, "(CAPS SHIFT for capitals)", 25, gameinfo::ATTR_DIM);
        } else {
            help_put_text(screen, 20, 1, "Q/A-Up/Down  ENTER-Type/Change", 30, gameinfo::ATTR_KEYS);
            help_put_text(screen, 21, 1, "N-Settings  X-Back", 18, gameinfo::ATTR_KEYS);
        }
#endif
    } else {
        help_put_text(screen, 2, 1, "V shows a game's pictures in", 28, gameinfo::ATTR_DIM);
        help_put_text(screen, 3, 1, "this order (hidden: not shown)", 30, gameinfo::ATTR_DIM);
        for (int i = 0; i < screengrab::CATEGORIES; i++) {
            int row = 5 + i;
            bool here = i == opt_cursor[1];
            uint8_t c = a.list[i];
            uint8_t attr = here ? OPT_ATTR_CURSOR : a.shown[c] ? gameinfo::ATTR_TEXT : gameinfo::ATTR_DIM;
            if (here) memset(screen + 6144 + row * 32, OPT_ATTR_CURSOR, 32);
            snprintf(v, sizeof(v), "%d %s", i + 1, screengrab::ORDER_NAME[c]);
            help_put_text(screen, row, 1, v, (int) strlen(v), attr);
            if (!a.shown[c]) help_put_text(screen, row, 25, "hidden", 6, attr);
        }
        if (options::order_changed(b, a)) help_put_text(screen, 12, 1, "* changed", 9, gameinfo::ATTR_HEADING);
        help_put_text(screen, 18, 1, "Q/A-Up/Down  H-Hide/Show", 24, gameinfo::ATTR_KEYS);
        help_put_text(screen, 19, 1, "U-Move up  D-Move down", 22, gameinfo::ATTR_KEYS);
        help_put_text(screen, 20, 1, "N-Settings  X-Back", 18, gameinfo::ATTR_KEYS);
    }
    if (opt_state == 1) {
        help_put_text(screen, 21, 1, "Save changes?", 13, gameinfo::ATTR_HEADING);
        help_put_text(screen, 22, 1, "Y-Yes  N-No  BREAK-Stay", 23, gameinfo::ATTR_KEYS);
    }
    if (opt_msg != nullptr) help_put_text(screen, 23, 1, opt_msg, (int) strlen(opt_msg), gameinfo::ATTR_TEXT);
}

// ACTION_OPTIONS: BYTE param1 = the key (0: opened). Status 0 the screen stays, 1 closed
__attribute__((noinline)) static void nmi_action_options() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t key = nmi_rom_data[2];
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer);
#if PICOMENU_WIFI_UI
    if (key != 0 && opt_state == 4) {
        // typing a WiFi value: as it is typed (capitals and all)
        options::Edit& a = *opt_after();
        uint8_t id = (uint8_t) (options::WIFI_FIRST + opt_cursor[2]);
        char* typed = opt_typed();
        size_t len = strlen(typed);
        size_t max = id == options::WIFI_PASSWORD ? sizeof(a.wifi.password) : sizeof(a.wifi.ssid);
        if (is_key_break(key)) {
            opt_state = 0;
        } else if (key == notes::KEY_ENTER || key == 13) {
            char* to = id == options::WIFI_SSID ? a.wifi.ssid : id == options::WIFI_PASSWORD ? a.wifi.password : a.wifi.web_password;
            size_t a0 = 0, n = len;
            while (a0 < n && typed[a0] == ' ') a0++;        // (spaces at the ends: the file can't keep them)
            while (n > a0 && typed[n - 1] == ' ') n--;
            memcpy(to, typed + a0, n - a0);
            to[n - a0] = '\0';
            opt_state = 0;
        } else if (key == notes::KEY_DELETE) {
            if (len > 0) typed[len - 1] = '\0';
        } else if (key >= 32 && key < 127 && len + 1 < max) {
            typed[len] = (char) key;
            typed[len + 1] = '\0';
        }
        opt_draw(screen);
        preview_screen = screen;
        nmi_rom_data[0] = 0;
        return;
    }
#endif
    if (key >= 'A' && key <= 'Z') key = (uint8_t) (key - 'A' + 'a');
    const bool up = key == notes::KEY_UP || key == 'q' || key == '7';
    const bool down = key == notes::KEY_DOWN || key == 'a' || key == '6';
    const bool leave = key == 'x' || is_key_break(key);
    if (key == 0) {
        opt_page = 0;
        opt_cursor[0] = opt_cursor[1] = opt_cursor[2] = 0;
        opt_state = 0;
        opt_msg = nullptr;
        if (!opt_open()) opt_state = 2;
    } else if (opt_state == 3) {
        opt_state = 0;                  // the Diagnostics page: any key goes back to the settings
    } else if (opt_state == 2) {
        nmi_rom_data[0] = 1;            // the message has been seen
        return;
    } else if (opt_state == 1) {
        if (key == 'y') {
            if (opt_save()) {
                nmi_rom_data[0] = 1;
                return;
            }
            opt_state = 0;
        } else if (key == 'n') {
            nmi_rom_data[0] = 1;        // closed, the changes forgotten
            return;
        } else if (is_key_break(key)) {
            opt_state = 0;
        }
    } else {
        opt_msg = nullptr;
        options::Edit& a = *opt_after();
        int count = opt_page == 0 ? options::MAIN_COUNT : opt_page == 1 ? screengrab::CATEGORIES : options::WIFI_COUNT;
        uint8_t& cur = opt_cursor[opt_page];
        if (leave) {
            if (!options::any_changed(*opt_before(), a)) {
                nmi_rom_data[0] = 1;
                return;
            }
            opt_state = 1;
        } else if (up) {
            cur = (uint8_t) (cur == 0 ? count - 1 : cur - 1);
        } else if (down) {
            cur = (uint8_t) (cur + 1 >= count ? 0 : cur + 1);
        } else if (key == 'n' || key == 'p') {
            opt_page = opt_page == 0 ? 1 : 0;
#if PICOMENU_WIFI_UI
        } else if (opt_page == 0 && key == 'w') {
            opt_page = 2;
        } else if (opt_page == 2 && (key == notes::KEY_ENTER || key == 13 || key == 'c')) {
            uint8_t id = (uint8_t) (options::WIFI_FIRST + cur);
            if (options::typed(id)) {
                // type it: starting from what it is
                options::value_text(*opt_after(), id, opt_typed(), OPT_TYPED_MAX);
                opt_state = 4;
            } else {
                options::cycle(a, id, sd_cwd);          // a Bluetooth setting: the next value
            }
#endif
        } else if (opt_page == 0 && key == 'd') {
            help_draw_page(screen, HELP_PAGE_COUNT - 1);       // Diagnostics (the last page of Help)
            opt_state = 3;
            preview_screen = screen;
            nmi_rom_data[0] = 0;
            return;
        } else if (opt_page == 0 && (key == 'c' || key == notes::KEY_ENTER || key == 13)) {
            options::cycle(a, cur, sd_cwd);
        } else if (opt_page == 1 && key == 'h') {
            if (!options::toggle(a, cur)) opt_msg = "One kind at least must show";
        } else if (opt_page == 1 && key == 'u') {
            if (options::move(a, cur, -1)) cur--;
        } else if (opt_page == 1 && key == 'd') {
            if (options::move(a, cur, 1)) cur++;
        }
    }
    if (opt_state == 2) {
        memset(screen, 0, 6144);
        memset(screen + 6144, ui_attr(0x07), 768);
        memset(screen + 6144, gameinfo::ATTR_BANNER, 32);
        help_put_text(screen, 0, 1, "Options (Picomenu.ini)", 22, gameinfo::ATTR_BANNER);
        help_put_text(screen, 2, 1, opt_msg, (int) strlen(opt_msg), gameinfo::ATTR_TEXT);
        help_put_text(screen, 4, 1, "Press any key", 13, gameinfo::ATTR_KEYS);
    } else {
        opt_draw(screen);
    }
    preview_screen = screen;
    nmi_rom_data[0] = 0;
}

// ACTION_TEXT_FILE: BYTE param1 = the key (0: open the file: WORD param2 = the list entry's
// name, BYTE param4 = the drive). Status 0: the screen stays (shown again), 1: it is closed
__attribute__((noinline)) static void nmi_action_text_file() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t key = nmi_rom_data[2];
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer);
    if (key == 0) {
        uint16_t offset = (nmi_rom_data[4] << 8) | nmi_rom_data[3];
        txt_drive = nmi_rom_data[5] == 1 ? 1 : 0;
        int i = file_entry_for(txt_drive, offset);
        if (i < 0) {
            nmi_rom_data[0] = 1;
            return;
        }
        const char* name = list_entry_name[i];
        const char* ext = name + strlen(name) + 1;
        int n = snprintf(txt_path(), BROWSE_PATH_MAX, "%s/%s.%s", list_entry_dir[i], name, ext);
        if (n < 0 || n >= (int) BROWSE_PATH_MAX) {
            nmi_rom_data[0] = 1;
            return;
        }
        walk_stop();                        // nothing is open while the file is read
        info_loaded = false;                // the game information's buffers are not kept
        txt_pages()[0] = 0;
        txt_known = 1;
        txt_page = 0;
        txt_editing = false;
        txt_msg = nullptr;
        LOG("Text file %s on drive %d\n", txt_path(), txt_drive);
    } else if (txt_editing) {
        notes::EditorT<96, 30>::Result r = txt_editor()->key(key);
        if (r == DetEditorBig::SAVE) {
            txt_editing = false;
            txt_msg = !txt_save() ? "Could not save it" : !txt_saved_ini ? "Saved" :
                cfg.bad != 0 ? "Saved: see Diagnostics (O, D)" : "Saved: settings in use now";
            txt_known = 1;                  // the pages may have moved
            txt_page = 0;
        } else if (r == DetEditorBig::DISCARD) {
            txt_editing = false;
        }
    } else if (key == 'x' || key == 0x18) {
        nmi_rom_data[0] = 1;
        return;
    } else if (key == 'n' || key == ' ') {
        if (!txt_last && txt_page + 1 < txt_known) txt_page++;
    } else if (key == 'p') {
        if (txt_page > 0) txt_page--;
    } else if (key == 'e') {
        txt_edit_start();
    }
    txt_draw(screen);
    preview_screen = screen;
    // typing in the editor: a held key repeats (the ROM's keywait)
    nmi_rom_data[START_KEYREPEAT] = txt_editing && !txt_editor()->prompting() ? 1 : 0;
    nmi_rom_data[0] = 0;
}

// ---- The tape position kept with a snapshot ----
// A snapshot saved while a tape is being played (S, Q) or made from a tape (C) gets a note
// saying which tape and how far it had got. Loading the snapshot then starts the tape there
// (the 48K trap, or tape in for a 128K snapshot), so a multi-load game can load its next level.
// The notes are kept together in TAPE_NOTE_DIR, "/Tape Files" in the root of the snapshot's
// drive, so the games' folders only have games in them. A note is named after its snapshot's
// path with each / as ~: /Games/OutRun.z80 has "/Tape Files/Games~OutRun.z80.tape". It is a
// few lines of text, the last one the snapshot it is for (a note for another snapshot of the
// same name, as "a~b/c" and "a/b~c" would be, is not used):
//     PicoMenu tape position
//     drive 1
//     block 8
//     tape /Games/OutRun.tap
//     snap /Games/OutRun.z80
// The note follows its snapshot when that, or a folder it is in, is erased, renamed, moved or
// copied. Earlier builds kept "<snapshot>.tape" beside the snapshot: such a note is still read,
// and is moved into /Tape Files the first time it is used.
static const char TAPE_NOTE_EXT[] = ".tape";
static const char TAPE_NOTE_TITLE[] = "PicoMenu tape position";
static const char TAPE_NOTE_DIR[] = "/Tape Files";
static const size_t TAPE_NOTE_NAME_MAX = 255;       // a file's name (FAT and littlefs)
// One path buffer for all of them (none calls another while using it): the RAM left over is
// the heap, which littlefs needs (a test build had static buffers in each and too little heap). The
// rest is in the list's memory (list_pool), which is free once walk_stop() has been called:
//   0 - 16K              tape_notes_follow_folder: the names of the notes it changes
//   16K - 17K            a note's text (TAPE_NOTE_TEXT)
//   17K - end-3 paths    lent to FatFs to make /Tape Files (it clears a cluster)
//   start / end-3 paths  the tape's path, the snapshot's (and the new one) while notes change
static struct {
    char path[BROWSE_PATH_MAX + 24];        // a note's path
} tape_note_mem;
static const size_t TAPE_NOTE_TEXT_AT = 16384, TAPE_NOTE_TEXT_MAX = 1024;
static const size_t TAPE_NOTE_LEND_AT = TAPE_NOTE_TEXT_AT + TAPE_NOTE_TEXT_MAX;
static_assert(TAPE_NOTE_LEND_AT % 8 == 0 && LIST_POOL_SIZE > TAPE_NOTE_LEND_AT + 8192 + 3 * BROWSE_PATH_MAX,
    "room in list_pool for the notes");
static char* tape_note_text() { return list_pool + TAPE_NOTE_TEXT_AT; }
static char dirpath_tape_notes[sizeof(TAPE_NOTE_DIR) + 4];     // /Tape Files as the drive's folder is opened

// The note of the snapshot snap (a path on its drive)
static bool tape_note_path(const char* snap, char* out, size_t size) {
    const char* s = snap[0] == '/' ? snap + 1 : snap;
    int n = snprintf(out, size, "%s/%s%s", TAPE_NOTE_DIR, s, TAPE_NOTE_EXT);
    if (n <= 0 || n >= (int) size) return false;
    char* name = out + sizeof(TAPE_NOTE_DIR);       // after "/Tape Files/"
    for (char* c = name; *c != '\0'; c++) {
        if (*c == '/') *c = '~';
    }
    return strlen(name) <= TAPE_NOTE_NAME_MAX;
}

// Where earlier builds kept it: beside the snapshot
static bool tape_note_old_path(const char* snap, char* out, size_t size) {
    int n = snprintf(out, size, "%s%s", snap, TAPE_NOTE_EXT);
    return n > 0 && n < (int) size;
}

// /Tape Files on the drive, made if it is not there. walk_stop() has been called.
static void tape_files_dir(uint8_t drive) {
    const Drive d = fsops::drive_of(drive);
    if (fsops::kind(d, TAPE_NOTE_DIR) != fsops::KIND_DIR) {
        // FatFs clears the new folder's cluster: it is lent a part of the list's memory that
        // tpath (which may be in it too) is not in
        if (drive == 1) {
            ff_pool = reinterpret_cast<unsigned char*>(list_pool) + TAPE_NOTE_LEND_AT;
            ff_pool_size = sizeof(list_pool) - TAPE_NOTE_LEND_AT - 3 * BROWSE_PATH_MAX;
        }
        uint8_t made = fsops::make_dir(d, TAPE_NOTE_DIR);
        ff_pool = nullptr;
        ff_pool_size = 0;
        LOG("Tape note: made %s on drive %d: %d\n", TAPE_NOTE_DIR, drive, made);
    }
}

static void tape_note_write(uint8_t drive, const char* snap, uint8_t tdrive, const char* tpath, uint32_t block) {
    char* path = tape_note_mem.path;
    walk_stop();                                    // the list's memory is used
    char* text = tape_note_text();
    const Drive d = fsops::drive_of(drive);
    tape_files_dir(drive);
    if (!tape_note_path(snap, path, sizeof(tape_note_mem.path))) return;
    int n = snprintf(text, TAPE_NOTE_TEXT_MAX, "%s\r\ndrive %u\r\nblock %u\r\ntape %s\r\nsnap %s\r\n",
        TAPE_NOTE_TITLE, (unsigned) (tdrive == 1 ? 1 : 0), (unsigned) block, tpath, snap);
    if (n <= 0 || n >= (int) TAPE_NOTE_TEXT_MAX) return;
    if (fsops::kind(d, path) == fsops::KIND_FILE) {
        fsops::remove_path(d, path);                // FileHandle makes a new file
    }
    FileHandle f;
    if (!f.open(d, path, false)) {
        LOG("Tape note: cannot write %s\n", path);
        return;
    }
    bool ok = f.write(text, (size_t) n) == n;
    f.close();
    LOG("Tape note %s: block %u of %s%s\n", path, (unsigned) block, tpath, ok ? "" : " (not written)");
}

// Reads the note at path. snap (if not nullptr, BROWSE_PATH_MAX bytes) gets the snapshot it says
// it is for ("" in an earlier build's note)
static bool tape_note_parse(uint8_t drive, const char* path, char* snap, uint8_t& tdrive, char* tpath,
                            size_t tsize, uint32_t& block) {
    walk_stop();                                    // the list's memory is used
    char* text = tape_note_text();
    FileHandle f;
    if (!f.open(fsops::drive_of(drive), path, true)) return false;
    int n = f.read(text, TAPE_NOTE_TEXT_MAX - 1);
    f.close();
    if (n <= 0) return false;
    text[n] = '\0';
    if (strncmp(text, TAPE_NOTE_TITLE, sizeof(TAPE_NOTE_TITLE) - 1) != 0) return false;
    bool have_drive = false, have_block = false, have_tape = false;
    if (snap != nullptr) snap[0] = '\0';
    for (char* line = text; *line != '\0'; ) {
        char* end = line + strcspn(line, "\r\n");
        char saved = *end;
        *end = '\0';
        if (strncmp(line, "drive ", 6) == 0) {
            tdrive = (uint8_t) (atoi(line + 6) == 1 ? 1 : 0);
            have_drive = true;
        } else if (strncmp(line, "block ", 6) == 0) {
            block = (uint32_t) strtoul(line + 6, nullptr, 10);
            have_block = true;
        } else if (strncmp(line, "tape ", 5) == 0 && line[5] == '/') {
            snprintf(tpath, tsize, "%s", line + 5);
            have_tape = strlen(line + 5) < tsize;
        } else if (strncmp(line, "snap ", 5) == 0 && snap != nullptr) {
            snprintf(snap, BROWSE_PATH_MAX, "%s", line + 5);
        }
        *end = saved;
        line = end + strspn(end, "\r\n");
    }
    return have_drive && have_block && have_tape;
}

static void tape_note_remove(uint8_t drive, const char* snap) {
    char* path = tape_note_mem.path;
    const Drive d = fsops::drive_of(drive);
    if (tape_note_path(snap, path, sizeof(tape_note_mem.path)) && fsops::kind(d, path) == fsops::KIND_FILE) {
        fsops::remove_path(d, path);
    }
    if (tape_note_old_path(snap, path, sizeof(tape_note_mem.path)) && fsops::kind(d, path) == fsops::KIND_FILE) {
        fsops::remove_path(d, path);
    }
}

// The note of the snapshot snap. An earlier build's note beside it is moved into /Tape Files.
static bool tape_note_read(uint8_t drive, const char* snap, uint8_t& tdrive, char* tpath, size_t tsize,
                           uint32_t& block) {
    char* path = tape_note_mem.path;
    // whose note it is: in the list's memory (free: no folder is being read while a note is)
    char* who = list_pool + sizeof(list_pool) - BROWSE_PATH_MAX;
    if (tape_note_path(snap, path, sizeof(tape_note_mem.path)) &&
        tape_note_parse(drive, path, who, tdrive, tpath, tsize, block)) {
        if (who[0] == '\0' || strcasecmp(who, snap) == 0) {
            return true;
        }
        LOG("Tape note %s is for %s, not %s\n", path, who, snap);
        return false;
    }
    if (!tape_note_old_path(snap, path, sizeof(tape_note_mem.path)) ||
        !tape_note_parse(drive, path, nullptr, tdrive, tpath, tsize, block)) {
        return false;
    }
    LOG("Tape note: moving %s into %s\n", path, TAPE_NOTE_DIR);
    tape_note_write(drive, snap, tdrive, tpath, block);
    if (tape_note_old_path(snap, path, sizeof(tape_note_mem.path))) {
        fsops::remove_path(fsops::drive_of(drive), path);
    }
    return true;
}

// After a snapshot file is erased (to == nullptr), renamed or moved, or copied (keep): its note
// goes the same way. The list's memory is free (walk_stop has been called).
static void tape_note_follow(uint8_t from_drive, const char* from, uint8_t to_drive, const char* to, bool keep) {
    if (to == nullptr) {
        tape_note_remove(from_drive, from);
        return;
    }
    char* tpath = list_pool;                    // the tape's path, while the note is rewritten
    uint8_t tdrive = 0;
    uint32_t block = 0;
    if (!tape_note_read(from_drive, from, tdrive, tpath, BROWSE_PATH_MAX, block)) {
        tape_note_remove(to_drive, to);         // an old note of a file that was replaced
        return;
    }
    tape_note_write(to_drive, to, tdrive, tpath, block);
    if (!keep) {
        tape_note_remove(from_drive, from);
    }
    LOG("Tape note follows %s -> %s\n", from, to);
}

// After a folder is erased (to == nullptr), renamed or moved, or copied (keep): the notes of the
// snapshots in it (by their names: the folder's path with / as ~, then ~) go the same way. Notes
// beside the snapshots, from earlier builds, went with the folder already. The list's memory is
// free (walk_stop has been called), and so is find_dirs[0].
static void tape_notes_follow_folder(uint8_t drive, const char* from, const char* to, bool keep) {
    const Drive d = fsops::drive_of(drive);
    walk_stop();
    if (fsops::kind(d, TAPE_NOTE_DIR) != fsops::KIND_DIR) return;
    // the names' start, "Games~Old~": in the path buffer for now
    char* prefix = tape_note_mem.path;
    {
        const char* f = from[0] == '/' ? from + 1 : from;
        int n = snprintf(prefix, sizeof(tape_note_mem.path), "%s/", f);
        if (n <= 1 || n >= (int) sizeof(tape_note_mem.path)) return;
        for (char* c = prefix; *c != '\0'; c++) if (*c == '/') *c = '~';
    }
    const size_t plen = strlen(prefix);
    // the matching names, one after the other in the list's memory (the folder is read once,
    // and only changed after)
    char* names = list_pool;
    char* tpath = list_pool + sizeof(list_pool) - 3 * BROWSE_PATH_MAX;
    char* snap = list_pool + sizeof(list_pool) - 2 * BROWSE_PATH_MAX;
    char* newsnap = list_pool + sizeof(list_pool) - BROWSE_PATH_MAX;
    const size_t room = TAPE_NOTE_TEXT_AT;
    size_t used = 0;
    int count = 0;
    fsops::dir_open_path(d, TAPE_NOTE_DIR, dirpath_tape_notes, sizeof(dirpath_tape_notes));
    DirHandle& dir = find_dirs[0];
    if (!dir.open(dirpath_tape_notes)) return;
    while (!dir.atEnd()) {
        const char* name = dir.fileName();
        size_t len = strlen(name);
        if (!dir.isDirectory() && len > plen && strncasecmp(name, prefix, plen) == 0 && used + len + 1 <= room) {
            memcpy(names + used, name, len + 1);
            used += len + 1;
            count++;
        }
        if (!dir.next()) break;
    }
    dir.close();
    for (size_t at = 0; at < used; at += strlen(names + at) + 1) {
        char* path = tape_note_mem.path;
        snprintf(path, sizeof(tape_note_mem.path), "%s/%s", TAPE_NOTE_DIR, names + at);
        uint8_t tdrive = 0;
        uint32_t block = 0;
        if (!tape_note_parse(drive, path, snap, tdrive, tpath, BROWSE_PATH_MAX, block)) continue;
        size_t flen = strlen(from);
        if (snap[0] == '\0' || strncasecmp(snap, from, flen) != 0 || snap[flen] != '/') continue;
        if (to == nullptr) {
            fsops::remove_path(d, path);
            continue;
        }
        int n = snprintf(newsnap, BROWSE_PATH_MAX, "%s%s", to, snap + flen);
        if (n <= 0 || n >= (int) BROWSE_PATH_MAX) continue;
        if (!keep) fsops::remove_path(d, path);
        tape_note_write(drive, newsnap, tdrive, tpath, block);
    }
    LOG("Tape notes of %s: %d %s\n", from, count, to == nullptr ? "erased" : keep ? "copied" : "moved");
}

// After a tape (or a folder, is_dir) was renamed, moved or put in the Waste Bin: the notes that
// say a snapshot was taken on it (on either drive) now say where it is, so the snapshot still
// goes on with the tape. For a file only a .tap or .tzx is looked for. The list's memory is free
// (walk_stop has been called), and so is find_dirs[0].

// ---- Tape labels: a short label for a line of the Virtual Tape Player (L there) ----
// Kept in /Tape Files on the tape's own drive, as "<the tape's path, / as ~>.labels":
//     PicoMenu tape labels
//     tape /Games/OutRun.tzx
//     <the line's first data block> <its label>
// one line per label. They follow the tape (and its folder) when it is renamed, moved or put in
// the Waste Bin (tape_notes_retarget), and go when it is erased for good. The file is read
// and made in a part of snap_load_buffer nothing else uses while the tape player is up (past
// the menu grab's screen).
static const char TAPE_LABELS_EXT[] = ".labels";
static const char TAPE_LABELS_TITLE[] = "PicoMenu tape labels";
static const uint32_t TAPE_LABELS_OFFSET = 90112, TAPE_LABELS_MAX = 2048;
#if PICOMENU_NET
static_assert(ZX_STATE_AT >= TAPE_LABELS_OFFSET + 2 * TAPE_LABELS_MAX, "ZXDB's state is after the tape labels");
#endif
static_assert(TAPE_LABELS_OFFSET >= 82944 + 6912 && TAPE_LABELS_OFFSET + 2 * TAPE_LABELS_MAX <= 98304,
    "the labels between the menu grab and the Help text");
#ifdef PICOMENU_BT_SHARED
static_assert(TAPE_LABELS_OFFSET + 2 * TAPE_LABELS_MAX <= RADIO_ARENA_END,
    "the labels before the Bluetooth's memory (the tape player is used while it is on)");
#endif
static char* tape_labels_text() { return snap_load_buffer + TAPE_LABELS_OFFSET; }

// The labels file of the tape tpath (a path on its drive), in tape_note_mem.path
static bool tape_labels_path(const char* tpath) {
    char* out = tape_note_mem.path;
    const char* s = tpath[0] == '/' ? tpath + 1 : tpath;
    int n = snprintf(out, sizeof(tape_note_mem.path), "%s/%s%s", TAPE_NOTE_DIR, s, TAPE_LABELS_EXT);
    if (n <= 0 || n >= (int) sizeof(tape_note_mem.path)) return false;
    char* name = out + sizeof(TAPE_NOTE_DIR);
    for (char* c = name; *c != '\0'; c++) if (*c == '/') *c = '~';
    return strlen(name) <= TAPE_NOTE_NAME_MAX;
}

// Reads the tape's labels file into tape_labels_text(): its length, 0 if it has none
static uint32_t tape_labels_read(uint8_t drive, const char* tpath) {
    char* text = tape_labels_text();
    text[0] = '\0';
    if (!tape_labels_path(tpath)) return 0;
    FileHandle f;
    if (!f.open(fsops::drive_of(drive), tape_note_mem.path, true)) return 0;
    int n = f.read(text, TAPE_LABELS_MAX - 1);
    f.close();
    if (n <= 0 || strncmp(text, TAPE_LABELS_TITLE, sizeof(TAPE_LABELS_TITLE) - 1) != 0) {
        text[0] = '\0';
        return 0;
    }
    text[n] = '\0';
    return (uint32_t) n;
}

// The label of the line whose first data block is 'block', from what tape_labels_read read
static bool tape_label_of(uint32_t block, char* out, size_t size) {
    for (const char* line = tape_labels_text(); *line != '\0'; ) {
        size_t len = strcspn(line, "\r\n");
        if (line[0] >= '0' && line[0] <= '9') {
            char* after = nullptr;
            unsigned long b = strtoul(line, &after, 10);
            if (b == block && after != nullptr && *after == ' ' && after < line + len) {
                size_t l = (size_t) (line + len - after - 1);
                if (l >= size) l = size - 1;
                memcpy(out, after + 1, l);
                out[l] = '\0';
                return l > 0;
            }
        }
        line += len;
        line += strspn(line, "\r\n");
    }
    return false;
}

// Writes the labels in text (lines "<block> <label>", nothing else) for the tape; none: the file goes
static bool tape_labels_write(uint8_t drive, const char* tpath, const char* lines) {
    walk_stop();
    const Drive d = fsops::drive_of(drive);
    if (lines[0] == '\0') {
        if (tape_labels_path(tpath) && fsops::kind(d, tape_note_mem.path) == fsops::KIND_FILE) {
            fsops::remove_path(d, tape_note_mem.path);
        }
        return true;
    }
    tape_files_dir(drive);
    if (!tape_labels_path(tpath)) return false;
    if (fsops::kind(d, tape_note_mem.path) == fsops::KIND_FILE) fsops::remove_path(d, tape_note_mem.path);
    FileHandle f;
    if (!f.open(d, tape_note_mem.path, false)) return false;
    char head[BROWSE_PATH_MAX + 32];
    int hn = snprintf(head, sizeof(head), "%s\r\ntape %s\r\n", TAPE_LABELS_TITLE, tpath);
    bool ok = hn > 0 && hn < (int) sizeof(head) && f.write(head, (size_t) hn) == hn;
    size_t ln = strlen(lines);
    ok = ok && f.write(lines, ln) == (int) ln;
    f.close();
    return ok;
}

// The label lines of what tape_labels_read read (without the title and tape lines), with the
// line for 'block' changed to label (removed if label is ""), into the second half of the space
static const char* tape_labels_with(uint32_t block, const char* label) {
    char* out = tape_labels_text() + TAPE_LABELS_MAX;
    size_t used = 0;
    bool done = false;
    for (const char* line = tape_labels_text(); *line != '\0'; ) {
        size_t len = strcspn(line, "\r\n");
        if (line[0] >= '0' && line[0] <= '9') {
            char* after = nullptr;
            unsigned long b = strtoul(line, &after, 10);
            if (b == block) {
                if (label[0] != '\0' && !done) {
                    int n = snprintf(out + used, TAPE_LABELS_MAX - used, "%lu %s\r\n", b, label);
                    if (n > 0 && used + n < TAPE_LABELS_MAX) used += n;
                }
                done = true;
            } else if (used + len + 3 < TAPE_LABELS_MAX) {
                memcpy(out + used, line, len);
                used += len;
                out[used++] = '\r';
                out[used++] = '\n';
            }
        }
        line += len;
        line += strspn(line, "\r\n");
    }
    if (!done && label[0] != '\0') {
        int n = snprintf(out + used, TAPE_LABELS_MAX - used, "%lu %s\r\n", (unsigned long) block, label);
        if (n > 0 && used + n < TAPE_LABELS_MAX) used += n;
    }
    out[used] = '\0';
    return out;
}

// After a tape (or a folder of them, is_dir) was renamed, moved or put in the Waste Bin, or
// erased (to == nullptr): its labels go the same way. The list's memory is free.
static void tape_labels_follow(uint8_t from_drive, const char* from, uint8_t to_drive, const char* to, bool is_dir) {
    if (sna_in_buffer) return;          // a snapshot waiting to be saved fills the space the file is read in
    walk_stop();
    if (!is_dir) {
        if (tape_labels_read(from_drive, from) == 0) return;
        // the label lines start after the title and tape lines
        const char* lines = tape_labels_text();
        for (int skip = 0; skip < 2 && *lines != '\0'; skip++) {
            lines += strcspn(lines, "\r\n");
            lines += strspn(lines, "\r\n");
        }
        char* keep = tape_labels_text() + TAPE_LABELS_MAX;
        snprintf(keep, TAPE_LABELS_MAX, "%s", lines);
        tape_labels_write(from_drive, from, "");
        if (to != nullptr) tape_labels_write(to_drive, to, keep);
        LOG("Tape labels follow %s -> %s\n", from, to != nullptr ? to : "(erased)");
        return;
    }
    // a folder: the labels files named after tapes in it, renamed (or removed) on that drive
    const Drive d = fsops::drive_of(from_drive);
    if (fsops::kind(d, TAPE_NOTE_DIR) != fsops::KIND_DIR) return;
    char* prefix = tape_labels_text();
    const char* f0 = from[0] == '/' ? from + 1 : from;
    int pn = snprintf(prefix, 256, "%s/", f0);
    if (pn <= 1 || pn >= 256) return;
    for (char* c = prefix; *c != '\0'; c++) if (*c == '/') *c = '~';
    char* names = list_pool;
    size_t used = 0;
    fsops::dir_open_path(d, TAPE_NOTE_DIR, dirpath_tape_notes, sizeof(dirpath_tape_notes));
    DirHandle& dir = find_dirs[0];
    if (!dir.open(dirpath_tape_notes)) return;
    const size_t el = sizeof(TAPE_LABELS_EXT) - 1;
    while (!dir.atEnd()) {
        const char* name = dir.fileName();
        size_t len = strlen(name);
        if (!dir.isDirectory() && len > (size_t) pn + el && strncasecmp(name, prefix, pn) == 0 &&
            strcasecmp(name + len - el, TAPE_LABELS_EXT) == 0 && used + len + 1 <= TAPE_NOTE_TEXT_AT) {
            memcpy(names + used, name, len + 1);
            used += len + 1;
        }
        if (!dir.next()) break;
    }
    dir.close();
    char* tfrom = list_pool + sizeof(list_pool) - 2 * BROWSE_PATH_MAX;
    char* tto = list_pool + sizeof(list_pool) - BROWSE_PATH_MAX;
    for (size_t at = 0; at < used; at += strlen(names + at) + 1) {
        // the tape's own path back from the name: from + the rest, with ~ as / again
        const char* rest = names + at + pn;
        int n = snprintf(tfrom, BROWSE_PATH_MAX, "%s/%.*s", from, (int) (strlen(rest) - el), rest);
        if (n <= 0 || n >= (int) BROWSE_PATH_MAX) continue;
        for (char* c = tfrom + strlen(from) + 1; *c != '\0'; c++) if (*c == '~') *c = '/';
        if (to != nullptr) {
            n = snprintf(tto, BROWSE_PATH_MAX, "%s%s", to, tfrom + strlen(from));
            if (n <= 0 || n >= (int) BROWSE_PATH_MAX) continue;
        }
        tape_labels_follow(from_drive, tfrom, to_drive, to != nullptr ? tto : nullptr, false);
    }
}

static void tape_notes_retarget(uint8_t from_drive, const char* from, uint8_t to_drive, const char* to, bool is_dir) {
    if (from == nullptr || to == nullptr) return;
    size_t flen = strlen(from);
    if (!is_dir && (flen < 4 || (strcasecmp(from + flen - 4, ".tap") != 0 && !tape_is_tzx(from)))) return;
    tape_labels_follow(from_drive, from, to_drive, to, is_dir);   // the tape's labels, on its own drive
    walk_stop();
    char* names = list_pool;
    char* tpath = list_pool + sizeof(list_pool) - 3 * BROWSE_PATH_MAX;
    char* snap = list_pool + sizeof(list_pool) - 2 * BROWSE_PATH_MAX;
    char* newtpath = list_pool + sizeof(list_pool) - BROWSE_PATH_MAX;
    int changed = 0;
    for (uint8_t nd = 0; nd < 2; nd++) {
        if (nd == 1 && !sd_card_mounted) continue;
        const Drive d = fsops::drive_of(nd);
        if (fsops::kind(d, TAPE_NOTE_DIR) != fsops::KIND_DIR) continue;
        size_t used = 0;
        fsops::dir_open_path(d, TAPE_NOTE_DIR, dirpath_tape_notes, sizeof(dirpath_tape_notes));
        DirHandle& dir = find_dirs[0];
        if (!dir.open(dirpath_tape_notes)) continue;
        while (!dir.atEnd()) {
            const char* name = dir.fileName();
            size_t len = strlen(name);
            if (!dir.isDirectory() && len > sizeof(TAPE_NOTE_EXT) - 1 &&
                strcasecmp(name + len - (sizeof(TAPE_NOTE_EXT) - 1), TAPE_NOTE_EXT) == 0 &&
                used + len + 1 <= TAPE_NOTE_TEXT_AT) {
                memcpy(names + used, name, len + 1);
                used += len + 1;
            }
            if (!dir.next()) break;
        }
        dir.close();
        for (size_t at = 0; at < used; at += strlen(names + at) + 1) {
            char* path = tape_note_mem.path;
            snprintf(path, sizeof(tape_note_mem.path), "%s/%s", TAPE_NOTE_DIR, names + at);
            uint8_t tdrive = 0;
            uint32_t block = 0;
            if (!tape_note_parse(nd, path, snap, tdrive, tpath, BROWSE_PATH_MAX, block)) continue;
            if (snap[0] == '\0' || tdrive != from_drive || strncasecmp(tpath, from, flen) != 0) continue;
            if (is_dir ? tpath[flen] != '/' : tpath[flen] != '\0') continue;
            int n = snprintf(newtpath, BROWSE_PATH_MAX, "%s%s", to, tpath + flen);
            if (n <= 0 || n >= (int) BROWSE_PATH_MAX) continue;
            tape_note_write(nd, snap, to_drive, newtpath, block);
            changed++;
        }
    }
    LOG("Tape notes on %s now on %s: %d\n", from, to, changed);
}

// A snapshot was saved (S or Q): if a tape is being played, a note of where it is; otherwise
// any old note of a file of that name goes
static void tape_note_after_save(uint8_t drive, const char* snap) {
    if (tape_service_active && !tape_ejected && !tape_rec) {
        tape_note_write(drive, snap, tape_drive, tape_name, tape_done);
    } else {
        tape_note_remove(drive, snap);
    }
}

// Loading a snapshot with a note: the tape trap is started on that tape at that block. Only
// for a 48K snapshot, or a 128K one locked in 48K mode (port 0x7FFD bits 4 and 5).
static bool tape_goto(uint32_t block);

static void tape_note_resume(uint8_t drive, const char* snap, bool paged, uint8_t port_7ffd) {
    uint8_t tdrive = 0;
    uint32_t block = 0;
    char* tpath = tape_name;         // the trap is stopped (load_snapshot_file), so it is free
    if (!tape_note_read(drive, snap, tdrive, tpath, sizeof(tape_name), block)) return;
    if (paged && (port_7ffd & 0x30) != 0x30) {
        // a 128K game: the trap's ROM is a 48K one, so the tape goes in for the Spectrum's own
        // ROMs instead (tape in), at the block the note says
        const char* why = tape_in_start(tdrive, tpath, block);
        if (why != nullptr) {
            const char* name = strrchr(tpath, '/');
            set_last_error("Tape not found", name != nullptr ? name + 1 : tpath);
            LOG("Tape note: cannot put in %s\n", tpath);
        }
        return;
    }
    uint8_t* scratch = nullptr;
    if (paged) {
        walk_stop();                // the 128K image fills the snapshot buffer: the ROM goes here
        scratch = reinterpret_cast<uint8_t*>(list_pool);
    }
    tape_drive = tdrive;
    if (!tape_open_file() || !tape_load_patched_rom(false, scratch)) {
        if (tape_file_open) {
            tape_file.close();
            tape_file_open = false;
        }
        const char* name = strrchr(tpath, '/');
        set_last_error("Tape not found", name != nullptr ? name + 1 : tpath);
        LOG("Tape note: cannot play %s\n", tpath);
        return;
    }
    tape_service_active = true;
    tape_ejected = false;
    rom_state.flags_on_nmi_exit = 1;
    rom_state.writableStartAddress = TAPE_MBOX;
    tape_goto(block);
    level_loaders_scan_pending = true;      // once the snapshot is in
    LOG("Tape note: %s from block %u\n", tpath, (unsigned) tape_done);
}


// ---- E in Tools: erase all of a game's assets (ACTION_ASSETS) ----
// (Only in the Pico W firmware, which has room for it: the standard one's program is all but
// full, and says so.)
// A game's name is typed (part of it), the games that have assets and match are listed (from
// the folders assets are kept in), and one is picked. Its assets are then found, by category:
//   1 Autosaves       /Autosave/<game> (the folder)
//   2 Saves           /Save Games/<game> (its slots, the folder), and its quick saves,
//                     "<game> Save Game[ n].z80" (.sna .szx) in /Save Games, the save folder and
//                     the folder of the game last loaded
//   3 Game Notes      /Game Notes/<game>.txt (its notes) and <game>.info.txt (its details)
//   4 Pokes           /Poke List/<game>.pok
//   5 Tape files      /Tape Files: the notes of where a tape had got to for its snapshots, and
//                     the labels of its tapes
//   6 Screen grabs    /Scraper/Screen Grabs and /Screen Grabs: <game> and its categories' grabs
//   7 High scores     /High Scores/<game>.txt (where its table is), .bin (the table), .bak
//                     (the Pico W build: the standard one has no high scores, so 6 categories)
// 1-7 (1-6) turn a category on or off, E erases what is on (Y to be sure): to the Waste Bin, as E in
// Browse Games does (waste_bin = off: for good). The names listed and the assets found are kept
// in snap_load_buffer (free while the screen is up), after the screen.
static const int AS_CATS = PICOMENU_HISCORE ? 7 : 6;
static const uint32_t AS_NAMES_AT = 16384, AS_NAMES_SIZE = 16384;           // the names, \0 after each
static const uint32_t AS_INDEX_AT = AS_NAMES_AT + AS_NAMES_SIZE;            // where each one starts
static const int AS_NAMES_MAX = 1024;
static const uint32_t AS_FOUND_AT = AS_INDEX_AT + AS_NAMES_MAX * 2;          // the assets: cat, dir, path, \0
static const uint32_t AS_FOUND_SIZE = 65536 - AS_FOUND_AT;
static_assert(AS_FOUND_AT + 4096 <= 65536, "the assets' room");
static const int AS_PAGE = 10;
#if PICOMENU_ASSETS
enum AsMode : uint8_t { AS_TYPE, AS_LIST, AS_CATS_SCREEN, AS_ASK, AS_DONE };
struct AsState {
    char typed[32];
    char game[screengrab::NAME_MAX + 1];
    char said[40];
    uint16_t count[AS_CATS];
    uint32_t found_used;
    int names, page;
    uint8_t mode, on;               // on: a bit for each category
    bool found_full;                // there were more than fit (erase again for the rest)
};
// (in snap_load_buffer after the screen, as the RAM is short)
static const uint32_t AS_STATE_AT = 6912;
static_assert(AS_STATE_AT + sizeof(AsState) <= AS_NAMES_AT, "the assets screen's state fits");
#define as (*reinterpret_cast<AsState*>(snap_load_buffer + AS_STATE_AT))
static const char* const AS_CAT_NAME[AS_CATS] = { "Autosaves", "Save slots, saves", "Game Notes", "Pokes (.pok)",
    "Tape files", "Screen grabs",
#if PICOMENU_HISCORE
    "High scores"
#endif
};

static char* as_names() { return snap_load_buffer + AS_NAMES_AT; }
static uint16_t* as_index() { return reinterpret_cast<uint16_t*>(snap_load_buffer + AS_INDEX_AT); }
static char* as_found() { return snap_load_buffer + AS_FOUND_AT; }

// "OutRun Loading Screen2" -> "OutRun": a grab's file name (without .scr) as its game's name
static void as_grab_base(char* s) {
    size_t n = strlen(s), d = 0;
    while (d < 3 && d < n && s[n - 1 - d] >= '0' && s[n - 1 - d] <= '9') d++;
    if (d > 0 && d < n && s[n - 1 - d] != ' ') { n -= d; s[n] = '\0'; }       // (a number of its own: "Area 51")
    for (int c = screengrab::GRAB_CATEGORIES - 1; c > 0; c--) {
        size_t sl = strlen(screengrab::SUFFIX[c]);
        if (n > sl && strcasecmp(s + n - sl, screengrab::SUFFIX[c]) == 0) { s[n - sl] = '\0'; return; }
    }
}

// Whether s has part in it (capitals or not)
static bool as_has(const char* s, const char* part) {
    size_t n = strlen(part);
    for (; *s != '\0'; s++) if (strncasecmp(s, part, n) == 0) return true;
    return false;
}

// A game's name for the list, if it matches what was typed and is not there already
static void as_name_add(const char* name) {
    char base[screengrab::NAME_MAX + 1];
    if (screengrab::base_name(name, base) == 0) return;
    if (as.typed[0] != '\0' && !as_has(base, as.typed)) return;
    char* names = as_names();
    uint16_t* index = as_index();
    for (int i = 0; i < as.names; i++) {
        if (strcasecmp(names + index[i], base) == 0) return;
    }
    uint32_t at = as.names > 0 ? index[as.names - 1] + (uint32_t) strlen(names + index[as.names - 1]) + 1 : 0;
    size_t n = strlen(base);
    if (as.names >= AS_NAMES_MAX || at + n + 1 > AS_NAMES_SIZE) return;
    memcpy(names + at, base, n + 1);
    index[as.names++] = (uint16_t) at;
}

// A Tape Files note's (or tape labels') name as its game: "Games~OutRun.z80.tape" -> OutRun
static bool as_tape_game(const char* fn, char* out) {
    char p[sizeof(sd_cwd)];
    snprintf(p, sizeof(p), "/%s", fn);
    char* ext = strrchr(p, '.');
    if (ext == nullptr || (strcasecmp(ext, TAPE_NOTE_EXT) != 0 && strcasecmp(ext, TAPE_LABELS_EXT) != 0)) return false;
    *ext = '\0';                                        // "/Games~OutRun.z80"
    for (char* c = p; *c != '\0'; c++) if (*c == '~') *c = '/';
    char* slash = strrchr(p, '/');
    char* dot = strrchr(slash, '.');
    if (dot != nullptr) *dot = '\0';                    // "/Games/OutRun"
    *slash = '\0';
    return game_base(p[0] != '\0' ? p : "/", slash + 1, out) > 0;
}

// Each file (or folder: dirs) in folder of the SD card, to fn(name, is_dir)
template <class Fn>
static void as_each(const char* folder, Fn fn) {
    char path[sizeof(sd_cwd) + 8];
    snprintf(path, sizeof(path), "/sd%s", folder);
    DirHandle& d = grab_dir;
    if (!d.open(path)) return;
    while (!d.atEnd()) {
        char name[96];
        snprintf(name, sizeof(name), "%s", d.fileName());
        bool dir = d.isDirectory();
        fn(name, dir);
        if (!d.next()) break;
    }
    d.close();
}

// The games with assets that match what was typed, sorted
__attribute__((noinline)) static void as_list() {
    walk_stop();
    as.names = 0;
    as.page = 0;
    as_each(CP_FOLDER, [](const char* n, bool dir) { if (dir) as_name_add(n); });
    as_each(slots::FOLDER, [](const char* n, bool dir) { if (dir) as_name_add(n); });
    as_each(notes::FOLDER, [](char* n, bool dir) {
        size_t l = strlen(n);
        if (dir || n[0] == '~') return;
        if (l > 9 && strcasecmp(n + l - 9, ".info.txt") == 0) n[l - 9] = '\0';
        else if (l > 4 && strcasecmp(n + l - 4, ".txt") == 0) n[l - 4] = '\0';
        else return;
        as_name_add(n);
    });
    as_each(POKE_LIST_DIR, [](char* n, bool dir) {
        size_t l = strlen(n);
        if (!dir && l > 4 && strcasecmp(n + l - 4, ".pok") == 0) { n[l - 4] = '\0'; as_name_add(n); }
    });
    auto grabs = [](char* n, bool dir) {
        size_t l = strlen(n);
        if (dir || l < 5 || (strcasecmp(n + l - 4, ".scr") != 0 && strcasecmp(n + l - 4, ".png") != 0)) return;
        n[l - 4] = '\0';
        as_grab_base(n);
        as_name_add(n);
    };
    as_each(screengrab::SCRAPER_FOLDER, grabs);
    as_each(screengrab::OLD_FOLDER, grabs);
    as_each(TAPE_NOTE_DIR, [](const char* n, bool dir) {
        char g[screengrab::NAME_MAX + 1];
        if (!dir && as_tape_game(n, g)) as_name_add(g);
    });
#if PICOMENU_HISCORE
    as_each(hiscore::FOLDER, [](char* n, bool dir) {
        char* dot = strrchr(n, '.');
        if (dir || dot == nullptr || (strcasecmp(dot, ".txt") != 0 && strcasecmp(dot, ".bin") != 0 && strcasecmp(dot, ".bak") != 0)) return;
        *dot = '\0';
        as_name_add(n);
    });
#endif
    // in order (a few hundred at most: a simple sort)
    char* names = as_names();
    uint16_t* index = as_index();
    for (int i = 1; i < as.names; i++) {
        uint16_t v = index[i];
        int j = i - 1;
        while (j >= 0 && strcasecmp(names + index[j], names + v) > 0) { index[j + 1] = index[j]; j--; }
        index[j + 1] = v;
    }
}

// An asset found: kept (category, whether a folder, its path)
static void as_found_add(int cat, bool dir, const char* path) {
    size_t n = strlen(path);
    if (as.found_used + n + 3 > AS_FOUND_SIZE) { as.found_full = true; return; }
    char* f = as_found() + as.found_used;
    f[0] = (char) ('0' + cat);
    f[1] = dir ? 'd' : 'f';
    memcpy(f + 2, path, n + 1);
    as.found_used += (uint32_t) n + 3;
    as.count[cat]++;
}

// Its quick saves, "<game> Save Game[ n].z80" (.sna .szx): in /Save Games (where Q puts them),
// the folder snapshots are saved to (S), and the folder of the game last loaded. (Not the whole
// card: a card with a big collection took minutes to look through, the Spectrum waiting.)
static const char* as_folder = "";
static void as_quick_in(const char* folder) {
    as_folder = folder;
    as_each(folder, [](char* n, bool dir) {
        char* dot = strrchr(n, '.');
        if (dir || dot == nullptr || (strcasecmp(dot, ".z80") != 0 && strcasecmp(dot, ".sna") != 0 && strcasecmp(dot, ".szx") != 0)) return;
        *dot = '\0';
        char base[screengrab::NAME_MAX + 1];
        bool quick = screengrab::base_name(n, base) > 0 && strcasecmp(base, as.game) == 0 && strlen(base) != strlen(n);
        *dot = '.';
        if (!quick) return;
        char full[sizeof(sd_cwd) + 8];
        snprintf(full, sizeof(full), "%s/%s", strcmp(as_folder, "/") == 0 ? "" : as_folder, n);
        // (once: the same folder may be more than one of the three)
        for (uint32_t at = 0; at < as.found_used;) {
            const char* f = as_found() + at;
            if (strcasecmp(f + 2, full) == 0) return;
            at += (uint32_t) strlen(f) + 1;
        }
        as_found_add(1, false, full);
    });
}

__attribute__((noinline)) static void as_quick_saves() {
    as_quick_in(slots::FOLDER);
    if (save_drive == 1 && save_dir[0] != '\0') as_quick_in(save_dir);
    if (last_game_drive == 1) as_quick_in(last_game_dir[0] != '\0' ? last_game_dir : "/");
}

// All of the game's assets, found
__attribute__((noinline)) static void as_find() {
    walk_stop();
    memset(as.count, 0, sizeof(as.count));
    as.found_used = 0;
    as.found_full = false;
    char path[sizeof(sd_cwd) + 8];
    snprintf(path, sizeof(path), "%s/%s", CP_FOLDER, as.game);
    if (fsops::kind(Drive::EXTERNAL_SD_CARD, path) == fsops::KIND_DIR) as_found_add(0, true, path);
    snprintf(path, sizeof(path), "%s/%s", slots::FOLDER, as.game);
    if (fsops::kind(Drive::EXTERNAL_SD_CARD, path) == fsops::KIND_DIR) as_found_add(1, true, path);
    as_quick_saves();
    static const char* const NOTE_EXT[] = { ".txt", ".info.txt" };
    for (const char* e : NOTE_EXT) {
        snprintf(path, sizeof(path), "%s/%s%s", notes::FOLDER, as.game, e);
        if (fsops::kind(Drive::EXTERNAL_SD_CARD, path) == fsops::KIND_FILE) as_found_add(2, false, path);
    }
    snprintf(path, sizeof(path), "%s/%s.pok", POKE_LIST_DIR, as.game);
    if (fsops::kind(Drive::EXTERNAL_SD_CARD, path) == fsops::KIND_FILE) as_found_add(3, false, path);
    as_each(TAPE_NOTE_DIR, [](const char* n, bool dir) {
        char g[screengrab::NAME_MAX + 1];
        if (dir || !as_tape_game(n, g) || strcasecmp(g, as.game) != 0) return;
        char p[sizeof(sd_cwd) + 8];
        snprintf(p, sizeof(p), "%s/%s", TAPE_NOTE_DIR, n);
        as_found_add(4, false, p);
    });
    as_each(screengrab::SCRAPER_FOLDER, [](const char* n, bool dir) {
        screengrab::Category c;
        int k;
        if (dir || !screengrab::parse_name(n, as.game, &c, &k)) return;
        char p[sizeof(sd_cwd) + 8];
        snprintf(p, sizeof(p), "%s/%s", screengrab::SCRAPER_FOLDER, n);
        as_found_add(5, false, p);
    });
    as_each(screengrab::OLD_FOLDER, [](const char* n, bool dir) {
        screengrab::Category c;
        int k;
        if (dir || !screengrab::parse_name(n, as.game, &c, &k)) return;
        char p[sizeof(sd_cwd) + 8];
        snprintf(p, sizeof(p), "%s/%s", screengrab::OLD_FOLDER, n);
        as_found_add(5, false, p);
    });
#if PICOMENU_HISCORE
    static const char* const HS_EXT[] = { "txt", "bin", "bak" };
    for (const char* e : HS_EXT) {
        snprintf(path, sizeof(path), "%s/%s.%s", hiscore::FOLDER, as.game, e);
        if (fsops::kind(Drive::EXTERNAL_SD_CARD, path) == fsops::KIND_FILE) as_found_add(6, false, path);
    }
#endif
    as.on = 0;
    for (int c = 0; c < AS_CATS; c++) if (as.count[c] > 0) as.on |= (uint8_t) (1 << c);
}

// What is on, erased (to the Waste Bin): how many, and how many could not be
__attribute__((noinline)) static void as_erase() {
    int done = 0, failed = 0;
    for (uint32_t at = 0; at < as.found_used;) {
        const char* f = as_found() + at;
        size_t n = strlen(f);
        at += (uint32_t) n + 1;
        int cat = f[0] - '0';
        if (!(as.on & (1 << cat))) continue;
        char path[sizeof(sd_cwd) + 8];
        snprintf(path, sizeof(path), "%s", f + 2);
        if (fsops::kind(Drive::EXTERNAL_SD_CARD, path) == fsops::KIND_NONE) continue;   // (gone with another)
        if (web_erase(path, f[1] == 'd')) done++;
        else failed++;
        LOG("Assets of %s: %s %s\n", as.game, path, failed ? "not erased" : "erased");
    }
#if PICOMENU_HISCORE
    hs.def = 0;                         // (a high score table may have gone: looked for again)
#endif
    if (failed > 0) snprintf(as.said, sizeof(as.said), "Erased %d, %d could not be", done, failed);
    else snprintf(as.said, sizeof(as.said), "Erased %d%s", done, cfg.waste_bin ? " (to the Waste Bin)" : "");
}

static void as_draw(uint8_t* screen) {
    slots_banner(screen, "", "Erase all assets");
    char t[48];
    auto text = [&](int r, const char* s, uint8_t a) { help_put_text(screen, r, 1, s, (int) strlen(s), a); };
    const char* keys = "X-Back";
    switch (as.mode) {
        case AS_TYPE:
            text(2, "The game (part of its name):", gameinfo::ATTR_HEADING);
            snprintf(t, sizeof(t), "%s_", as.typed);
            text(4, t, gameinfo::ATTR_TEXT);
            text(6, "ENTER lists the games that have", gameinfo::ATTR_DIM);
            text(7, "assets (nothing typed: all):", gameinfo::ATTR_DIM);
            text(8, "autosaves, saves, notes, pokes,", gameinfo::ATTR_DIM);
            text(9, "tape files, screen grabs and", gameinfo::ATTR_DIM);
            text(10, "high scores.", gameinfo::ATTR_DIM);
            keys = "ENTER-List  BREAK-Back";
            break;
        case AS_LIST: {
            int pages = (as.names + AS_PAGE - 1) / AS_PAGE;
            if (as.names == 0) text(2, "No games with assets match", gameinfo::ATTR_HEADING);
            else if (pages > 1) { snprintf(t, sizeof(t), "%d games, page %d of %d", as.names, as.page + 1, pages); text(2, t, gameinfo::ATTR_HEADING); }
            else { snprintf(t, sizeof(t), "%d game%s with assets", as.names, as.names == 1 ? "" : "s"); text(2, t, gameinfo::ATTR_HEADING); }
            for (int i = 0; i < AS_PAGE && as.page * AS_PAGE + i < as.names; i++) {
                snprintf(t, sizeof(t), "%d %.29s", (i + 1) % 10, as_names() + as_index()[as.page * AS_PAGE + i]);
                text(4 + i, t, gameinfo::ATTR_TEXT);
            }
            text(21, pages > 1 ? "1-0 Pick  N/P-Page  T-Type" : "1-0 Pick  T-Type again", gameinfo::ATTR_KEYS);
            break;
        }
        case AS_CATS_SCREEN:
        case AS_ASK:
        case AS_DONE: {
            snprintf(t, sizeof(t), "%.31s", as.game);
            text(2, t, gameinfo::ATTR_HEADING);
            int total = 0;
            for (int c = 0; c < AS_CATS; c++) {
                bool on = as.on & (1 << c);
                snprintf(t, sizeof(t), "%d %s %-17s %3u", c + 1, as.count[c] == 0 ? "   " : on ? "[x]" : "[ ]", AS_CAT_NAME[c],
                    (unsigned) as.count[c]);
                text(4 + c, t, as.count[c] == 0 ? gameinfo::ATTR_DIM : on ? gameinfo::ATTR_TEXT : gameinfo::ATTR_HEADING);
                if (on) total += as.count[c];
            }
            text(12, cfg.waste_bin ? "They go to the Waste Bin (W in" : "waste_bin is off: they are", gameinfo::ATTR_DIM);
            text(13, cfg.waste_bin ? "Tools gets them back)." : "erased for good.", gameinfo::ATTR_DIM);
            if (as.found_full) text(14, "(more than fit: E again after)", gameinfo::ATTR_DIM);
            if (as.mode == AS_ASK) {
                snprintf(t, sizeof(t), "Erase %d? Y-Yes  other key-No", total);
                text(21, t, gameinfo::ATTR_KEYS);
            } else if (as.mode == AS_DONE) {
                text(15, as.said, gameinfo::ATTR_HEADING);
                keys = "Any key-Back";
            } else {
                text(21, total > 0 ? (AS_CATS == 7 ? "1-7 On/off  E-Erase" : "1-6 On/off  E-Erase") : "Nothing to erase", gameinfo::ATTR_KEYS);
                keys = "X-Back to the list";
            }
            break;
        }
    }
    text(22, keys, gameinfo::ATTR_KEYS);
}

// ACTION_ASSETS (E in Tools): BYTE param1 = the key (0: opened). Status 0 the screen stays, 1 closed
__attribute__((noinline)) static void nmi_action_assets() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t key = nmi_rom_data[2];
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer);
    const bool back = as.mode == AS_TYPE ? is_key_break(key) : is_key_back(key);    // (X is typed in the name)
    if (key == 0) {
        memset(&as, 0, sizeof(as));
        info_loaded = false;                // (the game information's buffers are used)
        if (!sd_card_mounted) {
            nmi_rom_data[0] = 1;
            return;
        }
    } else {
        switch (as.mode) {
            case AS_TYPE: {
                size_t len = strlen(as.typed);
                if (back) { nmi_rom_data[0] = 1; return; }
                if (key == notes::KEY_DELETE) { if (len > 0) as.typed[len - 1] = '\0'; }
                else if (key == notes::KEY_ENTER || key == 13) { as_list(); as.mode = AS_LIST; }
                else if (key >= ' ' && key < 127 && len + 1 < sizeof(as.typed)) { as.typed[len] = (char) key; as.typed[len + 1] = '\0'; }
                break;
            }
            case AS_LIST: {
                int pages = (as.names + AS_PAGE - 1) / AS_PAGE;
                if (back || key == 't' || key == 'T') as.mode = AS_TYPE;
                else if ((key == 'n' || key == 'N') && pages > 1) as.page = (as.page + 1) % pages;
                else if ((key == 'p' || key == 'P') && pages > 1) as.page = (as.page + pages - 1) % pages;
                else if (key >= '0' && key <= '9') {
                    int i = as.page * AS_PAGE + (key == '0' ? 9 : key - '1');
                    if (i < as.names) {
                        snprintf(as.game, sizeof(as.game), "%s", as_names() + as_index()[i]);
                        as_find();
                        as.mode = AS_CATS_SCREEN;
                    }
                }
                break;
            }
            case AS_CATS_SCREEN: {
                int total = 0;
                for (int c = 0; c < AS_CATS; c++) if (as.on & (1 << c)) total += as.count[c];
                if (back) { as_list(); as.mode = AS_LIST; }       // (the list again: some may be gone)
                else if (key >= '1' && key <= '0' + AS_CATS && as.count[key - '1'] > 0) as.on ^= (uint8_t) (1 << (key - '1'));
                else if ((key == 'e' || key == 'E') && total > 0) as.mode = AS_ASK;
                break;
            }
            case AS_ASK:
                if (key == 'y' || key == 'Y') {
                    as_erase();
                    as_find();                  // (what is left)
                    as.mode = AS_DONE;
                } else {
                    as.mode = AS_CATS_SCREEN;
                }
                break;
            default:
                as.mode = AS_CATS_SCREEN;
                break;
        }
    }
    as_draw(screen);
    preview_screen = screen;
    nmi_rom_data[START_KEYREPEAT] = as.mode == AS_TYPE ? 1 : 0;     // (typing: a held key repeats)
    nmi_rom_data[0] = 0;
}
#undef as
#endif

// ---- V on the menu: the Virtual Tape Player (the tape position) ----
// The blocks of the tape being played (the tape trap's) by name, where it has got to, and keys
// to move it: 1-0 makes the next load start at that block, R rewinds it, N and P turn the
// pages. A header and the block after it (its data) are one line. Moving the tape is done
// as playing it: from the start, block by block (the blocks themselves are skipped).

struct TapeEntry {
    uint32_t first;         // its first data block (0 is the tape's first)
    uint8_t blocks;         // 1, or 2 for a header and its data
    int8_t type;            // a header's type (0 program, 1 and 2 arrays, 3 bytes), -1 none
    uint8_t flag;
    char name[11];
    uint32_t length;        // the data's length (a header's says it)
};

// Calls visit(entry) for each line of the tape in f (open at its start), until it returns false.
// Uses the global tz for a .tzx, which the caller saves.
template <class F>
static void tape_entries(FileHandle& f, bool tzx, F visit) {
    if (tzx && !tzx_begin(f)) return;
    TapeEntry e;
    bool pending = false;           // a header waiting to see whether data follows
    uint32_t block = 0, blen;
    while (tape_next_block(f, tzx, blen)) {
        uint32_t at = f.tell();
        uint8_t h[18];
        uint32_t n = blen < sizeof(h) ? blen : (uint32_t) sizeof(h);
        if (n < 1 || !tap_read_exact(f, h, n)) break;
        if (pending && h[0] != 0) {             // the header's data
            e.blocks = 2;
            pending = false;
            if (!visit(e)) return;
        } else {
            if (pending && !visit(e)) return;
            pending = false;
            memset(&e, 0, sizeof(e));
            e.first = block;
            e.blocks = 1;
            e.flag = h[0];
            e.type = -1;
            e.length = blen >= 2 ? blen - 2 : 0;
            if (h[0] == 0 && blen == 19) {      // a standard header
                e.type = (int8_t) h[1];
                for (int i = 0; i < 10; i++) e.name[i] = (h[2 + i] >= 32 && h[2 + i] < 127) ? (char) h[2 + i] : '?';
                for (int i = 9; i >= 0 && e.name[i] == ' '; i--) e.name[i] = '\0';
                e.length = h[12] | (h[13] << 8);
                pending = true;
            } else if (!visit(e)) {
                return;
            }
        }
        block++;
        if (!f.seek(at + blen)) return;
    }
    if (pending) visit(e);
}

static uint32_t tape_pos_page = 0;
static char tape_pos_msg[33] = "";

// Plays the tape from its start to just before data block 'block'
static bool tape_goto(uint32_t block) {
#if PICOMENU_BT
    bt_tape_ready = false;          // the Bluetooth's player starts again from tape_done
    bt_playing = false;
#endif
    if (!tape_open_file()) return false;
    const bool tzx = tape_is_tzx(tape_name);
    for (uint32_t i = 0; i < block; i++) {
        tz = tape_tz;
        uint32_t blen;
        bool ok = tape_next_block(tape_file, tzx, blen) && tape_file.seek(tape_file.tell() + blen);
        tape_tz = tz;
        if (!ok) return tape_open_file();
        tape_done++;
        tape_block_ms = to_ms_since_boot(get_absolute_time());
    }
    return true;
}

// The data blocks of a tape, counted as the trap counts them (at most TZX_MAX_BLOCKS)
static uint32_t tape_count_data_blocks(uint8_t drive, const char* path) {
    FileHandle f;
    if (!f.open(drive == 1 ? Drive::EXTERNAL_SD_CARD : Drive::INTERNAL_FLASH, path, true)) return 0;
    const bool tzx = tape_is_tzx(path);
    TzxState saved = tz;
    uint32_t n = 0, blen;
    if (!tzx || tzx_begin(f)) {
        while (n < TZX_MAX_BLOCKS && tape_next_block(f, tzx, blen) && f.seek(f.tell() + blen)) n++;
    }
    f.close();
    tz = saved;
    return n;
}

static void tape_entry_text(const TapeEntry& e, char* out, size_t size) {
    static const char* const kinds[] = { "Program", "Numbers", "Characters", "Bytes" };
    if (e.type >= 0 && e.type <= 3) snprintf(out, size, "%s: %s", kinds[e.type], e.name);
    else if (e.type >= 0) snprintf(out, size, "Header: %s", e.name);
    else if (e.flag == 0xFF) snprintf(out, size, "Headerless data");
    else snprintf(out, size, "Data, flag %02X", e.flag);
}

// W: the name of a new recording is typed here (the keys come one at a time, as for the rest of
// this screen): letters, digits and symbols, DELETE, ENTER to start, BREAK to go back
static bool tape_rec_naming = false;
static char tape_rec_typed[25];
static const char* tape_rec_why = nullptr;      // why the last ENTER did not start it

static void tape_rec_name_draw(uint8_t* screen) {
    const uint8_t BANNER = 0x4F, TEXT = 0x47, DONE = 0x07, HEADING = 0x46, KEYS = 0x45, NOW = 0x44;
    memset(screen, 0, 6144);
    memset(screen + 6144, ui_attr(0x07), 768);
    memset(screen + 6144, BANNER, 32);
    static const char title[] = "Record a new tape";
    help_put_text(screen, 0, 1, title, sizeof(title) - 1, BANNER);
    static const char where[] = "In /Recordings on the SD card:";
    help_put_text(screen, 2, 0, where, sizeof(where) - 1, TEXT);
    char line[40];
    int n = snprintf(line, sizeof(line), "%s_.tap", tape_rec_typed);
    help_put_text(screen, 4, 1, line, n, NOW);
    const char* const lines[] = {
        "Type the tape's name, then", "ENTER. Each SAVE (in BASIC, or", "a program that saves with the",
        "ROM) then goes on the end of", "it at once, until E on this", "screen stops it. While it",
        "records, no tape is played.", "", "A name in use is not taken, so", "no tape is written over.",
    };
    for (int i = 0; i < (int) (sizeof(lines) / sizeof(lines[0])); i++) {
        help_put_text(screen, 7 + i, 0, lines[i], strlen(lines[i]), DONE);
    }
    if (tape_rec_why != nullptr) help_put_text(screen, 21, 0, tape_rec_why, strlen(tape_rec_why), HEADING);
    static const char k[] = "ENTER-Record DELETE BREAK-Back";
    help_put_text(screen, 23, 1, k, sizeof(k) - 1, KEYS);
    (void) TEXT;
}

// L: a label for a line. 0 none, 1 "Label which line? 1-0", 2 typing it (in tape_rec_typed)
static uint8_t tape_label_state = 0;
static uint32_t tape_label_block = 0;           // the line's first data block
static uint8_t tape_label_line = 0;             // its number on the page (1-10)
static const size_t TAPE_LABEL_MAX = 16;

// The tape player's screen for a key (0: opened), drawn in snap_load_buffer: its status, 0
// shown, 1 closed. (It is given back, not put in the ROM's status byte straight away: the
// Spectrum runs on as the Pico draws, and would take a status written early as the answer)
static uint8_t tape_position_screen(uint8_t key) {
    const uint8_t BANNER = 0x4F, TEXT = 0x47, DONE = 0x07, HEADING = 0x46, KEYS = 0x45, NOW = 0x44;
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer);
    bool keep_msg = false;              // drawn again after recording starts: its message stays
    if (key == 0) {
        tape_rec_naming = false;        // the screen is opened
        tape_label_state = 0;
    }
    if (tape_label_state == 2 && key != 0) {
        // typing the label: the keys are its letters until ENTER (save) or BREAK (back)
        size_t len = strlen(tape_rec_typed);
        if (key == 0x18) {
            tape_label_state = 0;
        } else if (key == 0x13) {
            while (len > 0 && tape_rec_typed[len - 1] == ' ') tape_rec_typed[--len] = '\0';
            const char* start = tape_rec_typed;
            while (*start == ' ') start++;
            bool ok = !sna_in_buffer && (tape_labels_read(tape_drive, tape_name), true) &&
                tape_labels_write(tape_drive, tape_name, tape_labels_with(tape_label_block, start));
            snprintf(tape_pos_msg, sizeof(tape_pos_msg), "%s", !ok ? "The label could not be saved" :
                *start != '\0' ? "Label saved" : "Label removed");
            tape_label_state = 0;
        } else if (key == 0x0A) {
            if (len > 0) tape_rec_typed[len - 1] = '\0';
        } else if (key >= 32 && key < 127 && len < TAPE_LABEL_MAX) {
            tape_rec_typed[len] = (char) key;
            tape_rec_typed[len + 1] = '\0';
        }
        key = 0x7F;                     // drawn again, with nothing else done
    }
    if (tape_rec_naming) {
        size_t len = strlen(tape_rec_typed);
        if (key == 0x18) {                              // BREAK: back to the tape position
            tape_rec_naming = false;
            key = 0;
        } else if (key == 0x13) {                       // ENTER: start
            tape_rec_why = tape_record_start(tape_rec_typed);
            if (tape_rec_why == nullptr) {
                tape_rec_naming = false;
                tape_pos_page = 0;
                snprintf(tape_pos_msg, sizeof(tape_pos_msg), "Recording: SAVE goes on the end");
                keep_msg = true;
                key = 0;
            }
        } else if (key == 0x0A) {                       // DELETE
            if (len > 0) tape_rec_typed[len - 1] = '\0';
            tape_rec_why = nullptr;
        } else if (key >= 32 && key < 127 && key != '~' && len + 1 < sizeof(tape_rec_typed)) {
            tape_rec_typed[len] = (char) key;
            tape_rec_typed[len + 1] = '\0';
            tape_rec_why = nullptr;
        }
        if (tape_rec_naming) {
            tape_rec_name_draw(screen);
            preview_screen = screen;
            return 0;
        }
    }
    if (tape_label_state == 1 && key != 0) {
        // the line to label: its number on this page, anything else goes back
        tape_label_state = key >= '0' && key <= '9' ? 3 : 0;      // 3: found below, once the lines are read
        if (tape_label_state == 3) tape_label_line = (uint8_t) (key == '0' ? 10 : key - '0');
        key = 0x7F;
    }
    if (key == 'w' && !tape_rec) {                      // W: record a new tape
        tape_rec_naming = true;
        tape_rec_typed[0] = '\0';
        tape_rec_why = nullptr;
        tape_rec_name_draw(screen);
        preview_screen = screen;
        return 0;
    }
    bool playing = tape_service_active && !tape_ejected;
    if (key == 'l' && playing && !tape_rec) {          // L: label a line
        tape_label_state = 1;
        key = 0x7F;
    }
    if (key == 'x' || key == 0x18 || key == 0x13 || (key != 0 && key != 'e' && key != 0x7F && !playing)) {
        return 1;            // closed
    }
    if (key == 'e' && playing && tape_rec) {    // stop recording
        const char* name = strrchr(tape_name, '/');
        snprintf(tape_pos_msg, sizeof(tape_pos_msg), "Saved %.24s", name != nullptr ? name + 1 : tape_name);
        tape_eject();
        playing = false;
    } else if (key == 'e' && playing) {        // eject: the file is closed, the trap stays
        tape_eject();
        playing = false;
    } else if (key == 'e') {
        return 1;
    }
    memset(screen, 0, 6144);
    memset(screen + 6144, ui_attr(0x07), 768);
    memset(screen + 6144, BANNER, 32);
    const char* title = playing && tape_rec ? "Recording" : "Virtual Tape Player";
    help_put_text(screen, 0, 1, title, strlen(title), BANNER);
    preview_screen = screen;
    if (!playing) {
        const char* const lines[] = {
            tape_service_active ? "The tape is out (ejected)." : "No tape is being played.",
            tape_service_active ? "Its file can be erased or moved." : "",
            "",
            "Load a .tap or .tzx with its", "number in B - Browse Games, or",
            "put one in with T there for the", "Spectrum's own LOAD and the 128",
            "Tape Loader. This screen then", "shows its blocks and where the", "tape has got to, and moves it",
            "on or back for a game that", "loads more as it is played.", "",
            "W records a new tape in", "/Recordings: each SAVE goes", "into it at once.",
        };
        for (int i = 0; i < (int) (sizeof(lines) / sizeof(lines[0])); i++) {
            help_put_text(screen, 2 + i, 0, lines[i], strlen(lines[i]), i == 0 ? HEADING : TEXT);
        }
        if (tape_pos_msg[0] != '\0') help_put_text(screen, 21, 0, tape_pos_msg, strlen(tape_pos_msg), HEADING);
        static const char k[] = "W-Record  Any other key: back";
        help_put_text(screen, 23, 1, k, sizeof(k) - 1, KEYS);
        return 0;
    }
    const bool tzx = tape_is_tzx(tape_name);
    TzxState saved = tz;
    FileHandle f;
    bool open = f.open(tape_drive == 1 ? Drive::EXTERNAL_SD_CARD : Drive::INTERNAL_FLASH, tape_name, true);
    // the lines, and the one the tape is at (the next load's)
    uint32_t count = 0, now = 0;
    bool at_end = true;
#if PICOMENU_BT
    // the Bluetooth's place while it has the tape (P in B - Bluetooth tape): the blocks it has
    // played are dim as it goes
    const uint32_t pos = bt_tape_ready ? bt_position() : tape_done;
#else
    const uint32_t pos = tape_done;
#endif
    if (open) {
        tape_entries(f, tzx, [&](const TapeEntry& e) {
            if (at_end && pos < e.first + e.blocks) {
                now = count;
                at_end = false;
            }
            count++;
            return true;
        });
    }
    const uint32_t PER_PAGE = 10;
    uint32_t pages = count == 0 ? 1 : (count + PER_PAGE - 1) / PER_PAGE;
    if (tape_label_state == 3) {
        // the line chosen for L: its first data block, and its label now (to change)
        uint32_t want = tape_pos_page * PER_PAGE + tape_label_line - 1, i = 0;
        bool found = false;
        if (want < count && open && f.seek(0)) {
            tape_entries(f, tzx, [&](const TapeEntry& e) {
                if (i++ == want) {
                    tape_label_block = e.first;
                    found = true;
                    return false;
                }
                return true;
            });
        }
        tape_label_state = 0;
        if (found && !sna_in_buffer) {
            tape_labels_read(tape_drive, tape_name);
            if (!tape_label_of(tape_label_block, tape_rec_typed, TAPE_LABEL_MAX + 1)) tape_rec_typed[0] = '\0';
            tape_label_state = 2;
        }
    }
    if (key == 0) {
        tape_pos_page = tape_rec ? (pages - 1) : at_end ? 0 : now / PER_PAGE;     // recording: the newest blocks
        if (!keep_msg) tape_pos_msg[0] = '\0';
    } else if (key == 0x7F) {
        // (a key for L: nothing else to do)
    } else if ((key == 'n' || key == ' ' || key == 8) && tape_pos_page + 1 < pages) {
        tape_pos_page++;
    } else if ((key == 'p' || key == 5) && tape_pos_page > 0) {
        tape_pos_page--;
    } else if (key == 'r' && !tape_rec) {
        tape_goto(0);
        snprintf(tape_pos_msg, sizeof(tape_pos_msg), "Rewound to the start");
        tape_pos_page = 0;
    } else if (key >= '0' && key <= '9' && open && !tape_rec) {
        uint32_t want = tape_pos_page * PER_PAGE + (key == '0' ? 9 : key - '1');
        if (want < count) {
            uint32_t i = 0, block = 0;
            if (f.seek(0)) {
                tape_entries(f, tzx, [&](const TapeEntry& e) {
                    if (i++ == want) {
                        block = e.first;
                        return false;
                    }
                    return true;
                });
            }
            tape_goto(block);
            snprintf(tape_pos_msg, sizeof(tape_pos_msg), "The next load starts at %u", (unsigned) (want + 1));
        }
    }
    // after a move: where it is now
    if (!tape_rec && (key == 'r' || (key >= '0' && key <= '9'))) {
        now = 0;
        at_end = true;
        uint32_t i = 0;
        if (open && f.seek(0)) {
            tape_entries(f, tzx, [&](const TapeEntry& e) {
                if (tape_done < e.first + e.blocks) {
                    now = i;
                    at_end = false;
                    return false;
                }
                i++;
                return true;
            });
        }
    }
    // the tape's name and the page
    const char* name = strrchr(tape_name, '/');
    name = name != nullptr ? name + 1 : tape_name;
    help_put_text(screen, 1, 0, name, strlen(name), HEADING);
    char line[40];
    int n = snprintf(line, sizeof(line), "%u/%u", (unsigned) (tape_pos_page + 1), (unsigned) pages);
    help_put_text(screen, 0, 31 - n, line, n, BANNER);
    // this page's lines: the ones the tape has gone past are dim, the next load's is marked; a
    // line with a label shows it (in yellow) in place of what kind of block it is
    uint32_t first = tape_pos_page * PER_PAGE, i = 0;
    const bool labels = !tape_rec && !sna_in_buffer && tape_labels_read(tape_drive, tape_name) > 0;
    if (open && f.seek(0)) {
        tape_entries(f, tzx, [&](const TapeEntry& e) {
            if (i >= first + PER_PAGE) return false;
            if (i >= first) {
                int row = 3 + (int) (i - first);
                bool here = !tape_rec && !at_end && i == now;
                bool done = !tape_rec && (at_end || i < now);
                char text[32];
                bool labelled = labels && tape_label_of(e.first, text, TAPE_LABEL_MAX + 1);
                if (!labelled) tape_entry_text(e, text, sizeof(text));
                line[0] = (char) ('0' + (i - first + 1) % 10);
                line[1] = here ? '>' : ' ';
                help_put_text(screen, row, 0, line, 2, here ? NOW : KEYS);
                help_put_text(screen, row, 3, text, strlen(text) < 22 ? strlen(text) : 22,
                    here ? NOW : done ? DONE : labelled ? HEADING : TEXT);
                char len[12];
                int ln = snprintf(len, sizeof(len), "%u", (unsigned) e.length);
                help_put_text(screen, row, 32 - ln, len, ln, here ? NOW : done ? DONE : TEXT);
            }
            i++;
            return true;
        });
    }
    if (open) f.close();
    tz = saved;
    if (count == 0) {
        const char* none = tape_rec ? "Nothing saved on it yet" : "No blocks on this tape";
        help_put_text(screen, 3, 0, none, strlen(none), TEXT);
    }
    if (tape_rec) {
        static const char* const rec[] = {
            "Each SAVE (in BASIC, or a", "program that saves with the", "ROM) goes on the end of this",
            "tape at once. E stops: the tape", "stays in /Recordings, to load", "as any other tape.",
        };
        for (int k = 0; k < (int) (sizeof(rec) / sizeof(rec[0])); k++) {
            help_put_text(screen, 14 + k, 0, rec[k], strlen(rec[k]), DONE);
        }
        if (tape_rec_failed) {
            static const char bad[] = "A block could not be written";
            help_put_text(screen, 21, 0, bad, sizeof(bad) - 1, HEADING);
        } else if (tape_pos_msg[0] != '\0') {
            help_put_text(screen, 22, 0, tape_pos_msg, strlen(tape_pos_msg), HEADING);
        }
        static const char kr[] = "E-Stop recording  N P X-Back";
        help_put_text(screen, 23, 1, kr, sizeof(kr) - 1, KEYS);
        return 0;
    }
    const char* where = at_end ? "At the end: it starts again" : "> the next load";
    help_put_text(screen, 14, 0, where, strlen(where), NOW);
#if PICOMENU_BT
    if (bt_tape_ready || bt_owns) {
        const char* in = bt_tape_ready ? "Playing by Bluetooth (T-B)" : "For Bluetooth: LOAD plays it";
        help_put_text(screen, 15, 0, in, strlen(in), DONE);
    } else
#endif
    if (tape_in) {
        static const char in[] = "In with T: for the Spectrum ROM";
        help_put_text(screen, 15, 0, in, sizeof(in) - 1, DONE);
    }
    if (tape_label_state == 1) {
        static const char q[] = "Label which line? 1-0";
        help_put_text(screen, 17, 0, q, sizeof(q) - 1, HEADING);
        static const char kq[] = "1-0 the line  Other keys: back";
        help_put_text(screen, 23, 1, kq, sizeof(kq) - 1, KEYS);
        return 0;
    }
    if (tape_label_state == 2) {
        char q[40];
        int qn = snprintf(q, sizeof(q), "Label for line %u (16 letters):", (unsigned) tape_label_line);
        help_put_text(screen, 17, 0, q, qn, HEADING);
        qn = snprintf(q, sizeof(q), "%s_", tape_rec_typed);
        help_put_text(screen, 18, 1, q, qn, NOW);
        static const char e1[] = "Empty: the label goes.";
        help_put_text(screen, 20, 0, e1, sizeof(e1) - 1, DONE);
        static const char kt[] = "ENTER-Save DELETE BREAK-Back";
        help_put_text(screen, 23, 1, kt, sizeof(kt) - 1, KEYS);
        return 0;
    }
    static const char* const help[] = {
        "1-0 the next load starts there", "R   rewind to the start",
        "L   label a line (yellow)", "E   eject (to erase or move it)",
        "W   record a new tape instead", "Dim: loaded already.",
    };
    for (int k = 0; k < (int) (sizeof(help) / sizeof(help[0])); k++) {
        help_put_text(screen, 16 + k, 0, help[k], strlen(help[k]), DONE);
    }
    if (tape_pos_msg[0] != '\0') help_put_text(screen, 22, 0, tape_pos_msg, strlen(tape_pos_msg), HEADING);
    static const char k1[] = "1-0 L-Label E-Eject N P X-Back";
    help_put_text(screen, 23, 1, k1, sizeof(k1) - 1, KEYS);
    return 0;
}

static void nmi_action_tape_position() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    nmi_rom_data[0] = tape_position_screen(nmi_rom_data[2]);
}

// ---- B - Bluetooth tape (Tools; the Pico W build) ----
// The tape put in (T in Browse Games, or one the trap is playing) is played as sound to a
// Bluetooth receiver wired to the Spectrum's EAR socket (or a speaker held to it), in real time,
// as a phone's tape app plays it: the Spectrum's own loader loads it, so a turbo or protected
// loader works as with a real tape. The sound is made from the tape file (tapeaudio.h) as
// Bluetooth needs it (bt.cxx).
//
// The tape is the Bluetooth's (bt_owns) from P, or from leaving this screen connected, or from T
// in Browse Games ([Bluetooth] tapes = bluetooth). Then the trap's hook answers 4 to the ROM's
// LD-BYTES (TAPE_IN_HOOK): the ROM loads from the EAR socket, and the load starts the tape
// (auto_play), so LOAD "" (or a multi-load game's next load) plays it as a deck would be started.
// Opening the menu while a block plays loses that block (the Spectrum stops listening while the
// receiver plays on a moment): the tape goes back to the start of its line, for the next load.
// A few seconds after the tape ends it switches off (auto_off), so autosave can go on.
//
// Memory: the Bluetooth's variables are in the last 24 KB of snap_load_buffer (the Pico W
// build's memmap_picomenu_w.ld). It stays on only while nothing else can use that: its own
// screen, the menu being opened and closed (the screen's stash and restore), and the game
// running. Anything else the menu does (bt_keeps), autosaving and the level loader scan wait or
// switch it off first (bt_off). The tape's place is kept in ordinary RAM (bt_tape), so P goes on
// from where it was after the Bluetooth was off.
#if PICOMENU_BT
#ifdef WIFI_ENABLE
// (in the scratch RAM bank, where core 1's stack is, as the main RAM is all used)
static tapeaudio::Audio<FileHandle> bt_tape __attribute__((section(".scratch_x.bt_tape")));
#else
static tapeaudio::Audio<FileHandle> bt_tape;
#endif
static uint32_t bt_lead = 0;                // samples of silence before the tape (P)
static uint8_t bt_volume = 0;               // 0 1/2 (the one that loads best), 1 3/4, 2 full
static const char* bt_msg = nullptr;
static bool bt_auto = false;                // connect to the one paired last, once it is ready
static uint8_t bt_shown = 0xFF;             // what the screen showed (state, found, playing)
static uint32_t bt_shown_block = 0xFFFFFFFF;
static bool bt_vtp = false;                 // T: the tape player, opened from this screen (its X comes back)
static bool bt_want_play = false;           // a load asked for the tape (auto_play): played once connected
static bool bt_want_connect = false;        // connect to the receiver paired last once the chip is ready
static bool bt_menu_seen = false;           // an NMI (the menu) came while it played: seen
static uint32_t bt_end_ms = 0;              // when the tape was found to have ended (auto_off)
static const char* bt_note = nullptr;       // what happened while the screen was closed: shown when it opens
static const uint32_t BT_LEAD_LOAD = 4;     // the silence before the tape when a load starts it: 1/4 s
static const uint64_t BT_CUT_WITHIN = 3500000 / 2;  // data played this recently (T-states) is lost with the menu


// The sound (bt.cxx asks for it as it is due)
static void bt_source(int16_t* out, uint32_t n, void*) {
    if (!bt_playing || rom_state.nmi_active || !tape_file_open) {
        memset(out, 0, n * sizeof(int16_t));     // the menu is open (the Spectrum waits): silence
        return;
    }
    uint32_t i = 0;
    for (; i < n && bt_lead > 0; i++, bt_lead--) out[i] = 0;
    if (i < n) {
        static const int16_t AMP[] = { 15000, 22500, 30000 };
        bt_tape.fill(out + i, n - i, AMP[bt_volume]);
    }
#ifdef PICOFACE_HOST
    static uint32_t shown = 0xFFFFFFFF;
    if (bt_tape.data_position() != shown) {
        shown = bt_tape.data_position();
        LOG("Bluetooth tape: block %u (%u ms played)\n", (unsigned) shown, (unsigned) bt_tape.played_ms());
    }
#endif
    if (bt_tape.state() != tapeaudio::Audio<FileHandle>::PLAYING) {
        bt_playing = false;                     // ended, or a stop
        LOG("Bluetooth tape: %s at block %u\n", bt_tape.state() == tapeaudio::Audio<FileHandle>::HELD ? "stopped by the tape" :
            bt_tape.state() == tapeaudio::Audio<FileHandle>::ENDED ? "ended" : "could not be read", (unsigned) bt_tape.data_position());
    }
}

// The next data block the Bluetooth plays (bt_tape_ready), counted as tape_done is
static uint32_t bt_position() { return bt_tape.data_position(); }

// The trap has the tape again (it was the Bluetooth's): from where the Bluetooth got to
static void bt_trap_back() {
    bt_owns = false;
    bt_want_play = false;
    if (!bt_tape_ready) return;             // the Bluetooth has played none of it
    uint32_t at = bt_tape.data_position();
    bt_playing = false;
    if (tape_in && tape_service_active && !tape_ejected && !tape_rec && tape_goto(at)) {
        tape_done = at;
        tape_in_watch(true);
    }
    bt_tape_ready = false;
}

// The watchdog, on while the Bluetooth is (bt_poll feeds it from the main loop): if the Pico
// hangs, it starts again (the crash recorder, bt.h, says so on the B screen) rather than
// leaving the menu dead
static void bt_watchdog(bool on) {
#ifdef WIFI_ENABLE
    if (on) watchdog_enable(8000, true);
    else hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
#else
    (void) on;
#endif
}

static void bt_off() {
    if (!bt_on) return;
    bt::stop();
    bt_on = false;
    bt_watchdog(false);
    bt_want_connect = false;
    bt_end_ms = 0;
    bt_trap_back();
}

// The radio on (the menu is up): for this screen, or T in Browse Games. Null, or why not
static const char* bt_radio_start() {
    web_off();                              // one radio at a time
    walk_stop();
    sna_in_buffer = false;                  // (a save not finished: its snapshot is let go)
    const char* why = bt::start(reinterpret_cast<uint8_t*>(snap_load_buffer) + WEB_ARENA_AT,
        RADIO_ARENA_END - WEB_ARENA_AT, bt_source, nullptr);
    bt_on = why == nullptr;
    bt_volume = cfg.bt_volume;
    if (why != nullptr) LOG("Bluetooth: %s\n", why);
    bt_watchdog(bt_on);
    return why;
}

// Whether the menu doing this leaves the Bluetooth on: the screen's stash and restore as the
// menu opens and closes, and its own screen (and the ROM reading it: its first 6912 bytes)
static bool bt_keeps(uint8_t action) {
    // (and V - the tape player, whose screen is there too, its labels further on: before the
    // Bluetooth's part)
    // (a high score table put back, and a hotkey asked about, use only the NMI ROM's memory: the
    // Bluetooth plays on after them)
    return action == ACTION_SCREEN_STASH || action == ACTION_SCREEN_RESTORE ||
        (action == ACTION_WIFI && (bt_screen || rom_data[16384 + 2] == 0xFE)) ||
        action == ACTION_TAPE_POSITION || action == ACTION_HS_CHECK || action == ACTION_HOTKEY ||
        (action == ACTION_PREVIEW_READ && (bt_screen || preview_screen == reinterpret_cast<uint8_t*>(snap_load_buffer)));
}

// Plays the tape from where it is (from where the trap's tape is, the first time), after
// lead/rate() of silence (a receiver can take a moment to start sounding). Null, or why not
static const char* bt_start(uint32_t lead) {
    if (!bt_tape_ready) {
        // from where the trap's tape is (the next data block it would give)
        uint32_t at = tape_done;
        if (!bt_tape.begin(&tape_file, tape_is_tzx(tape_name), bt::rate())) return "The tape cannot be read";
        if (at > 0 && !bt_tape.skip_to(at)) return "The tape has ended";
        bt_tape_ready = true;
    } else if (bt_tape.state() == tapeaudio::Audio<FileHandle>::ENDED) {
        return "The tape has ended (R rewinds)";
    }
    bt_tape.play();                         // (after a "stop the tape")
    bt::note(0x42);
    bt_lead = bt::rate() / lead;
    bt_owns = true;                         // the ROM loads from the EAR socket (the hook's 4)
    bt_playing = true;
#if PICOMENU_HISCORE
    if (!tape_rec) hs_tape_game(tape_name);          // (the tape's game, if it is not the one already)
#endif
    LOG("Bluetooth tape: playing from block %u\n", (unsigned) bt_tape.data_position());
    return nullptr;
}

// A tape put in that the Bluetooth can have (not recording, not ejected)
static bool bt_tape_usable() {
    return tape_service_active && !tape_ejected && tape_file_open && tape_in && !tape_rec;
}

// P: plays now (the menu goes)
static const char* bt_play() {
    if (!tape_service_active || tape_ejected || !tape_file_open) return "Put a tape in first (T on it)";
    if (tape_rec) return "Not while recording";
    if (!tape_in) return "Put it in with T in Browse Games";
    if (bt::state() != bt::CONNECTED) return "Connect to the receiver first";
    return bt_start(1);                     // a second of silence first
}

// The trap's hook asks for a block (the ROM's LD-BYTES): true if the Bluetooth has the tape, so
// the ROM is to load it from the EAR socket; the tape starts (auto_play) as soon as it can
static bool bt_takes_load() {
    if (!bt_owns || !bt_on || !bt_tape_usable()) return false;
    bt::State st = bt::state();
    if (st == bt::READY && bt::connect_saved()) st = bt::state();     // (it went: connected to again)
    if (st != bt::CONNECTED && st != bt::CONNECTING && st != bt::STARTING) return false;
    if (cfg.bt_auto_play && !bt_playing) bt_want_play = true;
    bt::note(0x41);
    return true;
}

// The menu came while a block was being played (bt_tape.cut): the Spectrum's loader has lost it.
// The tape goes back to the start of that block's line in the tape player (its header with it),
// and waits for the next load.
static void bt_cut_back() {
    uint32_t block = bt_tape.cut_block();
    uint32_t first = block, line = 0, i = 0;
    const bool tzx = tape_is_tzx(tape_name);
    TzxState saved = tz;
    FileHandle f;
    if (f.open(tape_drive == 1 ? Drive::EXTERNAL_SD_CARD : Drive::INTERNAL_FLASH, tape_name, true)) {
        tape_entries(f, tzx, [&](const TapeEntry& e) {
            if (block < e.first + e.blocks) {
                first = e.first;
                line = i;
                return false;
            }
            i++;
            return true;
        });
        f.close();
    }
    tz = saved;
    bt::note(0x43);
    tape_goto(first);                       // (the Bluetooth's player starts again from there)
    static char note[40];
    snprintf(note, sizeof(note), "Menu mid-block: back to line %u", (unsigned) (line + 1));
    bt_note = note;
    LOG("Bluetooth tape: the menu cut block %u: back to %u (line %u)\n", (unsigned) block, (unsigned) first, (unsigned) (line + 1));
}

// V: the volume, kept in Picomenu.ini ([Bluetooth] volume) as O - Options keeps it
static bool bt_save_volume() {
    if (!opt_open()) return false;
    opt_after()->s.bt_volume = bt_volume;
    bool ok = opt_save();
    list_pool_busy = false;
    return ok;
}

// T in Browse Games, then B (Bluetooth): the tape just put in is the Bluetooth's, and the
// receiver paired last is connected to (the menu goes; LOAD "" plays it)
static void bt_browse_takes() {
    if (!bt_on && bt_radio_start() != nullptr) {
        bt_note = "Bluetooth did not start";
        return;                             // the trap has it
    }
    bt_owns = true;
    bt::State st = bt::state();
    if (st != bt::CONNECTED && st != bt::CONNECTING) bt_want_connect = true;
}

// What the screen shows, kept in snap_load_buffer after the screen
static void bt_draw(uint8_t* screen) {
    slots_banner(screen, "", "Bluetooth tape");
    char t[48];
    auto text = [&](int r, const char* s, uint8_t attr) { help_put_text(screen, r, 1, s, (int) strlen(s), attr); };
    bt::State st = bt::state();
    int row = 2;
    switch (st) {
    case bt::OFF: text(row, "Off", gameinfo::ATTR_TEXT); break;
    case bt::STARTING: text(row, "Starting ...", gameinfo::ATTR_TEXT); break;
    case bt::FAILED:
        text(row, "Bluetooth did not start:", gameinfo::ATTR_TEXT);
        text(row + 1, bt::why(), gameinfo::ATTR_HEADING);
        break;
    case bt::READY:
        if (bt::saved_name()[0] != '\0') {
            snprintf(t, sizeof(t), "Paired: %.22s", bt::saved_name());
            text(row, t, gameinfo::ATTR_TEXT);
        } else {
            text(row, "Not paired with a receiver", gameinfo::ATTR_TEXT);
        }
        break;
    case bt::SEARCHING: text(row, "Searching (10 seconds) ...", gameinfo::ATTR_TEXT); break;
    case bt::CONNECTING:
        snprintf(t, sizeof(t), "Connecting to %.16s", bt::connected_name());
        text(row, t, gameinfo::ATTR_TEXT);
        break;
    case bt::CONNECTED:
        snprintf(t, sizeof(t), "On %.29s", bt::connected_name());
        text(row, t, gameinfo::ATTR_HEADING);
        snprintf(t, sizeof(t), "(%s, %u Hz)", bt::mono() ? "mono" : "stereo", (unsigned) bt::rate());
        text(row + 1, t, gameinfo::ATTR_DIM);
        break;
    }
    if (st != bt::CONNECTED && st != bt::FAILED && bt::why()[0] != '\0') text(3, bt::why(), gameinfo::ATTR_HEADING);
    // the last restart was a crash or a hang: what and where (the crash recorder, bt.h)
    if (bt::crashed()) {
        bt::crash_text(0, t, sizeof(t));
        text(4, t, gameinfo::ATTR_HEADING);
        bt::crash_text(1, t, sizeof(t));
        text(13, t, gameinfo::ATTR_DIM);
    }
    // what was found
    int n = bt::found_count();
    if ((st == bt::SEARCHING || st == bt::READY) && n > 0) {
        for (int i = 0; i < n && i < bt::FOUND_MAX; i++) {
            const bt::Found& f = bt::found(i);
            snprintf(t, sizeof(t), "%d %.27s", i + 1, f.name[0] != '\0' ? f.name : "(no name yet)");
            text(5 + i, t, gameinfo::ATTR_TEXT);
        }
    } else if (st == bt::READY && n == 0 && bt::saved_name()[0] == '\0') {
        text(5, "Switch the receiver on (ready", gameinfo::ATTR_DIM);
        text(6, "to pair: its light flashing),", gameinfo::ATTR_DIM);
        text(7, "then S to search for it.", gameinfo::ATTR_DIM);
    }
    // the tape
    if (tape_service_active && !tape_ejected && tape_file_open) {
        const char* name = strrchr(tape_name, '/');
        snprintf(t, sizeof(t), "Tape: %.24s", name != nullptr ? name + 1 : tape_name);
        text(14, t, gameinfo::ATTR_TEXT);
        uint32_t block = bt_tape_ready ? bt_tape.data_position() : tape_done;
        if (bt_tape_ready && bt_tape.state() == tapeaudio::Audio<FileHandle>::ENDED) {
            snprintf(t, sizeof(t), "At the end of the tape");
        } else {
            const char* how = !bt_tape_ready ? "ready" :
                bt_playing ? "playing" :
                bt_tape.state() == tapeaudio::Audio<FileHandle>::HELD ? "stopped by the tape" : "paused";
            snprintf(t, sizeof(t), "Block %u, %s", (unsigned) (block + 1), how);
        }
        text(15, t, gameinfo::ATTR_DIM);
        // how far it has got, as C - Convert shows it: the time played, a bar of the file read
        // (green), the percentage
        int size = tape_file.size();
        uint32_t at = (uint32_t) tape_file.tell();
        bool ended = bt_tape_ready && bt_tape.state() == tapeaudio::Audio<FileHandle>::ENDED;
        uint32_t pct = ended ? 100 : size > 0 ? (uint32_t) ((uint64_t) at * 100 / (uint32_t) size) : 0;
        if (pct > 100) pct = 100;
        if (!ended && pct > 99) pct = 99;
        if (!bt_tape_ready && tape_done == 0) pct = 0;      // (the start: the file's header read)
        uint32_t secs = bt_tape_ready ? (uint32_t) (bt_tape.played_ms() / 1000) : 0;
        snprintf(t, sizeof(t), "%u:%02u", (unsigned) (secs / 60), (unsigned) (secs % 60));
        text(16, t, gameinfo::ATTR_TEXT);
        const int BAR_AT = 7, CELLS = 20;
        for (int i = 0; i < CELLS; i++) {
            screen[6144 + 16 * 32 + BAR_AT + i] = (uint32_t) i * 100 < pct * (uint32_t) CELLS ? 0x20 : 0x08;   // green or blue paper
        }
        snprintf(t, sizeof(t), "%u%%", (unsigned) pct);
        help_put_text(screen, 16, BAR_AT + CELLS + 1, t, (int) strlen(t), gameinfo::ATTR_TEXT);
    } else {
        text(14, "No tape in: T on one in Browse", gameinfo::ATTR_DIM);
        text(15, "Games puts it in.", gameinfo::ATTR_DIM);
    }
    static const char* const VOL[] = { "1/2", "3/4", "full" };
    if (st == bt::CONNECTED) {
        if (cfg.bt_auto_play) {
            text(17, "LOAD \"\" or the Tape Loader", gameinfo::ATTR_DIM);
            text(18, "plays it (or P to play now).", gameinfo::ATTR_DIM);
        } else {
            text(17, "P plays (the menu goes), then", gameinfo::ATTR_DIM);
            text(18, "LOAD \"\" or the Tape Loader.", gameinfo::ATTR_DIM);
        }
        snprintf(t, sizeof(t), "V-Volume: %s", VOL[bt_volume]);
        text(19, t, gameinfo::ATTR_DIM);
    }
    if (bt_msg != nullptr) text(20, bt_msg, gameinfo::ATTR_HEADING);
    if (st == bt::CONNECTED) {
        text(21, "P-Play T-Tape player R-Rewind", gameinfo::ATTR_KEYS);
    } else if (st == bt::READY) {
        text(21, bt::saved_name()[0] != '\0' ? (n > 0 ? "S-Search 1-8 C-Connect F-Forget" : "S-Search C-Connect F-Forget") :
            (n > 0 ? "S-Search again 1-8 Pair" : "S-Search"), gameinfo::ATTR_KEYS);
    }
    text(22, st == bt::CONNECTED ? "D-Disconnect O-Off X-Back" : "T-Tape player O-Off X-Back", gameinfo::ATTR_KEYS);
    preview_screen = screen;
}

// What the screen showed, packed: has anything changed?
static uint8_t bt_summary() {
    return (uint8_t) ((uint8_t) bt::state() | (bt::found_count() << 3) | (bt_playing ? 0x80 : 0));
}

// ACTION_WIFI opened with 0xFE (B in Tools), then its keys and polls (255: none): status 0 as it
// is, 1 closed, 2 shown again, 3 leave the menu (P)
static void nmi_action_bt(uint8_t key) {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer);
    bool redraw = false;
    if (bt_vtp && key != 0xFE) {
        // T: the Virtual Tape Player, its keys as V on the menu has them (this screen's polls
        // leave it as it is); when it closes (X, BREAK) this screen is back
        if (key == 255) {
            nmi_rom_data[0] = 0;
            return;
        }
        if (tape_position_screen(key) == 0) {
            nmi_rom_data[0] = 2;            // its screen, shown again
            return;
        }
        bt_vtp = false;
        key = 255;                          // this screen again, as it is now
        redraw = true;
    }
    if (key == 0xFE) {
        bt_screen = true;
        bt_vtp = false;
        bt_msg = bt_note;                   // (what happened while it was closed)
        bt_note = nullptr;
        if (!bt_on) {
            bt_auto = bt_radio_start() == nullptr && cfg.bt_auto_connect;
        } else {
            web_off();
            walk_stop();
            sna_in_buffer = false;
        }
        redraw = true;
    } else if (key != 255) {
        bt_msg = nullptr;
        redraw = true;
        bt::State st = bt::state();
        if (is_key_break(key) || key == 'x' || key == 'X') {
            bt_screen = false;
            bt::crash_clear();                  // (seen)
            // connected (or connecting) with a tape in: the tape is the Bluetooth's, for the next
            // load; else the trap has it again
            if (bt_tape_usable() && (st == bt::CONNECTED || st == bt::CONNECTING)) bt_owns = true;
            else if (!bt_playing) bt_trap_back();
            nmi_rom_data[0] = 1;
            return;
        } else if (key == 'o' || key == 'O') {
            bt_off();
            bt_msg = "Bluetooth off";
        } else if ((key == 's' || key == 'S') && st == bt::READY) {
            bt_auto = false;
            bt::search();
        } else if ((key == 'c' || key == 'C') && (st == bt::READY || st == bt::SEARCHING)) {
            bt_auto = false;
            if (!bt::connect_saved()) bt_msg = "Not paired yet: S to search";
        } else if (key >= '1' && key <= '8' && (st == bt::READY || st == bt::SEARCHING) && key - '1' < bt::found_count()) {
            bt_auto = false;
            bt::connect(key - '1');
        } else if ((key == 'f' || key == 'F') && st == bt::READY && bt::saved_name()[0] != '\0') {
            bt::forget();
            bt_msg = "Forgotten (paired again with S)";
        } else if ((key == 'd' || key == 'D') && st == bt::CONNECTED) {
            bt_playing = false;
            bt_owns = false;
            bt::disconnect();
        } else if (key == 't' || key == 'T') {
            bt_vtp = true;
            tape_position_screen(0);        // the tape player opened
            nmi_rom_data[0] = 2;
            return;
        } else if ((key == 'v' || key == 'V') && st == bt::CONNECTED) {
            bt_volume = (uint8_t) ((bt_volume + 1) % 3);
            if (!bt_save_volume()) bt_msg = "Volume not saved in Picomenu.ini";
        } else if ((key == 'r' || key == 'R') && tape_service_active && !tape_ejected) {
            bt_playing = false;
            tape_goto(0);                               // (the trap's tape too)
            bt_msg = "Rewound to the start";
        } else if (key == 'p' || key == 'P') {
            const char* why = bt_play();
            if (why == nullptr) {
                bt_screen = false;
                nmi_rom_data[0] = 3;                    // the menu goes: the tape plays
                return;
            }
            bt_msg = why;
        }
    }
    // connect to the receiver paired last, as soon as the chip is ready
    if (bt_auto && bt::state() == bt::READY) {
        bt_auto = false;
        bt::connect_saved();
    }
    uint8_t sum = bt_summary();
    uint32_t block = bt_tape_ready ? bt_tape.data_position() : tape_done;
    if (sum != bt_shown || block != bt_shown_block) redraw = true;
    bt_shown = sum;
    bt_shown_block = block;
    if (redraw) bt_draw(screen);
    nmi_rom_data[0] = redraw ? 2 : 0;
}

// From the main loop: the Bluetooth's work
static void bt_poll() {
    if (!bt_on) return;
#ifdef WIFI_ENABLE
    watchdog_update();
#endif
    bt::poll();
    // a connection lost (the receiver off, out of range, or taken by a phone), or not made: the
    // Bluetooth is started afresh before it is used again (as O, then B), as BTstack may not be
    // left tidy; the tape stays the Bluetooth's, and the next load (or C) connects again
    if (bt::lost()) {
        static const char* lost_why;
        lost_why = bt::why()[0] != '\0' ? bt::why() : "The connection was lost";
        bt::note(0x40);
        LOG("Bluetooth: %s: started again\n", lost_why);
        bt_playing = false;
        bt::stop();
        bt_on = false;
        if (bt_radio_start() != nullptr) {
            bt_note = "Bluetooth did not start again";
            bt_trap_back();
            return;
        }
        bt_note = lost_why;
        if (bt_screen) bt_msg = lost_why;
        return;
    }
    bt::State st = bt::state();
    if (bt_want_connect && st == bt::READY) {
        bt_want_connect = false;
        if (!bt::connect_saved()) {
            bt_note = "Pair a receiver first (S)";
            bt_trap_back();                     // (none paired: the trap loads it)
        }
        st = bt::state();
    }
    if (bt_want_play) {
        if (bt_playing || !bt_owns) {
            bt_want_play = false;
        } else if (st == bt::CONNECTED) {
            bt_want_play = false;
            const char* why = bt_start(BT_LEAD_LOAD);
            if (why != nullptr) bt_note = why;
        }
    }
    // the menu (an NMI) while it plays: a block cut short goes back to its start
    if (rom_state.nmi_active) {
        if (bt_playing && !bt_menu_seen) {
            bt_menu_seen = true;
            if (bt_tape.cut(BT_CUT_WITHIN)) bt_cut_back();
        }
    } else {
        bt_menu_seen = false;
    }
    // the tape has ended: off a few seconds on (auto_off), so autosave and the level loader can go on
    if (cfg.bt_auto_off && bt_tape_ready && bt_tape.state() == tapeaudio::Audio<FileHandle>::ENDED &&
            !rom_state.nmi_active) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (bt_end_ms == 0) {
            bt_end_ms = now | 1;
        } else if (now - bt_end_ms >= 5000) {
            bt::note(0x44);
            bt_off();
            bt_note = "Switched off: the tape ended";
            LOG("Bluetooth: off, the tape ended\n");
        }
    } else {
        bt_end_ms = 0;
    }
}

#ifdef PICOFACE_HOST
// the simulator's main loop (the firmware's does it)
void host_bt_poll() { bt_poll(); }
#endif
#endif

uint8_t* beginSendSnapDataToMachine() {
#if PICOMENU_BT
    bt_off();                               // (a snapshot from the PC: all of snap_load_buffer)
#endif
    return reinterpret_cast<uint8_t*>(snap_load_buffer);
}

uint8_t* getRamLoadBuffer() {
#if PICOMENU_BT
    bt_off();
#endif
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
    checkpoints_stop();
    if (!tape_in) {
        tape_service_stop();            // a tape put in with T stays in
    }
    tap_make_reset_snapshot();
    game_running = tape_in;
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

extern "C" int ff_open_objects(void);

// SD card trouble that was put right (Help > Diagnostics): read errors, and times the card was
// mounted again because it had stopped answering or files and folders were left open

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
    return ((rom_state.flags & 0x2) != 0) || rom_state.nmi_active || (tape_service_active && !tape_ejected) ||
        tape_file_open;
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
    cfg_stale = true;               // Picomenu.ini may have been changed: read before it is used
    cfg_folder_due = true;
    enable_logging = loggingWasEnabled;
}

// The card is checked each time the NMI menu opens. Two things have made it look empty until
// the Picoface was switched off and on: the card stops answering after an error (it needs
// initialising again), or FatFs's 16 places for open files and folders (FF_FS_LOCK) are all
// taken by ones that were not closed, so nothing more opens. Mounting the card again puts both
// right (FatFs forgets every open file of the card, and the card is initialised from the
// start). Only files the Picoface keeps open on purpose may be open now: a tape playing from
// the card. Returns true if the card had to be mounted again.
struct TapeLost;
static bool tape_on_card_open();
static void tape_card_lost(TapeLost& t);
static void tape_card_back(const TapeLost& t);
__attribute__((noinline)) static bool sd_check() {
    if (!sd_card_mounted || usb_msc_pc_holds_card()) return false;
    walk_stop();                        // (its folders are closed)
    int allowed = tape_on_card_open() ? 1 : 0;
    int open_now = ff_open_objects();
    // a read of the card itself (the first sector of the volume), not of what FatFs has kept
    sd_card_t* pSD = sd_get_by_num(0);
    DRESULT dr = RES_OK;
    if (open_now <= allowed) {
        dr = disk_read(pSD->fatfs.pdrv, reinterpret_cast<BYTE*>(list_pool), pSD->fatfs.volbase, 1);
    }
    FRESULT fr = dr == RES_OK ? FR_OK : FR_DISK_ERR;
    if (open_now <= allowed && fr == FR_OK) return false;
    LOG("SD card: %d open (%d allowed), root %d: mounting it again\n", open_now, allowed, (int) fr);
    if (open_now > allowed) set_last_error("SD card: files left open", "(mounted again)");
    else set_last_error("SD card stopped answering", "(mounted again)");
    TapeLost lost;
    tape_card_lost(lost);               // a tape playing from it is put in again after
    f_unmount(pSD->pcName);
#ifndef PICOFACE_HOST
    pSD->m_Status |= STA_NOINIT;        // initialised from the start
#endif
    mountSdCard();
    tape_card_back(lost);
    sd_remounts++;
    return true;
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
    settings_load();
    check_trace();
}

bool sendNmiRequest(uint8_t command, uint16_t param1, uint16_t param2) {

    if (((rom_state.flags & 0x2) != 0) || rom_state.nmi_active) {
        return false;
    }
    else {

        uint8_t* nmi_rom_data = rom_data + 16384;
        web_off();                           // (its memory is snap_load_buffer)
        checkpoint_flush();                      // (a checkpoint not yet written: snap_load_buffer is needed)
        // the menu, or the S or G screen on its own (the button): the ROM turns the sound down
        // for them, the card is checked, and the game's time stops for the checkpoints
        const bool menuish = command == 0 || command == STARTUP_ACTION_SAVE_SCREEN || command == STARTUP_ACTION_GRAB_SCREEN;
        if (menuish) sd_check();             // is the card still well?
        nmi_rom_unpack(nmi_rom_data);        // (kept packed in the firmware)
        nmi_menu_open = menuish;
        if (menuish) checkpoint_menu_opens();
        // the NMI rom starts afresh each time, with no search in force, and
        // the card may have been changed since it was last listed
        snap_filter[0] = '\0';
        walk_stop();
        sna_in_buffer = false;
        list_pool_busy = false;
#if PICOMENU_BT
        tape_asking = false;
#endif
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
        // how Browse Games and Save snapshot start, from Picomenu.ini ("last": as they were left)
        settings_fresh();
        bool all = cfg.show_all == settings::SHOW_LAST ? list_all : cfg.show_all == settings::SHOW_ON;
        bool info = cfg.details == settings::SHOW_LAST ? list_details : cfg.details == settings::SHOW_ON;
        nmi_rom_data[START_SHOWALL] = all ? 4 : 0;
        nmi_rom_data[START_SHOWINFO] = info ? 2 : 0;
        nmi_rom_data[START_DRIVE] = cfg.start_drive == settings::DRIVE_LAST ? list_drive :
            cfg.start_drive == settings::DRIVE_FLASH ? 0 : 1;      // (with no card the menu uses the flash)
        nmi_rom_data[START_SAVETYPE] = cfg.save_type;
        nmi_rom_data[START_THEME] = cfg.light ? 1 : 0;
        bin_leave();        // the menu was left from the Waste Bin (a game loaded from it): B starts where it was
        // what the R item on the menu says: the last game, or that there is none to reload. A
        // save slot is named after its game too, from its folder: "Alpha Slot 1 - zone" (and a
        // checkpoint loaded from Browse Games: "Alpha Checkpoint 3")
        {
            char named[extras::LABEL_SIZE];
            const char* label_name = reload_name;
            const char* slot_game = slots::folder_game(reload_dir);
            bool in_folder = slot_game != nullptr && slots::parse(reload_name, nullptr, 0) != 0;
            if (!in_folder) {
                slot_game = cp_folder_game(reload_dir);
                in_folder = slot_game != nullptr && cp_is_name(reload_name);
            }
            if (in_folder) {
                snprintf(named, sizeof(named), "%s %s", slot_game, reload_name);
                label_name = named;
            }
            extras::reload_label(reload_known, reload_drive == 0 || sd_card_mounted, label_name,
                reinterpret_cast<char*>(nmi_rom_data + STARTUP_RELOAD_LABEL));
        }
        if (hotkey_nmi && command == 0) {
            // the button with hotkeys: hotkeys called in place of startmenu (see there)
            lzss_unpack(HOTKEYS_LZ, nmi_rom_data + HOTKEYS_AT, HOTKEYS_SIZE);
            nmi_rom_data[HK_CALLMENU + 1] = (uint8_t) (HOTKEYS_AT & 0xFF);
            nmi_rom_data[HK_CALLMENU + 2] = (uint8_t) (HOTKEYS_AT >> 8);
        }
        hotkey_nmi = false;
#if PICOMENU_HISCORE
        if (command == 0) hs_menu_nmi(nmi_rom_data);       // (after hotkeys: it goes on to them)
#endif
#if PICOMENU_HISCORE
        if (command == STARTUP_ACTION_CHECKPOINT && hs_auto_nmi) {
            // not an autosave: a tape game's high score table put back (hs_auto_poll)
            if (!hs_auto_code(nmi_rom_data)) {
                // (nothing to put in: out again at once)
                uint8_t* c = nmi_rom_data + CP_AT;
                c[0] = 0xC3; c[1] = (uint8_t) (EXIT_NO_SCREEN & 0xFF); c[2] = (uint8_t) (EXIT_NO_SCREEN >> 8);
            }
        } else
#endif
        if (command == STARTUP_ACTION_CHECKPOINT) {
            // the autosave's copy in place of the menu's code (after all the above, which
            // writes some of the menu's bytes)
#if PICOMENU_CP_OLD
            memcpy(nmi_rom_data + CP_AT, CP_OLD_BIN, CP_OLD_SIZE);      // (a test build: Rev 2.5.0's way)
#else
            lzss_unpack(CP_SAVE_LZ, nmi_rom_data + CP_AT, CP_SIZE);
            save_stream_start();
#endif
            cp_nmi_at = time_us_64();
        }
        rom_state.flags |= 2;
        gpio_put(PIN_NMI, false);
#ifdef PICOFACE_HOST
        sleep_ms(3);
#else
        // (Rev 2.6.1) A second NMI while the NMI ROM runs starts it again over itself: the game's
        // registers are lost and it crashes on the way out. On a standard Pico the line held low
        // for 3 ms sometimes gave one (a 128K tape's big blocks, loaded through the NMI ROM, then
        // looped). So the line is held low only until the NMI is taken (the bus handler sets
        // nmi_active at the fetch from 0x66, a few us; at most 3 ms as before), and once the Z80
        // is past 0x66/0x67 they become RETN: a second NMI returns at once. (nmi_rom_unpack puts
        // the NMI ROM back as it was for the next NMI.)
        uint32_t nmi_t0 = time_us_32();
        while (!rom_state.nmi_active && time_us_32() - nmi_t0 < 3000) { }
        if (rom_state.nmi_active) {
            busy_wait_us(20);       // (some 70 T-states: the NOP and the next instruction)
            nmi_rom_data[0x66] = 0xED;
            nmi_rom_data[0x67] = 0x45;
        }
#endif
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
// 128-0.rom and 128-1.rom, in the root of the flash, the SD card's ROMS folder, or its root.

enum ConvPhase : uint8_t { CONV_IDLE, CONV_CHOOSE, CONV_RUN };
enum ConvMode : uint8_t { CONV_AUTO, CONV_48, CONV_128 };
static uint8_t conv_phase = CONV_IDLE;
static bool conv_running() { return conv_phase == CONV_RUN; }
static uint8_t conv_mode = CONV_AUTO;
static bool conv_machine128 = false;        // the ROM found the machine can page memory
static bool conv_as128 = false;             // the attempt running is a 128K one
static bool conv_retried = false;           // Auto: the 128K attempt failed, this is the 48K one
static uint32_t conv_size = 0;              // of the tape file
static uint64_t conv_started = 0;           // time_us_64() at the start
static bool conv_file_open = false;         // conv_scratch().file is the tape, open
// The batch converter (C in Tools, see nmi_batch_info): the tapes of a folder, one after another
static bool batch_on = false;               // a batch is running: the .z80 goes in <folder>/Converted
static uint16_t batch_total = 0, batch_done = 0;    // tapes in the folder, and how many looked at

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
        const uint8_t* glyph = menu_font()[(ch >= 32 && ch < 128 ? ch : '?') - 32];
        for (int line = 0; line < 8; line++) c.pix[r][line][col + i] = glyph[line];
    }
}

// Reads a ROM file of exactly `size` bytes from the root of the flash, else the SD card
static bool conv_read_rom(const char* name, uint8_t* dest, uint32_t size) {
    FileHandle& f = conv_scratch().file;
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", ROMS_FOLDER, name);
    for (int d = 0; d < 3; d++) {           // the flash, the card's ROMS folder, the card
        if (d >= 1 && !sd_card_mounted) break;
        if (!f.open(d == 0 ? Drive::INTERNAL_FLASH : Drive::EXTERNAL_SD_CARD, d == 1 ? path : name, true)) continue;
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
    if (batch_on) {
        snprintf(t, sizeof(t), "%u/%u %s: %s", (unsigned) (batch_done + 1), (unsigned) batch_total,
            conv_as128 ? "128K" : "48K", info_name);
    } else {
        snprintf(t, sizeof(t), "Converting (%s): %s", conv_as128 ? "128K" : conv_retried ? "48K, 2nd go" : "48K", info_name);
    }
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
static int conv_auto_number = -1;               // its number (<name> Loading Screen<n>.scr), -1 none: BREAK removes it
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
    conv_scr_folder = grab_folder_make();
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
    // the loading screen saved by itself, and one saved with S, are "<name> Loading Screen.scr",
    // then "<name> Loading Screen1.scr" ... (the lowest number free)
    const screengrab::Category cat = screengrab::LOADING;
    int number = grab_free_number(base, cat);
    if (number < 0) {
        conv_note("Screen not saved: too many", CONV_ATTR_FAIL);
        return true;
    }
    uint32_t pos = c.file.tell();
    conv_close_tape();
    bool ok = grab_write(c.file, base, cat, number, scr);
    if (!ok) {
    } else if (automatic) conv_auto_number = number;
    else conv_manual_saved = true;
    LOG("Convert: screen %s %s\n", grab_path, ok ? "saved" : "not saved");
    if (!c.file.open(info_drive == 1 ? Drive::EXTERNAL_SD_CARD : Drive::INTERNAL_FLASH, info_path, true)) return false;
    conv_file_open = true;
    if (!c.file.seek(pos)) return false;
    // "Saved: <name> Loading Screen<n>", the end kept if it is long
    char name[screengrab::NAME_MAX + 24], t[40];
    screengrab::grab_name(base, cat, number, name, sizeof(name));
    const char* lead = !ok ? "Screen not saved: " : "Saved: ";
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
            snprintf(conv_msg, sizeof(conv_msg), "No 128.rom (flash or SD /ROMS)");
            return false;
        }
    } else if (!conv_read_rom(TAPE_ROM_NAME, buf + 65536, 16384)) {
        snprintf(conv_msg, sizeof(conv_msg), "No 48.rom (flash or SD /ROMS)");
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
        // (a batch: in the Converted folder of the tapes' folder)
        int n = batch_on ? snprintf(out, sizeof(out), "%.*s/Converted/%s%s.z80", dirlen, info_path, info_name, suffix)
                         : snprintf(out, sizeof(out), "%.*s/%s%s.z80", dirlen, info_path, info_name, suffix);
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
    if (ok) {
        // where the tape had got to, if the conversion stopped with more on it that nothing
        // took in (a multi-load game's next levels): they load from there when the .z80 is
        // loaded (through the tape trap)
        uint32_t at = pconv::tape_position(c.machine);
        bool more = pconv::stopped_early(c.machine);
        conv_close_tape();          // the tape is read again to count its blocks
        if (more && at < tape_count_data_blocks(info_drive, info_path)) {
            tape_note_write(info_drive, out, info_drive, info_path, at);
        } else {
            tape_note_remove(info_drive, out);
        }
    }
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
            grab_remove(base, screengrab::LOADING, conv_auto_number);
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
            if (cfg.loading_grab) {         // loading_grab = off in Picomenu.ini: only S saves one
                tape_ok = conv_save_screen(pconv::screen_data(c.machine), true);
            }
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
    int tn = snprintf(t, sizeof(t), "%u:%02u S-Screen X-Stop", (unsigned) (secs / 60), (unsigned) (secs % 60));
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

// ---- C in Tools: the batch converter ----
// A folder of the SD card is chosen (ACTION_BATCH, a screen of its own), then the game
// information screen runs in mode 3: each .tap and .tzx in the folder (not those below it, nor
// hidden ones) is converted as C does with Auto, into <folder>/Converted/<name>.z80. A tape
// whose .z80 is already there is skipped, so a batch that was stopped carries on where it got
// to. Each result goes in <folder>/Converted/Batch convert.txt; at the end (or BREAK) a summary.
// The tapes are found in the folder's own order, again for each (nothing is kept: the
// converter uses all the spare memory).
static uint8_t batch_phase = 0;             // 0 none, 1 chosen (starts at op 0), 2 running, 3 over
static uint16_t batch_saved = 0, batch_skipped = 0, batch_failed = 0;
static const char* batch_end_msg = nullptr; // why it stopped early, or nullptr
static uint8_t batch_page = 0;              // the chooser's page of folders
static const char* batch_msg = nullptr;     // the chooser's message row
static bool batch_machine128 = false;       // the Spectrum can page memory (the ROM's machine128)

// What the batch does: 0 tapes to .z80 (C, into Converted), 1 .z80 to tapes (T, into Tapes)
static uint8_t batch_kind = 0;
static const char* batch_out_folder() { return batch_kind == 0 ? "Converted" : "Tapes"; }

// kind 0: a .tap or .tzx; 1: a .z80 (not hidden)
static bool batch_is_input(DirHandle& d, uint8_t kind) {
    const char* n = d.fileName();
    if (d.isDirectory() || n[0] == '.' || d.isHiddenSet() || d.isSystem()) return false;
    size_t len = strlen(n);
    if (len <= 4) return false;
    if (kind == 1) return strcasecmp(n + len - 4, ".z80") == 0;
    return strcasecmp(n + len - 4, ".tap") == 0 || strcasecmp(n + len - 4, ".tzx") == 0;
}

// The chooser's folders: in list_pool, offsets then the names, sorted
static const int BATCH_DIRS_MAX = 500;
static uint16_t* batch_dir_offsets() { return reinterpret_cast<uint16_t*>(list_pool); }
static int batch_dir_count = 0;
static int batch_tapes_here = 0;
static int batch_z80_here = 0;

static int batch_dir_compare(const void* x, const void* y) {
    return strcasecmp(list_pool + *(const uint16_t*) x, list_pool + *(const uint16_t*) y);
}

// Reads the folder being chosen (sd_cwd): its folders, and how many tapes it has
__attribute__((noinline)) static bool batch_read_folder() {
    walk_stop();
    list_pool_busy = true;
    batch_dir_count = 0;
    batch_tapes_here = 0;
    batch_z80_here = 0;
    char path[sizeof(sd_cwd) + 4];
    snprintf(path, sizeof(path), "/sd%s", sd_cwd);
    DirHandle& d = find_dirs[0];
    if (!d.open(path)) {
        if (sd_cwd[0] == '\0') return false;
        sd_cwd[0] = '\0';                       // gone: the root
        snprintf(path, sizeof(path), "/sd");
        if (!d.open(path)) return false;
    }
    size_t used = BATCH_DIRS_MAX * sizeof(uint16_t);
    while (!d.atEnd()) {
        const char* n = d.fileName();
        if (batch_is_input(d, 0)) {
            batch_tapes_here++;
        } else if (batch_is_input(d, 1)) {
            batch_z80_here++;
        } else if (is_listable_dir(d) && n[0] != '.' && !d.isHiddenSet() && !d.isSystem() &&
                   strcasecmp(n, "Converted") != 0 && strcasecmp(n, "Tapes") != 0 && batch_dir_count < BATCH_DIRS_MAX) {
            size_t len = strlen(n) + 1;
            if (used + len <= LIST_POOL_SIZE) {
                memcpy(list_pool + used, n, len);
                batch_dir_offsets()[batch_dir_count++] = (uint16_t) used;
                used += len;
            }
        }
        if (!d.next()) break;
    }
    d.close();
    sort_u16(batch_dir_offsets(), (size_t) batch_dir_count, batch_dir_compare);
    return true;
}

static void batch_banner(uint8_t* screen, const char* title) {
    memset(screen, 0, 6144);
    memset(screen + 6144, ui_attr(0x07), 768);
    memset(screen + 6144, gameinfo::ATTR_BANNER, 32);
    help_put_text(screen, 0, 1, title, (int) strlen(title), gameinfo::ATTR_BANNER);
}

static void batch_put(uint8_t* screen, int row, const char* text, uint8_t attr = gameinfo::ATTR_TEXT) {
    help_put_text(screen, row, 1, text, (int) strlen(text), attr);
}

// "SD:/Games/Arcade" in 31 letters at most (the end of a long one)
static void batch_where(char* out, size_t size) {
    char t[sizeof(sd_cwd) + 8];
    snprintf(t, sizeof(t), "SD:%s", sd_cwd[0] ? sd_cwd : "/");
    size_t n = strlen(t);
    if (n > 31) snprintf(out, size, "..%s", t + n - 29);
    else snprintf(out, size, "%s", t);
}

// ACTION_BATCH: BYTE param1 = the key (0: opened). Status 0 the screen stays, 1 closed,
// 3 convert the tapes of sd_cwd (the ROM then shows the game information in mode 3)
__attribute__((noinline)) static void nmi_action_batch() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t key = nmi_rom_data[2];
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer);
    if (key >= 'A' && key <= 'Z') key = (uint8_t) (key - 'A' + 'a');
    const int per_page = 10;
    if (key <= 1) {                     // opened: 1 if the Spectrum can page memory
        batch_machine128 = key == 1;
        batch_phase = 0;
        batch_page = 0;
        batch_msg = nullptr;
        if (!sd_card_mounted) {
            batch_banner(screen, "Batch convert");
            batch_put(screen, 2, "It converts the tapes of a");
            batch_put(screen, 3, "folder on the SD card, and");
            batch_put(screen, 4, "there is no SD card (or the");
            batch_put(screen, 5, "PC has it).");
            batch_put(screen, 7, "Press any key", gameinfo::ATTR_KEYS);
            batch_phase = 3;            // (any key closes)
            preview_screen = screen;
            nmi_rom_data[0] = 0;
            return;
        }
        batch_read_folder();
    } else if (batch_phase == 3) {
        nmi_rom_data[0] = 1;
        return;
    } else {
        batch_msg = nullptr;
        int pages = (batch_dir_count + per_page - 1) / per_page;
        if (key == 'x' || is_key_break(key)) {
            walk_stop();
            nmi_rom_data[0] = 1;
            return;
        } else if (key >= '0' && key <= '9') {
            int i = batch_page * per_page + (key == '0' ? 9 : key - '1');
            if (i < batch_dir_count) {
                const char* name = list_pool + batch_dir_offsets()[i];
                size_t have = strlen(sd_cwd), len = strlen(name);
                if (have + 1 + len < sizeof(sd_cwd)) {
                    sd_cwd[have] = '/';
                    memcpy(sd_cwd + have + 1, name, len + 1);
                    batch_page = 0;
                    batch_read_folder();
                } else {
                    batch_msg = "That folder's path is too long";
                }
            }
        } else if (key == 'u') {
            char* slash = strrchr(sd_cwd, '/');
            if (slash != nullptr) {
                *slash = '\0';
                batch_page = 0;
                batch_read_folder();
            }
        } else if (key == 'n' && batch_page + 1 < pages) {
            batch_page++;
        } else if (key == 'p' && batch_page > 0) {
            batch_page--;
        } else if (key == 'c' || key == 't' || key == notes::KEY_ENTER || key == 13) {
            uint8_t kind = key == 't' ? 1 : 0;
            int count = kind == 1 ? batch_z80_here : batch_tapes_here;
            if (count == 0) {
                batch_msg = kind == 1 ? "No .z80 in this folder" : "No .tap or .tzx in this folder";
            } else {
                walk_stop();
                batch_kind = kind;
                batch_total = (uint16_t) count;
                batch_phase = 1;
                nmi_rom_data[0] = 3;
                return;
            }
        }
    }
    batch_banner(screen, "Batch convert: which folder?");
    char t[48];
    batch_where(t, sizeof(t));
    batch_put(screen, 2, t, gameinfo::ATTR_HEADING);
    snprintf(t, sizeof(t), "%d tape%s, %d .z80 here", batch_tapes_here, batch_tapes_here == 1 ? "" : "s", batch_z80_here);
    batch_put(screen, 3, t, batch_tapes_here + batch_z80_here > 0 ? gameinfo::ATTR_TEXT : gameinfo::ATTR_DIM);
    int pages = (batch_dir_count + per_page - 1) / per_page;
    if (batch_page >= pages) batch_page = 0;
    for (int k = 0; k < per_page; k++) {
        int i = batch_page * per_page + k;
        if (i >= batch_dir_count) break;
        snprintf(t, sizeof(t), "%d-%.27s/", k == 9 ? 0 : k + 1, list_pool + batch_dir_offsets()[i]);
        batch_put(screen, 5 + k, t);
    }
    if (batch_dir_count == 0) batch_put(screen, 5, "(no folders in it)", gameinfo::ATTR_DIM);
    if (pages > 1) {
        snprintf(t, sizeof(t), "Page %d/%d  N-Next P-Previous", batch_page + 1, pages);
        batch_put(screen, 16, t, gameinfo::ATTR_KEYS);
    }
    batch_put(screen, 18, "1-0 Open a folder  U-Up", gameinfo::ATTR_KEYS);
    batch_put(screen, 19, "C-Tapes here to .z80", gameinfo::ATTR_KEYS);
    batch_put(screen, 20, "T-.z80 here to tapes", gameinfo::ATTR_KEYS);
    batch_put(screen, 21, "X-Back", gameinfo::ATTR_KEYS);
    batch_put(screen, 22, "Into Converted/ or Tapes/ here", gameinfo::ATTR_DIM);
    if (batch_msg != nullptr) batch_put(screen, 23, batch_msg, gameinfo::ATTR_HEADING);
    preview_screen = screen;
    nmi_rom_data[0] = 0;
}

// A line at the end of <folder>/Converted/Batch convert.txt (made if need be)
static void batch_log(const char* a, const char* b = "", const char* c = "", const char* d = "") {
    int n = snprintf(grab_path, sizeof(grab_path), "%s/%s/Batch convert.txt", sd_cwd, batch_out_folder());
    if (n < 0 || n >= (int) sizeof(grab_path)) return;
    FileHandle& f = *notes_handle();
    if (!f.openAppend(Drive::EXTERNAL_SD_CARD, grab_path)) return;
    f.write(a, (int) strlen(a));
    f.write(b, (int) strlen(b));
    f.write(c, (int) strlen(c));
    f.write(d, (int) strlen(d));
    f.write("\r\n", 2);
    f.close();
}

// The next tape of the folder that has no .z80 in Converted yet, from the batch_done'th on
// (counting the skipped ones): its path and name in info_path / info_name. False at the end.
__attribute__((noinline)) static bool batch_next_tape() {
    char path[sizeof(sd_cwd) + 4];
    snprintf(path, sizeof(path), "/sd%s", sd_cwd);
    for (;;) {
        DirHandle& d = find_dirs[0];
        if (!d.open(path)) return false;
        int k = 0;
        bool found = false;
        while (!d.atEnd()) {
            if (batch_is_input(d, batch_kind) && k++ == batch_done) {
                found = true;
                snprintf(info_path, sizeof(info_path), "%s/%s", sd_cwd, d.fileName());
                break;
            }
            if (!d.next()) break;
        }
        d.close();
        if (!found) return false;
        // the name without the extension, as the game information has it
        const char* slash = strrchr(info_path, '/');
        const char* file = slash != nullptr ? slash + 1 : info_path;
        snprintf(info_name, sizeof(info_name), "%.*s", (int) strlen(file) - 4, file);     // (.tap .tzx .z80)
        int n = snprintf(grab_path, sizeof(grab_path), "%s/%s/%s.%s", sd_cwd, batch_out_folder(), info_name,
            batch_kind == 1 ? "tap" : "z80");
        if (n > 0 && n < (int) sizeof(grab_path) && fsops::kind(Drive::EXTERNAL_SD_CARD, grab_path) != fsops::KIND_NONE) {
            batch_skipped++;
            batch_done++;
            continue;                           // converted already
        }
        info_drive = 1;
        return true;
    }
}

static void batch_summary(uint8_t* screen) {
    batch_banner(screen, batch_end_msg != nullptr ? "Batch convert: stopped" : "Batch convert: done");
    char t[48];
    batch_where(t, sizeof(t));
    batch_put(screen, 2, t, gameinfo::ATTR_HEADING);
    snprintf(t, sizeof(t), "Saved:          %u", (unsigned) batch_saved);
    batch_put(screen, 4, t);
    snprintf(t, sizeof(t), "Already done:   %u", (unsigned) batch_skipped);
    batch_put(screen, 5, t);
    snprintf(t, sizeof(t), batch_kind == 1 ? "Not converted:  %u" : "Did not load:   %u", (unsigned) batch_failed);
    batch_put(screen, 6, t, batch_failed > 0 ? gameinfo::ATTR_HEADING : gameinfo::ATTR_TEXT);
    if (batch_done < batch_total) {
        snprintf(t, sizeof(t), "Not looked at:  %u", (unsigned) (batch_total - batch_done));
        batch_put(screen, 7, t);
    }
    snprintf(t, sizeof(t), "The %s files are in", batch_kind == 1 ? ".tap" : ".z80");
    batch_put(screen, 9, t);
    snprintf(t, sizeof(t), "%s/ in that folder, and", batch_out_folder());
    batch_put(screen, 10, t);
    batch_put(screen, 11, batch_kind == 1 ? "what happened to each in" : "what happened to each tape in");
    snprintf(t, sizeof(t), "%s/Batch convert.txt", batch_out_folder());
    batch_put(screen, 12, t);
    if (batch_end_msg != nullptr) {
        batch_put(screen, 14, batch_end_msg, gameinfo::ATTR_HEADING);
        if (strncmp(batch_end_msg, "Stopped", 7) == 0) batch_put(screen, 15, batch_kind == 1 ? "T there again carries on." : "C there again carries on.", gameinfo::ATTR_DIM);
    }
    batch_put(screen, 23, "X-Back", gameinfo::ATTR_KEYS);
}

// A tape is over (conv_msg says how): counted and written down
static void batch_tape_over() {
    const char* tape = info_path + strlen(sd_cwd) + 1;
    if (strncmp(conv_msg, "Saved", 5) == 0) {
        batch_saved++;
        batch_log("Saved: ", tape, " ->", conv_msg + 5);
    } else {
        batch_failed++;
        batch_log("Not loaded: ", tape, " - ", conv_msg);
    }
    batch_done++;
}

// Starts the next tape (skipping those done); false if there are none left or it could not
// start (then batch_phase is 3)
static bool batch_start_next(uint8_t& keep) {
    while (batch_next_tape()) {
        conv_machine128 = batch_machine128;
        conv_phase = CONV_CHOOSE;
        if (conv_op(0, 'a', keep)) return true;       // started, and its first slice run
        // over already: no ROM files (every tape would fail: the batch stops), or the tape
        // could not be opened, or it was all done in the first slice
        if (strncmp(conv_msg, "No ", 3) == 0) {
            batch_end_msg = conv_msg;
            batch_log("Stopped: ", conv_msg);
            return false;
        }
        batch_tape_over();
    }
    return false;
}

// ---- T on the Batch convert screen: .z80 snapshots to tapes (z80tap.h) ----
// One snapshot a request: read into snap_load_buffer, its tape written to <folder>/Tapes/<name>.tap
// (the loader's plan and bytes in list_pool), then a screen saying how far it has got, and
// byte 1 is 6 so the ROM asks again. False when there are no more (or it stopped).
struct TapFileSink {
    FileHandle* f;
    bool write(const void* p, size_t n) { return f->write(p, n) == (int) n; }
};
static_assert(sizeof(z80tap::Plan) + z80tap::LC_SIZE + 8 <= LIST_POOL_SIZE, "the tape's plan fits in list_pool");

__attribute__((noinline)) static const char* batch_tap_one() {
    uint8_t* buf = reinterpret_cast<uint8_t*>(snap_load_buffer);
    FileHandle& f = conv_scratch().file;            // (the converter is not running)
    if (!f.open(Drive::EXTERNAL_SD_CARD, info_path, true)) return "cannot read it";
    int32_t size = f.size();
    // a version 1 file (PC in the header) is 49182 bytes, or compressed and ending 00 ED ED 00:
    // anything else is not a .z80 (the reader takes any 30 bytes for a header)
    uint8_t h[30], tail[4];
    bool v1 = f.read(h, 30) == 30 && (h[6] | h[7]) != 0;
    bool good = !v1 || size == (int32_t) snapload::Z80V1_SIZE ||
        ((h[12] & 0x20) && size > 34 && f.seek((uint32_t) size - 4) && f.read(tail, 4) == 4 &&
         tail[0] == 0 && tail[1] == 0xED && tail[2] == 0xED && tail[3] == 0);
    if (!good || !f.seek(0)) {
        f.close();
        return "not a .z80 it can read";
    }
    snapload::Kind k = snapload::load_z80(f, size, buf, snap_io_buffer, sizeof(snap_io_buffer));
    f.close();
    if (k == snapload::KIND_ERROR) return "not a .z80 it can read";
    z80tap::Snap s{};
    s.hdr = buf;
    s.is128 = k == snapload::KIND_PAGED;
    if (s.is128) {
        for (int b = 0; b < 8; b++) s.bank[b] = snapload::bank_memory(buf, (uint32_t) b);
    } else {
        s.ram48 = buf + 30;
    }
    z80tap::Plan* plan = new (list_pool) z80tap::Plan();
    uint8_t* lc = reinterpret_cast<uint8_t*>(list_pool) + ((sizeof(z80tap::Plan) + 7) & ~(size_t) 7);
    int n = snprintf(list_path, sizeof(list_path), "%s/Tapes/%s.tap", sd_cwd, info_name);
    if (n < 0 || n >= (int) sizeof(list_path)) return "its name is too long";
    if (!f.open(Drive::EXTERNAL_SD_CARD, list_path, false)) return "cannot write the tape";
    TapFileSink sink{ &f };
    const char* why = z80tap::write_tap(sink, s, info_name, lc, *plan);
    bool closed = f.close();
    if (why == nullptr && !closed) why = "could not write the file";
    if (why != nullptr) fsops::remove_path(Drive::EXTERNAL_SD_CARD, list_path);
    LOG("Tape of %s: %s\n", info_path, why ? why : "saved");
    return why == nullptr ? (s.is128 ? "" : nullptr) : why;     // "" saved (128K), nullptr saved (48K)
}

__attribute__((noinline)) static bool batch_tap_step() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    if (!batch_next_tape()) return false;           // (skips those done)
    const char* why = batch_tap_one();
    const char* file = info_path + strlen(sd_cwd) + 1;
    if (why == nullptr || why[0] == '\0') {
        batch_saved++;
        batch_log("Saved: ", file, why == nullptr ? " -> a 48K tape" : " -> a 128K tape");
    } else {
        batch_failed++;
        batch_log("Not converted: ", file, " - ", why);
    }
    batch_done++;
    walk_stop();                                    // (list_pool was used)
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer) + INFO_SCREEN_OFFSET;
    batch_banner(screen, "Batch convert: .z80 to tapes");
    char t[48];
    batch_where(t, sizeof(t));
    batch_put(screen, 2, t, gameinfo::ATTR_HEADING);
    snprintf(t, sizeof(t), "%u/%u %.24s", (unsigned) batch_done, (unsigned) batch_total, info_name);
    batch_put(screen, 4, t);
    snprintf(t, sizeof(t), "Saved %u, already done %u", (unsigned) batch_saved, (unsigned) batch_skipped);
    batch_put(screen, 6, t);
    if (batch_failed > 0) {
        snprintf(t, sizeof(t), "Not converted %u", (unsigned) batch_failed);
        batch_put(screen, 7, t, gameinfo::ATTR_HEADING);
    }
    batch_put(screen, 23, "X-Stop", gameinfo::ATTR_KEYS);
    preview_screen = screen;
    conv_overlay_on = false;
    nmi_rom_data[1] = 6;                            // the ROM asks again
    nmi_rom_data[3] = 0;
    nmi_rom_data[0] = 1;
    return true;
}

// ACTION_GAME_INFO in mode 3: op 0 starts the batch (or shows the summary), op 6 goes on
// (key 0) or takes a key (BREAK/X stop it, S saves the screen as C does)
__attribute__((noinline)) static void nmi_batch_info(int op, uint8_t key) {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint8_t keep = 0;
    if (key >= 'A' && key <= 'Z') key = (uint8_t) (key - 'A' + 'a');
    if (batch_phase == 1 && op == 0) {
        batch_phase = 2;
        batch_saved = batch_skipped = batch_failed = 0;
        batch_done = 0;
        batch_end_msg = nullptr;
        batch_on = true;
        batch_on = batch_kind == 0;
        snprintf(grab_path, sizeof(grab_path), "%s/%s", sd_cwd, batch_out_folder());     // (a static: the stack is small)
        uint8_t made = make_sd_folder(grab_path);
        if (made != fsops::OK && made != fsops::NAME_IN_USE) {
            batch_end_msg = batch_kind == 1 ? "Could not make Tapes/" : "Could not make Converted/";
        } else {
            batch_log(batch_kind == 1 ? "Batch convert (.z80 to tapes) of SD:" : "Batch convert of SD:", sd_cwd[0] ? sd_cwd : "/");
            if (batch_kind == 1) {
                if (batch_tap_step()) return;
            } else if (batch_start_next(keep)) {
                return;
            }
        }
    } else if (batch_phase == 2 && op == 6 && batch_kind == 1) {
        if (key == 0x18 || key == 'x') {
            batch_end_msg = "Stopped with BREAK";
            batch_log("Stopped");
        } else if (batch_tap_step()) {
            return;
        }
    } else if (batch_phase == 2 && op == 6) {
        bool stopping = key == 0x18 || key == 'x';
        if (conv_op(0, key, keep)) return;            // still converting this tape
        // this tape is over
        const char* tape = info_path + strlen(sd_cwd) + 1;
        if (stopping) {
            batch_end_msg = "Stopped with BREAK";
            batch_log("Stopped at ", tape);
        } else {
            batch_tape_over();
            if (batch_start_next(keep)) return;
        }
    }
    if (batch_phase == 2) {
        batch_phase = 3;
        batch_on = false;
        char n[3][8];
        snprintf(n[0], sizeof(n[0]), "%u", (unsigned) batch_saved);
        snprintf(n[1], sizeof(n[1]), "%u", (unsigned) batch_skipped);
        snprintf(n[2], sizeof(n[2]), "%u", (unsigned) batch_failed);
        batch_log("Saved ", n[0], ", already done ", n[1]);
        batch_log(batch_kind == 1 ? "Not converted " : "Not loaded ", n[2]);
        conv_end();                     // (the list and the information are read again)
    }
    batch_on = false;
    uint8_t* screen = reinterpret_cast<uint8_t*>(snap_load_buffer) + INFO_SCREEN_OFFSET;
    batch_summary(screen);
    preview_screen = screen;
    conv_overlay_on = false;
    nmi_rom_data[1] = 0;                // the ROM waits for a key: X leaves
    nmi_rom_data[3] = 0;
    nmi_rom_data[0] = 1;                // one page, no pictures
}

// ---- Menu grabs: a double press of the button with a menu up (menushot.h) ----
// The main loop sets menu_shot in the ROM; the ROM's key scan sees it and sends the screen
// (screentopico, ACTION_SCREEN_STASH pieces), which go here instead of over the game's screen.
// With the last piece the screen is saved as /Scraper/Screen Grabs/<menu>.png (<menu> 2.png
// ... if that is there), and the answer is the border colour: 4 green saved, 2 red not.
// It is kept in a part of snap_load_buffer that nothing else uses while a menu is up (the
// place of the 128K bank 4); not while a snapshot to be saved is there (sna_in_buffer).
static const uint32_t MENU_SHOT_OFFSET = 82944;
static_assert(MENU_SHOT_OFFSET + menushot::SCR_SIZE <= SNAP_LOAD_BUFFER_SIZE, "the menu grab fits");

// (the screen grabs' sink: one write_png for both)
using ShotSink = GrabSink;

__attribute__((noinline)) static bool menu_shot_save(const uint8_t* scr) {
    if (!sd_card_mounted || sna_in_buffer) return false;
    if (fsops::kind(Drive::EXTERNAL_SD_CARD, screengrab::SCRAPER_FOLDER) != fsops::KIND_DIR) {
        if (list_pool_busy) return false;          // making it borrows list_pool, which a screen is using
        if (!grab_folder_make()) return false;
        walk_stop();
    }
    char name[menushot::NAME_MAX + 1];
    menushot::name_of(scr, menu_font(), name);
    bool free_name = false;
    for (int k = 1; k <= 99 && !free_name; k++) {
        char suffix[4] = "";
        if (k > 1) snprintf(suffix, sizeof(suffix), " %d", k);
        int n = snprintf(grab_path, sizeof(grab_path), "%s/%s%s.png", screengrab::SCRAPER_FOLDER, name, suffix);
        if (n < 0 || n >= (int) sizeof(grab_path)) return false;
        free_name = fsops::kind(Drive::EXTERNAL_SD_CARD, grab_path) == fsops::KIND_NONE;
    }
    if (!free_name) return false;
    FileHandle& f = *notes_handle();
    if (!f.open(Drive::EXTERNAL_SD_CARD, grab_path, false)) return false;
    ShotSink sink{ &f };
    bool ok = menushot::write_png(sink, scr);
    ok = f.close() && ok;
    if (!ok) fsops::remove_path(Drive::EXTERNAL_SD_CARD, grab_path);
    LOG("Menu grab %s: %s\n", grab_path, ok ? "saved" : "not saved");
    return ok;
}

// A piece of the screen for a menu grab (ACTION_SCREEN_STASH while menu_shot is set): true
// if it was taken (the status is set)
static bool menu_shot_piece() {
    uint8_t* nmi_rom_data = rom_data + 16384;
    uint16_t buffer = nmi_rom_data[2] | (nmi_rom_data[3] << 8);
    uint16_t address = nmi_rom_data[4] | (nmi_rom_data[5] << 8);
    uint16_t length = nmi_rom_data[6] | (nmi_rom_data[7] << 8);
    uint8_t* shot = reinterpret_cast<uint8_t*>(snap_load_buffer) + MENU_SHOT_OFFSET;
    if (address < 16384 || address - 16384 + length > menushot::SCR_SIZE || buffer + length > 16384) {
        return false;
    }
    uint32_t at = address - 16384;
    if (!sna_in_buffer && conv_phase != CONV_RUN) memcpy(shot + at, nmi_rom_data + buffer, length);
    if (at + length < menushot::SCR_SIZE) {
        nmi_rom_data[0] = 0;
        return true;
    }
    nmi_rom_data[START_MENUSHOT] = 0;           // (the ROM reads its answer after the last piece)
    bool ok = !sna_in_buffer && conv_phase != CONV_RUN && menu_shot_save(shot);
    if (!ok) set_last_error("Cannot save", "menu grab (.png)");
    nmi_rom_data[0] = ok ? 4 : 2;               // the border: green, red
    return true;
}


// The button held a second (held true) or pressed twice quickly: Picomenu.ini's save_press
// says which opens the save screen (S: the slots, or the Save snapshot screen) and which the
// screen grab question (G); either goes back to the game when it is closed. With a menu up a
// double press saves the menu's screen as a .png (menu_shot_piece), and a hold does nothing.
// False if it could not be done.
bool button_press_action(bool held) {
    if (rom_state.nmi_active) {
        if (held || conv_phase == CONV_RUN) return false;  // (converting: the buffer is the Spectrum's)
        rom_data[16384 + START_MENUSHOT] = 1;               // the ROM sends the screen (getkeypress)
        return true;
    }
    bool save = held == cfg.save_on_hold;
    return sendNmiRequest(save ? STARTUP_ACTION_SAVE_SCREEN : STARTUP_ACTION_GRAB_SCREEN, 0, 0);
}

// What a press of the button was, with Picomenu.ini's long_press and double_press (down: is it
// down now, wait: wait so many ms). The simulator's "button" gives it a made-up press.
extras::Press button_read(bool (*down)(), void (*wait)(uint32_t)) {
    return extras::classify_press(down, wait, cfg.long_press_ms, cfg.double_press);
}

// Does what the press says: a short one the NMI menu, held a second or a double press
// button_press_action, held for long_press (3 seconds) a reset into the Spectrum's own ROM.
// False if it could not be done (an NMI is already active).
bool button_act(extras::Press press) {
    switch (press) {
    case extras::Press::SHORT: return sendNmiRequest(0, 0, 0);
    case extras::Press::HOLD: return button_press_action(true);
    case extras::Press::DOUBLE: return button_press_action(false);
    default: reset_spectrum_to_internal_rom(); return true;
    }
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

    nmi_rom_unpack(rom_data + 16384);
    rom_state.flags = 0;
#ifdef WIFI_ENABLE
    // (outside .bss in this build, so not cleared by the start up: memmap_picomenu_w.ld)
    memset(snap_load_buffer, 0, sizeof(snap_load_buffer));
    // the crash recorder: a crash or hang before this start is kept for the B screen; the
    // watchdog (on while the Bluetooth is) is off until the Bluetooth is on again
    bt::crash_boot();
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
#endif

    multicore_launch_core1(do_my_pio);

    gpio_init(PIN_USER_SWITCH);
    gpio_set_dir(PIN_USER_SWITCH, false);
    gpio_pull_up(PIN_USER_SWITCH);

    gpio_init(PIN_NMI);
    gpio_put(PIN_NMI, true);
    gpio_set_dir(PIN_NMI, true);    


    sleep_ms(200);
    initFileSystem();

    uint8_t* nmi_rom_data = rom_data + 16384;

    while (true) {
        #ifdef WIFI_ENABLE
        if (web_on && !rom_state.nmi_active) web_off();     // the menu went (a reset): WiFi off
        if (web_on) on_net_stack(net_work);     // F - File transfer, ZXDB (on the WiFi's own stack)
        else wifi::poll();
        zx_watch();                             // (a ZXDB lookup: its steps noted, a hang caught)
        bt_poll();              // B - Bluetooth tape (while it is on)
        #endif
        usb_msc_task();
        if (!usb_msc_busy() && !web_busy()) {
            sleep_ms(1);        // while the PC is using the SD card go as fast as the card does
        }
        pollUsbCommandHandler();

        if (nmi_rom_data[0] == 255) {
            sd_setup_early();
            processNmiRequest();
        }
        while (save_stream) {
            // a snapshot being sent: its next piece at once (see save_stream). (volatile: the
            // ROM's write of 255 comes from the bus handler, and nothing else here would make
            // the compiler read it again)
            if (*(volatile uint8_t*) nmi_rom_data == 255) {
                processNmiRequest();
                save_stream_at = time_us_64();
            } else if (time_us_64() - save_stream_at > SAVE_STREAM_IDLE_US) {
                save_stream = false;
            }
        }

        tape_service_poll();
        #if PICOMENU_HISCORE
        hs_auto_poll();         // (not snap_load_buffer: with the Bluetooth on too)
        #endif
        #ifdef WIFI_ENABLE
        if (!bt_on) {           // (the Bluetooth's memory is in snap_load_buffer, which they use)
            level_loaders_poll();
            checkpoint_poll();
        }
        #else
        level_loaders_poll();
        checkpoint_poll();
        #endif

        bool isup = gpio_get(PIN_USER_SWITCH);
        if (cfg.hotkeys) {
            // hotkeys: the button's state, steady for BUTTON_SETTLE_MS
            static bool raw = false, steady = false;
            static uint32_t since = 0;
            uint32_t now = to_ms_since_boot(get_absolute_time());
            if (!isup != raw) {
                raw = !isup;
                since = now;
            } else if (raw != steady && now - since >= extras::SETTLE_MS) {
                steady = raw;
                hotkeys_button(steady);
            }
        } else if (!isup) {

            extras::Press press = button_read([] { return !gpio_get(PIN_USER_SWITCH); }, [](uint32_t ms) { sleep_ms(ms); });
            if (!button_act(press)) {
                LOG("Couldn't do the button's press because an NMI is already active.\n");
            }
            if (press != extras::Press::SHORT) {
                while (!gpio_get(PIN_USER_SWITCH)) {
                    sleep_ms(10);       // until the button is let go
                }
            }
        }
    }
    
}

#endif