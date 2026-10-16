# The tape converter: development notes

How the tape converter (C on a tape's information) was built and tested, from the first scope to Rev 1.9. It was developed as test builds 1 to 7 (`Rev 1.8.1 tzx` to `Rev 1.8.7 tzx`), kept apart from Rev 1.8 until it had been tried on a real Picoface, and is part of **Rev 1.9** (see CHANGELOG.md). The sections below are in the order the work was done, so earlier ones can describe things later ones changed: the last sections are the current state.

The original plan is the project doc `tzx-converter-scope.md`.

## Step 1: picoface.cxx built for size

`pico/CMakeLists.txt`: `picoface.cxx` is now `-Os` like everything else (it was `-O3`). Measured with Pico SDK 1.5.1 and GCC 13.2.1, Release build:

| | Rev 1.8 (-O3) | This build (-Os) |
|---|---|---|
| Firmware code | 229,908 bytes | **175,252 bytes** |
| Room left (of 253,952) | 24,044 | **78,700** |
| RAM (bss) | 250,680 | 250,664 |

**Stack.** This was measured with GCC's `-fcallgraph-info=su` on every file, adding up the deepest chain of calls (`tools/stackdepth.py`). Calls through function pointers aren't followed; there are about 50, mostly FatFs/littlefs callbacks.

| Deepest chain starting at | -O3 | -Os |
|---|---|---|
| USB command handler (`handleDownload`, which has a 4.7 KB buffer on the stack) | 6,064 | 5,896 |
| USB snapshot upload / download | 4,120 | 3,336 |
| NMI menu: load a snapshot (`load_snapshot_file`) | 4,008 | 3,224 |
| Game information (`nmi_game_info_show`) | 3,288 | lower |

-Os uses **less** stack everywhere, so the switch doesn't make the stack worse.

**Already in Rev 1.8, not caused by this build:** the CMake comment says core 0 has 2 KB of stack. In fact its stack runs from 0x20042000 down to core 1's stack at 0x20040800, so it has 6 KB in practice. The USB command path needs about 5.9 KB before any interrupt is added on top. It works today, but it is close to the edge. The 4.7 KB buffer in `handleDownload` could be made static if RAM allows; that is worth a look some time.

**To check on the real machine:** that menus, Browse, preview and saving feel the same speed. All the time-critical bus work is in `piohandler.S` (assembly), so no change is expected.

## Step 2: the PC prototype

New files:
- `pico/tzxconv.h`: the converter, written to go into the firmware unchanged.
  - `Player` plays a .tzx/.tap as a signal: blocks 0x10–0x15, 0x18 (CSW, plain RLE), 0x19 (generalised data), pauses, stops, loops, jumps, calls, set signal level; info blocks are skipped.
  - `Machine` is a 48K Spectrum: it boots `48.rom`, types `LOAD ""`, plays the tape into the EAR bit at the exact T-state of each `IN`, runs 2 s after the tape ends, and fills a .sna header with the exact registers (including R).
  - It allocates no memory: the caller gives the ROM and the 48K.
- `pico/z80/`: the Z80 emulator, redcode/Z80 (LGPL-3.0), unchanged, plus the Zeta headers it uses. Licence details are in `pico/z80/README.md`.
- `pico/snapload.h`: `write_z80_48` takes an optional PC. That way the converter's snapshot doesn't push the PC onto the game's stack, and no bytes get overwritten.
- `tools/tzxconv/`: the PC tool (`build.sh`, `tzxconv_host.cpp`), the tests (`tests/`) and the Cortex-M0+ speed test (`bench/`).

### How it decides
- **Tape loading error:** the ROM's R error report is seen, so it says "did not load".
- **Stopped:** the Spectrum is at a HALT with interrupts off, which is what a loader does when its checksum fails. It says "did not load".
- **BASIC error:** the program is back in BASIC with an error report. It says "did not load".
- **Damaged tape:** a damaged file or a block it doesn't know. It says "did not load".
- **Saved with a warning:** the snapshot is saved, but with a warning, in two cases:
  - after the tape ended, the program kept reading the tape port 500+ times a frame in a small loop (a loader waiting for more, or a game waiting for a key);
  - it is waiting in BASIC with no error.
- **Loaded:** anything else. The snapshot is saved.

### Tests
`tools/tzxconv/tests/runtests.sh 48.rom` builds the tool, makes 15 tapes and checks every result. **15 of 15 pass.** The tapes are made here (no commercial tapes yet):
- `game.asm` is a small "game" that leaves `GAMEOK` at 0x9000 and counts frames.
- `turbo.asm` is a home-made turbo loader: its own pilot, sync and bit timings, a checksum, and **decryption keyed on the R register** (with bit 7 set), as protected loaders do.
- `maketapes.py` writes the same load in every way a .tzx can describe a signal.

| Tape | What it tests | Result |
|---|---|---|
| standard_rom .tap and .tzx | plain ROM loader, LOAD "" CODE | loaded, checked |
| turbo_11 | turbo blocks + R decryption | loaded, checked |
| tone_pulses_14 | pure tone + pulse sequence + pure data | loaded, checked |
| direct_15 | direct recording (79 T samples) | loaded, checked |
| csw_18 | CSW at 44.1 kHz | loaded, checked |
| generalised_19 | generalised data | loaded, checked |
| loop_jump | pilot as a loop; a jump over junk | loaded, checked |
| call_seq | call sequence and returns | loaded, checked |
| stop_20 | "stop the tape" between blocks (played through) | loaded, checked |
| info_blocks | text, archive info, group blocks | loaded, checked |
| bad_rom_checksum | ROM loader error | "R Tape loading error" |
| bad_turbo_checksum | turbo checksum fails | "the Spectrum stopped" |
| unknown_block | block 0x99 | "damaged tape or unknown block" |
| missing_turbo | the turbo blocks missing | saved, warning "still reading" |

"Checked" means the .z80 was opened by a separate Python decoder. That decoder confirmed:
- the game bytes and the 6,912-byte turbo screen are exact;
- GAMEOK is at 0x9000 and the game has run;
- PC is in the game's loop (on its HALT);
- IM 1, interrupts on, border 5.

### Size and memory on the Pico
- Z80 emulator: 16,802 bytes (-Os).
- Converter (player + machine): about 5.2 KB.
- Total about 22 KB, of the 78.7 KB that step 1 freed.
- RAM: about 420 bytes of statics. The 48K and the ROM go in `snap_load_buffer`.

### Speed (simulated, not yet measured on a Pico)
The converter was built for the Cortex-M0+ and run in the CPU core of rp2040js, which counts clock cycles per instruction. The XIP flash cache was modelled with misses costing 50 cycles. It converted `turbo_11.tzx` correctly: 53.2 s of Spectrum time, the game checked.

| Z80 emulator built with | Z80.c size | Speed at 125 MHz | At 250 MHz (overclocked) |
|---|---|---|---|
| -Os | 16,802 | 1.27x a real Spectrum | 2.5x |
| **-O2** | 16,860 | **1.65x** | 3.3x |
| -O3 | 19,064 | 1.72x | 3.4x |

- **Use -O2 for Z80.c and the converter:** the same size as -Os and 30% faster.
- Cache misses are negligible: the emulator's inner loop stays in the 16 KB cache.
- **What it means:** a tape takes about 60% of its playing time (a 4-minute tape about 2.5 minutes). Overclocking would roughly halve that. The real figure will be a little slower, because core 0 also has to keep USB and the NMI menu going.
- **Faster still:** loader acceleration, as Fuse does. Spot the loader's "wait for an edge" loop and jump ahead to the next edge instead of running it. Most of a tape's time is spent in that loop, so this could make it several times faster. It is a later step, once real tapes show it is needed.

## Not done yet / known limits
- **Real tapes:** Speedlock 1–7 have been tried (see below): six work, and the 128K-only one is reported as such. Alkatraz, Bleepload, Firebird and others are not tried yet.
- No memory contention or floating bus, so a loader that depends on exact contended timing may fail.
- CSW blocks compressed with Z-RLE are skipped (counted as "not played").
- 48K only. Multi-load games give only the first load.
- "Stop the tape" blocks are played through after 1 s. A multi-load tape therefore plays to its end, which costs time but does no harm.
- The firmware side (step 3) isn't started: the Convert key, progress bar, BREAK, saving beside the tape and Help.

## Real tapes: Speedlock 1 to 7 (2026-10-15)
These are your seven tapes, converted with your 48.rom on the PC build. "Works" means:
- I looked at the screen when the snapshot was taken, then let the game run on with SPACE and then ENTER held, and looked again;
- the saved .z80 was decoded separately and its screen matches.

| Game (Speedlock version) | Result | Spectrum time | Estimated time on the Pico at 125 MHz |
|---|---|---|---|
| Mikie (1) | **works**: intro, then the menu | 196 s | about 2 min |
| Green Beret (2) | **works**: title, then the menu | 234 s | about 2½ min |
| Renegade 128K (3) | **not loaded**: the loader resets a 48K Spectrum at the end ("the Spectrum was reset (the tape may need a 128K)"), no file | 313 s | about 3 min to find out |
| Athena 128K (4) | **works**: title, then the menu (this tape loads on a 48K too) | 524 s | about 5½ min |
| Road Blasters (5) | **works**: "press SPACE to play", then the game starts. It is a multi-load: later rallies need the tape | 337 s (tape 1,305 s) | about 3½ min |
| Platoon (6) | **works**: title page | 328 s (tape 598 s) | about 3½ min |
| RoboCop (7) | **works**: high scores, then the menu | 259 s (tape 531 s) | about 2½ min |

Pico times are the Spectrum time divided by 1.65, the simulated speed. Overclocking would roughly halve them, and loader acceleration could cut them much further.

### Changes made after these tapes
- **Reset found.** A jump to address 0 after the tape has started is reported as "the Spectrum was reset (the tape may need a 128K)", and no file is written. Before this, Renegade was wrongly saved as "loaded" (its snapshot was the © 1982 screen).
- **Stopped early when the data is no longer being loaded.** If data bits play for 5 s while the tape port is hardly read (fewer than 50 reads a frame), or is read only in a loop under 16 bytes (a "press SPACE" loop), the load is over. The rest of the tape is for later levels.
  - Platoon stops at 328 s instead of 600 s, RoboCop at 259 s instead of 533 s, and Road Blasters at 337 s instead of 1,307 s.
  - Only data bits count, not tones or pulses: Athena's Speedlock 4 plays 14 s of clicks that its loader ignores while it decrypts. A first version that counted tones stopped Athena too soon, with a black screen.
  - `TZXFULL=1` turns this off on the PC.
- The Road Blasters warning ("still reading the tape port") was a false alarm: the game was waiting for SPACE. Since the early stop, it no longer appears for this tape.
- PC tool: `TZXSHOTS=name` saves the screen at the snapshot and after the game has run on with SPACE and ENTER held (`name-0.scr` to `name-3.scr`). `scr2png.py` turns those into a picture. `Machine::run_on()` was added for this.
- The 15 home-made tests still pass.

## More real tapes: Power-Load, Firebird SoftLock, Erbe, New Generation (2026-10-15)

| Game (loader) | Result | Spectrum time | Estimated time on the Pico at 125 MHz |
|---|---|---|---|
| Dynamite Dan (Power-Load) | **works**: menu, then the game starts with ENTER | 169 s | about 1¾ min |
| Rainbow Islands (Erbe re-release) | **works**: "STOP TAPE AND PRESS ANY KEY", then the title | 321 s (tape 675 s) | about 3¼ min |
| Rasputin (Firebird SoftLock) | **works**: the option menu | 266 s | about 2¾ min |
| Travel with Trashman (New Generation) | **works**: the control menu | 185 s | about 2 min |

So far that is 10 of 11 real tapes loaded, and the 11th is the 128K-only Renegade, which is correctly reported. Every saved .z80 was decoded separately and its screen matches the emulated Spectrum's.

### Third early-stop rule: data playing, nothing stored
Rainbow Islands was waiting for a key at about 303 s while the rest of the tape (the 128K levels) played on to 675 s. Its key loop reads the port 640 times a frame across a wide range of code (with the interrupt routine), so neither earlier rule saw it.

- **The new rule:** every loader stores what it reads as bytes one after another, so 10 s of data bits with no such run stored means the load is over. Rainbow Islands now stops at 321 s.
- **What counts as a run:**
  - Up to 4 runs are followed at once, so a checksum or counter stored between the bytes doesn't break one.
  - A byte counts from the 3rd of a run, so 16-bit stores don't count.
  - Writes within 4 bytes of SP are left out. Speedlock CALLs its edge routine between every byte, and those stack writes pushed the loader's run out. The first version without this stopped every Speedlock tape too soon (seen, fixed, all re-checked).
- **Not used while the ROM loader is running** (PC in 0x04C2–0x0605): it reads a block with the wrong flag without storing it.
- **The safe failure:** a block copy (LDIR) also counts as a run. A title screen that animates with LDIR therefore keeps the tape playing, which is slower but never wrong.
- The PC trace (`TZXTRACE=1`, with `TZXSTEP=frames`) now shows the stored bytes and what is playing.
- `TZXCONV_DEBUG_WRITES` can be defined to log every write; it isn't in the firmware.
- All 11 real tapes and the 15 home-made tests were re-run with this rule.
- Speed with all the checks (simulated, Z80 at -O2): **1.61x** a real Spectrum at 125 MHz, against 1.65x before. The store tracking costs about 2%.

### Renegade, 48K tape (Speedlock 3)
- **Works.** It reaches the Renegade menu; pressing 1 (keyboard) starts the game, which plays.
- Spectrum time 283 s, so about 3 min on the Pico at 125 MHz.
- The .z80 decodes and its screen matches.
- With this, **every Speedlock version, 1 to 7, has a working 48K conversion.** The earlier Renegade tape is the 128K-only release, which resets a 48K machine and is reported as such.
- PC tool: `TZXKEY=row,bit` holds that key instead of SPACE and ENTER when looking at the game after the snapshot (row 3 bit 0 is "1").


## 128K conversion (prototype, 2026-10-15)

### What was added (`tzxconv.h`)
- `Machine::begin128(rom0, rom1, banks[8], ...)`: a 128K Spectrum.
  - **Memory:** eight RAM banks, paged through port 0x7FFD (bank at 0xC000, ROM select, shadow screen, lock).
  - **Sound chip:** 16 registers, written through 0xFFFD/0xBFFD and read back through 0xFFFD (128K loaders check them).
  - **Timing:** 70,908 T frames, interrupt 36 T.
  - **Starting the load:** it boots to the 128K menu and presses ENTER on "Tape Loader".
- **All memory now goes through four 16K slot pointers**, for both machines. The ROM-specific checks (error report, MAIN-4, the ROM loader's range, SAVE) only apply when the 48K BASIC ROM is paged in.
- **Saving:** the snapshot is put into snapload's 128K buffer layout (header, 7FFD, sound chip, banks in PAGE_ORDER) and written with `snapload::write_z80_128`, the same writer as a 128K save on the Picoface.
- **On the Pico:**
  - The banks fit in `snap_load_buffer` exactly.
  - The two ROMs (32 KB) have to go elsewhere: `list_pool` (32 KB) can be lent during a conversion.
  - Code: about 100 bytes more than 48K only.
  - Machine state: 548 bytes either way.
  - Speed (simulated, -O2): **1.61x** for a 128K against 1.62x for a 48K, at 125 MHz.
- PC tool: a 32 KB ROM file (`128.rom`, the editor ROM then the 48K BASIC ROM) makes it a 128K.

### A bug found on the way: the closing edge
The 128K Renegade tape ends with a data block with no pause, so the last bit had no closing edge. The loader timed out on it and reset the machine.
- **That, not a 128K check, was the reset reported earlier as "the tape may need a 128K".** That conclusion was wrong.
- **The fix:** at the end of the tape the player now gives the closing edge, then 1 ms low, as the TZX spec says.
- All 12 tapes in 48K mode and the 15 home-made tests give the same results as before.

### New failure found: SAVE
On a 128K, Rasputin's BASIC loader goes wrong and ends up in the ROM's SAVE ("Start tape, then press any key"). Reaching SA-CONTRL (0x0970, 48K BASIC ROM) after the tape has started is now "did not load" (`Status::SAVE`). Before this, it was saved as if loaded.

### All 12 tapes on both machines (128K ROMs: a standard 1986 Spectrum 128 pair for testing)

| Tape | As 48K | As 128K |
|---|---|---|
| Mikie (SL1) | works | reset after 21 s (an old loader; a real 128K needs 48 BASIC too) |
| Green Beret (SL2) | works | works |
| Renegade 48K tape (SL3) | works | works |
| Renegade 128K tape (SL3) | loads but the screen is garbage (it needs a 128K; **not found by the checks**) | **works**: title picture, then the game (pressed 1) |
| Athena (SL4) | works | works |
| Road Blasters (SL5) | works | works |
| Platoon (SL6) | works (328 s) | works, 128K version with its intro and music registers (592 s: the whole tape is loaded) |
| RoboCop (SL7) | works (259 s) | works, music registers set (526 s) |
| Dynamite Dan (Power-Load) | works | works |
| Rainbow Islands (Erbe) | works (321 s) | works, 128K version (668 s) |
| Rasputin (SoftLock) | works | "went into SAVE" after 14 s (an old loader) |
| Travel with Trashman | works | works |

Every 128K .z80 was decoded separately (8 banks, 7FFD, sound chip), and its screen matches the shown screen bank. All 21 files (11 as 128K, 10 as 48K) were also loaded with the Picoface's own `snapload::load_z80`, which takes them as KIND_PAGED (128K) and KIND_Z80V1 (48K).

### Suggested rule for the Picoface
- **On a 128K Spectrum:** convert as 128K. If that fails (reset, SAVE, tape error, stopped, BASIC error), convert again as 48K. The two failures seen came within 21 s, so the retry costs little.
- **On a 48K:** 48K only.
- **Override:** a key to force either one.
- **Limit:** a 128K-only tape converted as 48K can look "loaded" with a garbage screen (Renegade 128K). That only happens when someone forces 48K, or on a 48K machine, where such a tape can't run anyway.
- **ROM file:** a `128.rom` (32 KB: the editor ROM, then the 48K BASIC ROM) is needed, beside `48.rom`.

### Your 128K ROMs
Your `128-0.rom` and `128-1.rom` are byte-for-byte the same as the test pair (MD5 b4d26921… and 6e09e5d3…, the standard 1986 Spectrum 128). The emulation is deterministic, so every 128K result above is the result with your ROMs. On the Picoface, both forms can be accepted: one 32 KB `128.rom`, or `128-0.rom` and `128-1.rom` (as you have them).

## Step 3: the converter in the firmware (test build 2, `Rev 1.8.2 tzx`)

### How it is used
1. In Browse Games, V and the tape's number: its information. **C** on any page of a .tap or .tzx.
2. The key row asks: `Convert: A-Auto(128K) 4-48K 1-128K` (or `A-Auto(48K)` on a machine that can't page).
   - **A Auto:** a 128K if this Spectrum can page memory now; if that doesn't load it, a 48K by itself.
   - **4:** a 48K.
   - **1:** a 128K.
   - Any other key: "Not converted".
3. While it runs, the screen is the emulated Spectrum's own screen, so the loading picture appears as it loads.
   - **Top row:** `Converting (128K): <name>` (or `48K`, or `48K, 2nd go` after a failed 128K try).
   - **Bottom row:** a 12-cell bar, the percentage of the file read, the time taken, `BREAK-Stop`.
   - **BREAK or X stops it:** "Stopped: nothing was saved".
4. At the end the information page comes back with the result on the key row:
   - "Saved <name>.z80" (with "(check it)" if the loader still seemed to be reading the tape);
   - "Not loaded: <why>" (tape loading error, damaged tape, the loader stopped, BASIC error, reset (try 48K / try 128K), went to SAVE);
   - "No 48.rom" / "No 128.rom (see Help: Convert)";
   - "Cannot open the tape" or "Could not save the snapshot".
   - C on a file that isn't a tape says "C converts a tape (.tap .tzx)".
5. **Saving:** the snapshot goes beside the tape, as `<name>.z80`, or `<name> 2.z80` up to 9 if the name is taken. Nothing is overwritten.
   - The Browse list on the screen is the one from before. The new file shows when Browse is next entered: X twice, then B.
6. **ROMs, in the root of the flash or the SD card** (the flash is tried first):
   - `48.rom` (16384 bytes);
   - `128.rom` (32768 bytes: the editor ROM, then 48 BASIC), or `128-0.rom` and `128-1.rom` (16384 bytes each).
7. Help has two new pages ("Convert a tape: C", "Convert: the ROMs"). The "Tapes: 48.rom" page points to them, and the tape page of a game's information ends with "Press C to convert this .tzx or .tap into a .z80", in yellow (a line of the tape summary starting with `gameinfo::TAPE_YELLOW`, \x01, is drawn in the heading colour).

### How it works
- **ROM (nmirom.asm), 59 bytes:**
  - **New key:** `C` in the game information sends op 6 with byte 6 = 1 if the machine can page memory.
  - **The paging test:** `canpage`, the test the snapshot save already did, is now a shared routine; the save calls it too.
  - **`machine128`:** puts the paging back after the test, the 48K way (0x10) for a 48K program the Pico started, else BANK_M, as the save does.
  - **While op is 6 or 7, every key goes to the Pico (op 6, the key).**
  - **While op is 6, the ROM asks again with no key**, so the conversion goes on (7: wait for a key).
  - **Spare room:** `spare_size` 0x8DB against 0x880 needed, so 91 bytes are left.
- **Pico:**
  - **`conv_op`** (picoface.cxx) runs slices of about 120 ms per request.
  - **Overlay:** the screen is sent from the emulated screen (`pconv::screen`), and `nmi_action_screen` draws the two rows over it as the pieces go out (`conv_overlay`, 576 bytes).
  - **Memory, all borrowed:**
    - the 48K or the 8 banks in `snap_load_buffer` (the 48K ROM at +65536);
    - the 128K ROMs in `list_pool`, so the Browse list is re-read afterwards (`walk_stop`);
    - the machine (528 bytes), the tape's FileHandle and the overlay in `pok_file`, which is read again whenever the poke list opens (`pok_file.clear()` after).
  - **Afterwards:** the game information is read again (`info_loaded = false`), and its picture is gone until the tape is viewed again.
  - The tape ROM trap is stopped first (as a load does).
- **The emulation** (`tzxconv.h` + Z80) is in `tzxconv_pico.cxx`, built at -O2 behind plain functions (`tzxconv_pico.h`). `picoface.cxx` stays at -Os.
- **Size:** firmware 207,428 bytes of 253,952 (46.5 KB left). Static RAM is 836 bytes more, and the 2 KB heap is unchanged.
- **Stack:** the deepest new path (`conv_op`, then the emulator, then a tape read through littlefs) is about 1.5 KB. The NMI menu's existing deepest is 3.2 KB.
- **The updater:** the Update screen of this build and of Rev 1.8 accept each other's uf2 (checked with `fwupdate.h` on both).

### Tested in the simulator (tools/picosim): the real NMI ROM and the real firmware code
The real NMI ROM runs on an emulated Spectrum, the real `picoface.cxx` answers it, the SD card is a RAM FatFs and the flash a RAM littlefs. `tools/picosim/tests/run.sh`: all 6 scripts pass.

| Script | What it does |
|---|---|
| convert48 | 48K, Mikie on the SD card. C shows the choice, A converts it. **The file is byte-for-byte the PC tool's.** |
| convert128auto | 128K, the ROMs as 128-0/128-1 on the SD card, tapes in a folder. Auto: Renegade 128K as 128K (**byte-for-byte the PC tool's**). Mikie: the 128K try resets, then the 48K by itself (**same bytes**). Mikie again with 4: "Mikie 2.z80" |
| errors | 48K, the tape on the flash, 48.rom on the SD card: 1 with no 128K ROM ("No 128.rom"), a wrong key ("Not converted"), BREAK ("Stopped", no file), then A ("Saved Green.z80" in the flash), the list showing it after B, C on a .sna ("C converts a tape") |
| load48 | the converted Green Beret loaded from Browse: the game runs (its menu) |
| load128save | 128K, Renegade 128K converted with 1, loaded from Browse: its title runs. Then S Save: the 128K save route (the shared paging test) |
| save48 | 48K: S Save makes a 48K .sna, 49,179 bytes (the 48K route of the shared test) |

### Not tested yet, so to check on the real Picoface
- **The speed** (simulated about 1.6x a real Spectrum), and how the screen feels during the 120 ms slices: the ROM fetches the whole screen after each one.
- **The paging test (`machine128`) on a real 128K**, and that a game you go back to after it still runs. It is the save's own test, moved into a routine.
- **Memory:** the heap with a tape and a ROM on the flash, which littlefs files take from the heap. They are opened one at a time.


### Licences
- `THIRD-PARTY-NOTICES.txt` now lists the Z80 emulator and the Zeta headers (redcode, GNU LGPL v3 or later), with the full LGPL text as section 8. It also says that no ROMs or games are in the source, and names the two PC-only pieces: `pico_hal_host.c` (BSD, from `pico_hal.c`) and the benchmark's use of rp2040js (MIT, not included).
- Everything else new (the converter, its firmware glue, the ROM changes, the simulator, the tests) was written for this fork and is under the fork's GPL v3, like the rest. The new files carry the same licence header as the existing ones.

## Test build 3 (`Rev 1.8.3 tzx`)
The tape page of a game's information (V in Browse Games, then N to its last page) ends, after a blank line, with "Press C to convert this .tzx or .tap into a .z80" in yellow. It replaces the white "C converts it to a .z80 snapshot (its own loader is run)". No other change; the 6 simulator scripts still pass.

## Test build 4 (`Rev 1.8.4 tzx`): faster conversion (2026-10-16)
Two speed-ups (options 1 and 2), and the converter's own overhead cut. Nothing changes on the screen or in the menus.

### 1. Loader acceleration (exact)
- **Edge-wait loops.** A loader waits for the next edge in a loop that reads the tape port, where only B (counting up or down by one), R, the flags and the time change. After 3 identical passes (or 1, at an address where the same loop has already been proved), it jumps to just before the next edge. It sets B, R, F and the time to what the skipped passes would have left. Not while interrupts are on, not past the end of the slice, and B never reaches 0.
- **Delay loops.** `DEC A / JR NZ,$-1` and `DJNZ $` (the ROM's LD-DELAY and the DJNZ in LD-WAIT, and loaders' copies of them) are run at once, leaving the last pass for the CPU.
- **Checked exact:** with flash loading off, all 22 snapshots of the 12 tapes (48K and 128K) are **byte-for-byte the same** as without acceleration. A test build that predicted every jump and then ran every pass found 0 wrong of 4.4 million.
- **A wrong pattern was caught this way:** `DEC HL / LD A,H / OR L / JR NZ` looked like a delay loop, but in the ROM its JR goes back to the DJNZ before it. The snapshots differed, so it was taken out.

### 2. Flash loading of standard-speed blocks
- When the ROM's loader is at LD-START (0x056C) and the next thing on the tape is a standard-speed block, the block is loaded at once, as Fuse does. That covers the block playing (if enough of its pilot is left for the ROM to find it) or the next one after a pause; it never goes past a "stop the tape" block.
- The registers are left as the ROM leaves them: B, C, DE, H (parity), L, IX, A' and F'. The CPU then runs `LD A,H / CP 1 / RET` at 0x05DF, so a bad checksum still gives "R Tape loading error" (the `bad_rom_checksum` test).
- **Not flash-loaded** (played edge by edge as before): VERIFY; a block with the wrong flag, too short, or with no bytes asked for; non-standard timings (turbo blocks); a loader's own copy of the ROM routine.
- Only when the 48K BASIC ROM is paged in, and only if its LD-BYTES code (0x0556–0x0604) matches the standard one by a hash. Your 48.rom and 128-1.rom both do.
- **Not exact, by design:** the load takes no Spectrum time, so the snapshot is taken at another moment of the same game. Checked by the game's screens: 20 conversions, 4 screens each with and without flash loading. 74 of the 80 pairs are pixel-for-pixel the same; the other 6 are the same screens at a different point of an animation (Mikie's intro, Dynamite Dan's game screen, Trashman's map).

### 3. Less overhead per instruction (exact)
Profiling on the simulated Cortex-M0+ (PC sampling added to `bench-run.ts`) showed the converter's own code, not the Z80 emulation, was taking much of the time:
- `fetch_opcode`, called for every Z80 instruction, was a third of all cycles. It now does one memory lookup and a few compares; the reset, RST 8, SAVE and flash-load checks have moved to an out-of-line `trap()`. This alone made the plain emulator 10% faster.
- `accelerate`, called on every tape read, tests the cheap things (PC, port, B step, time) before comparing the other registers, and uses 32-bit divides.
- `in` skips the keyboard rows when no key is down.
All exact: the snapshots are byte-for-byte the same before and after.

### Speed
**Green Beret, 48K, measured on the simulated Pico at 125 MHz:**

| Build | Z80 instructions | Time |
|---|---|---|
| Test build 3 (no speed-ups) | 115.7 M | 184 s |
| Less overhead only | 115.7 M | 165 s |
| + loader acceleration | 56.2 M | 111 s |
| **+ flash loading (this build)** | 52.5 M | **104 s** (about 2x a real Spectrum) |

**All 12 tapes:** the times are estimated from the instruction counts at Green Beret's measured rate: test build 3 at 1.59 s per million instructions, this build at 1.99 s, the extra being the acceleration's bookkeeping.

| Tape | Machine | Test build 3 | This build | Faster |
|---|---|---|---|---|
| Athena (Speedlock 4) | 48K / 128K | 409 s | 281 s | 1.5x |
| Dynamite Dan (Power-Load) | 48K / 128K | 137 s | 116 s | 1.2x |
| Green Beret (Speedlock 2) | 48K / 128K | 184 s | 104 s | 1.8x |
| Mikie (Speedlock 1) | 48K | 155 s | 101 s | 1.5x |
| Platoon (Speedlock 6) | 48K | 265 s | 181 s | 1.5x |
| Platoon (Speedlock 6) | 128K | 470 s | 356 s | 1.3x |
| Rainbow Islands (Erbe) | 48K | 257 s | **43 s** | 5.9x |
| Rainbow Islands (Erbe) | 128K | 541 s | 177 s | 3.1x |
| Rasputin (Firebird SoftLock) | 48K | 212 s | 134 s | 1.6x |
| Renegade 128K (Speedlock 3) | 48K / 128K | 245 s | 159 s | 1.5x |
| Renegade 48K-128K (Speedlock 3) | 48K / 128K | 221 s | 130 s | 1.7x |
| Road Blasters (Speedlock 5) | 48K / 128K | 261 s | 139 s | 1.9x |
| RoboCop (Speedlock 7) | 48K | 200 s | 132 s | 1.5x |
| RoboCop (Speedlock 7) | 128K | 434 s | 326 s | 1.3x |
| Travel with Trashman | 48K / 128K | 145 s | 130 s | 1.1x |

- Tapes that are mostly at standard speed gain the most (Rainbow Islands). Turbo loaders gain from the acceleration only.
- Dynamite Dan and Trashman gain little. Their loaders' wait loops probably don't fit the pattern (B not counting by one, or other registers changing each pass); not looked into yet.
- Overclocking to 250 MHz would roughly halve every time; that isn't done.

### Size and stack
- Firmware 212,140 bytes of 253,952 (41.8 KB left), 4.7 KB more than test build 2.
- Stack: flash loading adds about 300 bytes to the converter's deepest path (`flash_load` 136 B, then `Player::flash_ready` 240 B, which keeps a copy of the player's state to put back if the next block isn't one to flash-load). That makes about 1.8 KB, still under the NMI menu's 3.2 KB.

### Tests
- The 15 made-up tapes pass, with and without flash loading (`TZXNOFLASH=1`).
- The 6 simulator scripts pass (the real NMI ROM and firmware, converting Mikie, Green Beret and Renegade 128K, then loading the results).
- 22 conversions of the 12 real tapes: all still load. They are byte-for-byte the same as before with flash loading off.

### PC tool switches
`TZXNOACCEL=1` and `TZXNOFLASH=1` turn each speed-up off, and the tool prints the instructions run, the loop passes skipped and the blocks flash-loaded. See `tools/tzxconv/README.md`.

### To check on the real Picoface
- **The time on a real Picoface for a tape you know.** The simulation counts the CPU and the flash cache, but not SD card reads, USB or the NMI menu's screen fetches between slices. Green Beret should take about 1¾ minutes.
- That a converted game still runs, especially a tape with standard-speed blocks (Rainbow Islands is the best test, the most of it flash-loaded).

## Test build 5 (`Rev 1.8.5 tzx`): Where Time Stood Still (2026-10-16)
**The fault (found on your Picoface):** Where Time Stood Still (Ocean, 128K, Speedlock 4) converted to a .z80 that only showed the loading screen.

**Why:** the converter stopped at 62.7 s, 10 s into block 14 (22,183 bytes, about 115 s of tape). The 128K loader reads that block but throws it away: every byte goes to address 0, which is ROM. So the converter's third early-stop rule ("data playing, the port read, but nothing stored for 10 s": made for a game waiting for a key with the tape still running) decided the load was over.

**The fix (`tzxconv.h`, `write`):** a store into the ROM (below 0x4000) now counts as loading. A loader skipping a block keeps doing that; a game waiting for a key doesn't. The runs of stores are followed exactly as before.
- A first try returned early for those stores, so they no longer took one of the four run trackers. That changed what Rainbow Islands' game loop counted (it writes the same few addresses over and over), and its 48K conversion played the whole 441 s tape instead of stopping at 87 s. So that path is unchanged now.

**Checked:**
- Where Time Stood Still, 128K: loads to the game's CONTROL SELECTION menu. 640.6 s of tape, 189 million instructions: about **6¼ minutes on the Pico** (estimated as in test build 4). The .z80 is the same, byte for byte, as with the early stop turned off (`TZXFULL`).
- **In the simulator, with the real firmware on a 128K and Auto:** "Saved WTSS.z80", 105,925 bytes, the same bytes as the PC tool.
- The other 12 tapes: all 22 snapshots byte-for-byte the same as test build 4. The 15 made-up tapes and the 6 simulator scripts pass.
- On a 48K (key 4), it also gets to the menu, but this is a 128K-only game: use Auto or 1 on a 128K.

Firmware 212,236 bytes.

## Test build 6 (`Rev 1.8.6 tzx`): screens saved while converting (2026-10-16)
### What it does
- **By itself:** once the tape's loading screen has loaded, it is saved, and the top row shows "Loading screen saved: <name>" (black on green) for 3 seconds.
- **S, at any time while converting:** saves the screen as it is then; "Screen saved: <name>".
- **Where:** the SD card, `/Scraper/Screen Grabs`, as `<name>.scr`, or the next free `<name>1.scr`, `<name>2.scr` ... (up to 39), so a screen the scraper downloaded is never overwritten. `<name>` is the tape's name, so V shows them with the tape's and the snapshot's information. The folders are made at the start of a conversion if they aren't there.
- **Problems** show on the top row in red: "No SD card: screen not saved", "Screen not saved: too many", "Screen not saved (SD card)".
- Screens are saved even if the conversion then fails or is stopped. In Auto, if the 128K attempt fails after its loading screen was saved, the 48K attempt doesn't save it again.
- **The status row** is now `<bar> 1:23 S-Screen BREAK-Stop`. The percentage has gone; the bar shows it (8 cells, 7 from 10 minutes).
- **Help:** a new page, "Convert: screens", and a line on "Convert a tape: C". It also now says a conversion takes about half the tape's time.

### How the loading screen is found (`tzxconv.h`)
- The converter counts stores of tape data into the screen: while data bits are playing, or while a block is flash-loaded.
- When they cover it (at least 5,800 of the 6,144 picture bytes and 700 of the 768 colour bytes), it waits for a store of tape data somewhere else. The loader has gone on, so the screen is done, and it is saved.
- **Why not the whole 6,144:** Speedlock leaves a few bytes alone (Green Beret about 6,130, Where Time Stood Still about 6,100).
- **A screen all one colour doesn't count**, and the count starts again: a CLS, or a loader clearing the screen.
- **Only the first screen is saved.** Watching for later ones (tried) found only junk: Road Blasters' cleared screen, then the start of the game screen.
- `run()` comes back after the frame it was seen in, and the firmware saves it then. That doesn't change the emulation: all 22 snapshots of the 12 tapes are byte-for-byte the same as test build 5.

### What it found on your tapes
All 13 tapes, on 48K and 128K: every conversion that loads saves a loading screen. Checked by eye: Athena, Dynamite Dan, Green Beret, Mikie, Platoon, Rasputin, Renegade (both), Road Blasters, RoboCop, Travel with Trashman and Where Time Stood Still all give their title pictures.
- **Speedlock screens have the loader's countdown clock** in a corner (e.g. "2m22s1" on Road Blasters): it is on the screen as it loads.
- **Rainbow Islands** gives Erbe's "No juegues con tu vida" screen: the only full screen on that tape. The game's title is drawn by the game.

### The result message after a conversion
With screens saved, the information page now has pictures, which change by themselves every few seconds, and each change redrew the page without "Saved <name>.z80". The result now stays on the key row while the pictures change, until a key is pressed. This would also have happened before, for a tape with a scraper screen.

### Firmware details
- The tape's file handle is used to write the .scr: the tape is closed, the screen written, and the tape opened again where it was. No extra RAM.
- Making the folders borrows `list_pool`, which holds the 128K ROMs during a conversion, so it's done before they're read.
- **Stack:** processNmiRequest 280 + conv_op 272 + make_sd_folder 16 bytes before `f_mkdir`, against 280 + 800 + 16 for the existing screen grab (G), so it is no deeper.
- Firmware 214,284 bytes (39.7 KB left).

### Tests
- **Simulator, two new scripts** (8 in all, all pass):
  - `convertscr48`: a `Green.scr` is already there. S saves `Green1.scr` ("Screen saved: Green1" seen), the loading screen `Green2.scr`, the snapshot `Green.z80`.
  - `convertscr128`: no `/Scraper` folder; it is made, and `Ren128.scr` saved.
  - Both screens are the same bytes as the PC tool's.
- **Simulator changes:** paths with spaces in `sd`, `get` and `ls`, and `slice N` (the converter runs N frames per request instead of 120 ms, so a script can press S at a known point).
- The 15 made-up tapes pass.

### To check on the real Picoface
- The message appears as the loading screen finishes, and the .scr files show in V.
- S during a conversion. The SD card write takes a moment, so the conversion may pause briefly.

## Test build 7 (`Rev 1.8.7 tzx`): BREAK keeps only the S screens (2026-10-16)
**Found on your Picoface:** S, then BREAK, left two .scr files: the S one and the loading screen saved by itself.
- **Now:** BREAK (or X) removes the loading screen the conversion saved by itself, and keeps the ones saved with S. The key row says "Stopped: only S screens kept", or "Stopped: nothing was saved" if S wasn't used.
- **A conversion that completes** keeps both.
- **One that fails** by itself (tape loading error, reset, ...) keeps both too, as before.
- The numbers of the kept files don't move up, so a gap can be left (e.g. Green.scr and Green2.scr). V shows them all.
- **New simulator script `convertscrbreak`:** S saves Green.scr, the loading screen Green1.scr, then X: Green1.scr is removed and Green.scr kept, with no .z80. All 9 scripts pass.
- Help, "Convert: screens": "BREAK keeps only the S ones."
