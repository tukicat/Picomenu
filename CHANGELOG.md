# Changelog

Changes on top of the original picoFace, in the order they were made. Base: `picoface-tapeloader`. This replaces the separate `.patch` files.

## Rev 2.1.0 (2026-10-25)
Changes since Rev 2.0. `FIRMWARE_ID` is `Rev 2.1.0 (tapeloader v210)`: Diagnostics shows "Rev 2.1.0".

### Virtual Tape Player: labels
- **L labels a line of the tape:** L, the line's number (1-0 on the page shown), then up to 16 letters; ENTER saves, BREAK goes back, and saving an empty label removes it. A labelled line shows its label in yellow in place of the kind of block (Bytes: partA, Data, flag 88 ...), so the blocks of a multi-load game can be marked with their levels.
- The labels are kept in `/Tape Files/<the tape's path, / as ~>.labels` on the tape's own drive (lines of `<first data block> <label>`), by the block a line starts at, so they stay right whatever the tape's position. They follow the tape, and its folder, when it is renamed, moved or put in the Waste Bin, and go when it is erased for good or the Waste Bin is emptied.
- The help lines on the screen list L; the key row reads `1-0 L-Label E-Eject N P X-Exit` (R - Rewind is in the help lines).

### Save snapshot: overwriting a name of another type
- **A name already used by a snapshot of any type asks first.** Saving Commando as a `.sna` while `Commando.z80` is in the folder used to make `Commando.sna` beside it; now it asks "A file with this name exists / Overwrite this file?". N goes back to the Save snapshot screen with the name, Y saves `Commando.sna` and removes `Commando.z80` (and its tape note), so there is one save of that name.
- The same goes for Q - Quick save: `<game> Save Game` of any type counts as there (the question, `quick_save = overwrite`, or the next number free in all three types). Save slots are as before.
- On the internal flash names are compared as typed (littlefs tells capitals apart); on the SD card capitals don't matter.

### Licences
- No new third-party code: the tape labels and the save question are PicoMenu's own code (GPL v3), in files that already carry its header. THIRD-PARTY-NOTICES.txt is retitled for Rev 2.1.0.

### Tests
- Simulator: `tapelabels48` (label two lines, BREAK while typing, read again when the screen opens, an empty label removes one, the folder renamed: the labels go with it), `savename48` (commando.z80 there: the question, N, then Y; a new name saves at once), and the `DELETE` key. 55 scripts, all passing.

## Rev 2.0 (2026-10-23)
Everything since Rev 1.9. `FIRMWARE_ID` is `Rev 2.0 (tapeloader v200)`: Diagnostics shows "Rev 2.0".

### Tapes and multi-load games
- **48K multi-load games** (a `.tap` or `.tzx` loaded with its number in Browse Games) load their later levels at once through the tape trap. Each level is found by name or in order, as the game asks, when the game uses the ROM's tape loader. Out Run, Gauntlet and Gauntlet II are tested.
  - The trap's hook now starts at 0x0562, after LD-BYTES' first 12 bytes, so loaders that do that part themselves land on it. The source is `tools/picosim/tests/tapehook.asm`.
  - The end of the tape rewinds it, so a search by name goes round.
  - A tape whose blocks load over each other (Gauntlet's levels) is no longer converted at once.
- **Games with their own level loader.** Some games load their levels with their own copy of the ROM's LD-BYTES in RAM; Out Run's Speedlock releases (48K and 128K) do. Before, they stayed at PRESS PLAY, because the Pico only sees fetches from the ROM. PicoMenu now finds such a copy and makes it load from the tape file, with no poke.
  - **How the copy is found:** by how LD-BYTES starts reading the tape (`LD HL,<return> / PUSH HL / IN A,(FE) / RRA / AND 20`), with its own LD-EDGE within 256 bytes.
  - **The patch:** its `IN A,(FE) / RRA` becomes `JP 0x0562`, where the trap's hook is. The game has already set the flag, IX and DE and pushed its own return, so the hook loads the block and returns there as LD-BYTES would.
  - **When it looks:**
    - in each block the trap loads;
    - just after a tape is put in with T;
    - when a snapshot carries on with its tape.

    For the last two, the Pico reads the Spectrum's memory and writes the 3 bytes back.
  - **When it doesn't:** a 128K game known to have the editor ROM paged is left alone, because 0x0562 is not LD-BYTES there.
  - **Without a tape in,** the patched loader runs the ROM's own LD-BYTES, so a real cassette then loads at the normal speed, not the game's.
  - **Setting:** `level_loaders` (`patch`, the default, or `off`) in `[Tapes]`.
  - **Out Run, Speedlock 3:** convert the tape with C (4 - 48K on a 128), load the `.z80` and press 1. The stages load from the full tape.
  - **Out Run, Speedlock 4 (128K):** load its `.z80`, put the stage tape in with T and press 1.
- **48K mode on a 128K Spectrum.** These now run with the 128 locked in 48K mode (port 0x7FFD = 0x30, as 48 BASIC does):
  - a 48K snapshot that carries on with its tape;
  - a tape loaded with its number.

  Unlocked, a 48K/128K game found the 128's memory and played as the 128K version; Out Run then took its 48K stage for a 128K one and crashed.
  - **Setting:** `lock_48k` in `[Tapes]`: `tapes` (the default), `always` (every 48K snapshot) or `off`.
  - **While locked,** a long press resets into 48 BASIC. The Spectrum's own reset button gets back to the 128's menu.
- **T - Tape in Browse Games** (then a tape's number) puts a tape in without a reset, for loading through the Spectrum's own ROMs as from a cassette:
  - the 128's Tape Loader;
  - `LOAD ""` in 128 or 48 BASIC;
  - a game already running.

  Each block loads at once, so 128K tapes and 128K multi-loads that use the ROM's loader work.
  - Resets into 128K or 48K (1, 4) and the long press keep the tape in. E ejects it, so a real tape in the EAR socket works again.
  - It doesn't work with a changed ROM.
  - **How:** the bus handler (`piohandler.S`) watches for the instruction fetch at 0x0562 in LD-BYTES, only while a tape is in and the internal ROM is serving. It then serves the trap's hook from `rom_data` and hands back at the hook's RET.
- **V - Virtual Tape Player** on the NMI menu shows the tape's blocks ten to a page, with a header and its data on one line. The next load is marked `>`, and the blocks already loaded are dim.
  - 1-0 moves the tape to a block, R rewinds, E ejects (its file is closed, so it can be erased or moved), and N/P turn pages.
  - The Pico draws the screen.
- **Recording:** W on the Virtual Tape Player asks for a name. Every SAVE after that goes at once onto the end of `/Recordings/<name>.tap` on the SD card: BASIC, CODE, SCREEN$, DATA, or a program calling SA-BYTES.
  - E stops it ("Saved <name>.tap"), and T puts it in to load back.
  - A name already used is refused, so nothing is recorded over.
  - SD card only, and not with a changed ROM. A game's own (turbo) saver can't be caught.
  - The hook is at 0x04C2 in SA-BYTES; its source is `tools/picosim/tests/savehook.asm`.
- **The tape is ejected by itself** before it, or a folder it is in, is erased, moved or renamed.
- **Snapshots remember the tape position.** A note of the tape's drive, path and block is kept with:
  - a snapshot saved with S or Q while a tape plays;
  - a `.z80` made with C that stopped with more still on the tape.

  Loading the snapshot puts the tape back in at that block.
  - **Where the notes live:** in `/Tape Files` in the root of the snapshot's drive, named after the snapshot's path with `/` as `~`, and ending `snap <path>` so a note is only used for its own snapshot.
  - **Following the snapshot:** a note goes with its snapshot, and with the snapshot's folder, when it is erased, renamed, moved, copied or added to Favourites.
  - **Following the tape:** it is rewritten when its tape, or the tape's folder, is renamed, moved or put in the Waste Bin.
  - **Older notes:** a note an earlier build left beside its snapshot is moved into `/Tape Files` the first time it is used.
  - **Missing tape:** if the tape has gone, Diagnostics says "Tape not found".

### Converting
- **Batch convert** (Tools, C) converts all the `.tap` and `.tzx` files of a folder on the SD card, as C - Convert does with A - Auto.
  - **Choosing the folder:** it opens on the folder Browse Games was last in. 1-0 opens a folder, U goes up, N/P turn pages.
  - **Output:** each `.z80` goes in a `Converted` folder made inside the chosen folder. A tape already converted is skipped, so a batch stopped with BREAK carries on when it is run again.
  - **Report:** `Converted/Batch convert.txt` gets a line for each tape and the totals, and a summary is shown at the end.
- **Snapshots to tapes:** T on the Batch convert screen turns each `.z80` (48K or 128K) in the folder into a `.tap` in a `Tapes` folder made there. The tape loads on a real Spectrum at the normal speed (LOAD "", or the 128's Tape Loader).
  - **The tape holds, in order:**
    1. a BASIC line;
    2. a 1 KB loader;
    3. the screen;
    4. the rest of the memory (a 128K game bank by bank);
    5. the loader's own 1 KB of the game.

    Runs of 512 or more of one byte are filled in rather than loaded.
  - **The loader** keeps interrupts off throughout. A 90-byte stub just below the game's stack pointer sets every register, the paging and the sound chip.
  - **Load times:** a 48K game takes about 4-5 minutes to load, a 128K one up to 12.
  - **Proof:** checked on the PC through the ROM's own loader (`tools/z80tap/roundtrip.py`).
- **C - Convert is about 3½ to 5 times faster** on turbo tapes, with exactly the same result.
  - **How:** the converter recognises a loader's copy of the ROM's edge routine and works out from the tape what it would do. Every register, flag and T-state comes out as if each instruction had run.
  - **Measured:** Green Beret 104 s → 25 s, Athena 281 s → 79 s, Platoon (128K) 356 s → 74 s.
- **720 Degrees and Football Director II convert completely.** The converter holds the tape in a pause while nothing reads it, and counts a loader-sized loop's scattered stores as loading. Football Director II needs its complete `.tap` (88 blocks) and a 128K.
- **Loading screens** saved by a conversion are Loading Screen grabs. The `loading_grab` setting in `[Converting]` turns the automatic one off.

### Screen grabs
- **All grabs go in `/Scraper/Screen Grabs`** on the SD card. Grabs left in the old `/Screen Grabs` still show in V.
- **Categories, in the name:**
  - `<game> Loading Screen.scr`, `<game> Menu Screen.scr`, `<game> Game Screen.scr`, `<game> Ending Screen.scr`, each then `...1.scr`, `...2.scr` (the lowest number free);
  - Standard Screen Number, the older `<game>.scr`, `<game>1.scr`;
  - From File, the screen in the game's own file.
- **G - Screen grab** asks the category: 1 Loading, 2 Menu, 3 Game, 4 Ending, 5 Standard Screen Number. A double press of the NMI button saves as Standard Screen Number.
- **The order** V shows a game's pictures in is set by `[Screen grab order]` in Picomenu.ini, or by `/Scraper/Screen Grabs/order.txt`. A category left out or commented out is not shown.
- **V** shows up to 15 pictures for a game (it was 8), and finds them by reading the folder once.
- **`.png` grabs:** `grab_format` (`scr`, `png` or `both`) in `[Saving]`. A `.png` is the screen as a 256x192 picture in the 16 Spectrum colours.
  - V shows a grab saved as both formats once, and reads PicoMenu's own `.png` files back.
  - D - Del deletes both files.
- **Menu grabs:** a double press while a menu is up saves that menu's screen as a `.png` in `/Scraper/Screen Grabs`, named after the menu (`NMI Menu.png`, `Browse Games.png`, `Diagnostics.png`, ...).
  - The border flashes green when it is saved, red when it is not.
  - Outside the menu, a double press still grabs the game's screen.
- **I - Game Instructions** shows the game's screen grabs, as V does. A game with no grabs shows its screen as it was when the button was pressed.

### Browse Games
- **.txt files** are listed with L-All. A .txt's number shows it a page at a time (N/P), and a file of any size can be read. On the SD card, E edits it: up to 3 KB, lines of up to 30 letters; BREAK, then Y saves.
- **H - Hide/Unhide** hides a file or folder or shows it again. On the SD card it uses the FAT hidden attribute (as Windows sets it); on the flash, a littlefs attribute.
  - L-All lists hidden files and folders dimmed.
  - Some are always hidden: `.scr` and `.txt` files, the Poke List, Tape Files and old Screen Grabs folders, system folders and names starting with a dot.
- **The Waste Bin:** E moves a file or folder into `/Waste Bin`, a hidden folder on the SD card. On the card this is a rename, so even a big folder goes at once. A folder on the flash is still erased for good.
  - W - Waste Bin (in Tools) opens Browse Games in the bin, where erasing is for good.
  - B - Empty asks first.
  - `waste_bin = off` in `[Waste Bin]` erases for good everywhere.
- **V - View is only for games** (`.tap`, `.tzx`, `.sna`, `.z80`, `.szx`).

### The NMI menu, settings and saves
- **T - Tools** holds the things used now and then:
  - T - Test ROM (diagnostics);
  - D - Set up SD card;
  - U - Update PicoMenu;
  - W - Waste Bin;
  - C - Batch convert.

  The NMI menu is shorter, and B - Browse Games is now at the top of it (then S, Q, R and the rest as before).
- **H - Help is gone from the NMI menu; O - Options takes its place.** The full user guide is at github.com/tukicat/Picomenu.
- **`Picomenu.ini`** in the root of the SD card (or, if it has none, the flash) sets how PicoMenu starts. It uses `[sections]`, `key = value` lines and `#` for a note. A line it doesn't understand is skipped, and Diagnostics (D on O - Options) shows the first one.
  - **Where it can be changed:** with L-All on, its number shows it and E edits it on the Spectrum. It is used as soon as it is saved, or when the PC gives the card back.
  - **The settings:**
    - `[Display]` `theme`;
    - `[Browse]` `show_all`, `details`, `sort`, `folders`, `start_drive`, `start_folder`;
    - `[Saving]` `save_type`, `quick_save`, `save_screen`, `slots_on_load`, `grab_format`;
    - `[Buttons]` `long_press`, `double_press`;
    - `[Converting]` `loading_grab`;
    - `[Waste Bin]` `waste_bin`;
    - `[Tapes]` `level_loaders`, `lock_48k`;
    - `[Screen grab order]`.
- **O - Options** on the NMI menu changes Picomenu.ini on the Spectrum. Each setting is on a line: Q/A move the bar and C changes the value. N shows the screen grab order, where U/D move a category and H hides it.
  - **D shows the Diagnostics page:** the firmware version, the internal flash, the SD card (and any trouble put right), the last error, and where Picomenu.ini was read from with the first line it didn't understand. Any key goes back to the settings.
  - Leaving after a change asks whether to save.
  - Only the changed lines are written; comments and the rest of the file stay as they were.
- **Light theme:** `theme = light` gives dark ink on white paper, in the ROM's menus and the Pico's own screens. Game pictures are never changed.
- **Save slots:** S shows nine save slots for the game being played, in `/Save Games/<game>`.
  - **Keys:** 1-9 saves, L labels, V shows a slot's screen, E erases, and S goes on to the Save snapshot screen.
  - **Q** saves over the slot used last.
  - **Loading a game with slots asks** whether to carry on from one: 1-9 carries on from that slot, 0 (or ENTER) starts the game itself, **V then a slot's number shows that slot's screen** (any key goes back to the question), BREAK goes back to Browse Games.
- **Set up SD card** makes `Scraper/Screen Grabs`, `Tape Files`, `Recordings`, `Waste Bin` (hidden), `ROMS` (hidden) and `Picomenu.ini` (if there is none). It no longer makes `/Screen Grabs`.
- **A 128K game's sound stops while the NMI menu is up.** The menu turns the sound chip's three volumes down and puts them back as it closes. A snapshot saved from the menu keeps the game's volumes.

### ROMs
- **A `ROMS` folder on the SD card** can hold these ROM files, which are looked for in the root of the flash, then in `/ROMS`, then in the card's root:
  - `48.rom` and `128.rom` (or `128-0.rom` and `128-1.rom`) for converting and tape loading;
  - `testrom.rom` and `DiagROM.rom` for T - Test ROM.
- **C - Change ROM** lists the `.rom` files of the flash's root. 2 switches to the card's `ROMS` folder, and 1 goes back to the internal ROM. The lists are in order (A-Z).

### Fixes
- **The SD card could go "empty" until switched off and on.**
  - A failed read or write is now tried twice more, then the card is started afresh.
  - A folder that can't be read is closed again.
  - Each time the menu opens the card is checked, and mounted again if anything is wrong.
  - Diagnostics shows the retries, failures and remounts.
- **Copying a folder froze the Pico.** FatFs now takes at most 2 KB from the heap, and the memory lent to it is 8-aligned. The same problem could stop New folder, Set up SD card and the first screen grab on some cards.
- **A search could run until BREAK** while the file details were shown, and the list was garbled while it ran. Each file now shows "..." for its details until the search is over.
- **Going back from V - View, Help, a text file or other full screens** no longer shows the old screen (the 128K menu or a game) for a moment.
- **A key could be read wrongly** when two keys went down together. The menu now takes a key only when two keyboard scans agree.
- **A long software house name** no longer shows over a full-screen picture.
- **"Press any key"** no longer stays on the screen after emptying the Waste Bin.
- **The firmware was too big to install** at one point during testing. The NMI ROM and the Help text are now kept packed (LZSS) and unpacked when needed, and the build fails if the program would not fit.

### NMI ROM and firmware space
- **NMI ROM space** came from:
  - smaller transfer buffers (`spare_space` fixed at 0x3900);
  - one shared wait-for-the-Pico routine (`waitpico`);
  - prompts without padding spaces, and repeated text stored once;
  - the Pico drawing more of the screens (`picoscreen`);
  - `cp 0` written as `or a`;
  - `jr` where `jp` was not needed.

  9 bytes are left.
- **Firmware space** came from:
  - the NMI ROM and the Help text kept packed;
  - no floating-point printf;
  - the debug messages left out (`cmake -DPICOMENU_LOG=1` puts them back);
  - a small sort in place of qsort;
  - the Pico drawing with the NMI ROM's font.

  The program is 245.5 KB of the 248 KB before the flash's file system (2.5 KB left). The build fails with less than 5 KB of heap, with `list_pool` not 8-aligned, or with a program too big for the flash.

### Help, licences and tests
- **Help:** the on-screen help pages are removed; the full user guide is at github.com/tukicat/Picomenu. `Docs/Help.txt` now holds only the Diagnostics page, which `pico/make_help.py` packs into the firmware. Messages that pointed to Help now say where to look ("No 48.rom (flash or SD /ROMS)", "Saved: see Diagnostics (O, D)").
- **Licences:** the GPL v3 and the components' licences are in LICENSE and THIRD-PARTY-NOTICES.txt (the Help Licences page went with the rest of Help).
- **Third-party code:** none new was added. THIRD-PARTY-NOTICES.txt:
  - names PicoMenu's own new parts and the PC scripts' dependencies;
  - explains `pico/lwipopts.h`;
  - says how to share the firmware (with LICENSE, this file and the source).

  Commercial tapes named by simulator scripts are not included. The level-loader test uses a made-up game (`make_levelloader.py`) with no ROM code in it.
- **Simulator** (`tools/picosim`): 53 scripts, all passing. It models the tape trap, tape in, recording, double presses, the Kempston joystick, the 128's sound chip and SD card faults.

## Rev 1.9 (2026-10-16)
New: convert a `.tap` or `.tzx` to a `.z80` snapshot on the Pico, for turbo and protected loaders too. Development notes: `Docs/Tape-converter-notes.md`.
- **C on a tape's information** (V in Browse Games) asks `Convert: A-Auto 4-48K 1-128K`. A (Auto) converts as a 128K if this Spectrum can page memory, and as a 48K if that doesn't load it; 4 and 1 choose. The tape is played into an emulated Spectrum (the redcode Z80 emulator) running the tape's own loader, from `LOAD ""` (48K) or the 128K menu's Tape Loader, and the memory and registers are saved beside the tape as `<name>.z80` (`<name> 2.z80` ... if taken). While it runs the screen is the emulated Spectrum's, with a title row and `<bar> 1:23 S-Screen BREAK-Stop`; BREAK or X stops it.
- **ROMs** (not included): `48.rom`, and `128.rom` (32K) or `128-0.rom` and `128-1.rom`, in the root of the flash or the SD card.
- **When it is done:** the tape's data is no longer being loaded, or the tape has ended and the game has run 2 s. It reports a tape loading error, a damaged tape, a loader that stopped, a BASIC error, a reset (the tape needs the other machine) or a loader that went into SAVE. A conversion that may still be waiting for the tape says "(check it)".
- **Speed:** a loader's wait-for-an-edge and delay loops are skipped exactly (the snapshot is the same byte for byte), and standard-speed blocks loaded by the ROM are flash-loaded, as Fuse does. A conversion takes about half the tape's playing time (Green Beret about 1¾ minutes, estimated).
- **Screens:** the tape's loading screen is saved by itself once it has loaded, and S saves the screen at any time, in `/Scraper/Screen Grabs` on the SD card as `<name>.scr` or the next free number (never over a scraper's screen). The top row says "Loading screen saved" or "Screen saved" for 3 s. BREAK keeps only the ones saved with S.
- The game information keeps the result on the key row while its pictures change, until a key is pressed.
- **Tested:** 13 commercial tapes (Speedlock 1-7, Power-Load, Firebird SoftLock, Erbe, New Generation, Ocean 128K), on 48K and 128K, and 15 made-up tapes of every block type; the firmware and the real NMI ROM in a PC simulator (`tools/picosim`).
- **Licences:** the Z80 emulator and Zeta headers are GNU LGPL v3 (`pico/z80`, full text in THIRD-PARTY-NOTICES.txt section 8). The new PC tools and tests carry the project's GPL v3 header.
- **Build:** `picoface.cxx` is built with `-Os` (it was `-O3`) to make room; the converter and Z80.c with `-O2`. Firmware 214 KB of 248 KB.

Fixes:
- **A long software house scrolls** on the game information (it ran off the screen): the ROM scrolls it in its 15 letters, as the Reload item does on the menu. The Pico writes the whole name at `INFO_HOUSE` in the ROM's spare space when the first page shows; the menu's scroll code is shared (about 60 bytes of ROM).
- **The Convert prompt fits the key row** (it was `Convert: A-Auto(128K) 4-48K 1-128K`), and so do "Not loaded: SAVE (try 48K)" and "Saved <name>.z80" with a long name.
- **Browse Games reads the folder again** when you go back from a game's information, so a new `.z80` shows at once.

New in Browse Games:
- **L-All** lists what is normally left out: folders that are hidden (on Windows) or system or start with `.`, the `Poke List` and `Screen Grabs` folders in the root of the card, and `.scr` files, so they can be moved, copied, renamed and erased. While it is on, `L-All` is white on blue, as the drive being browsed is. L again hides them. It stays on until power off. ROM: `.allhandler` toggles `.showall` (4), which is ORed into the list request's flags; the key rows colour it like the drive. Pico: `list_all` changes `is_listable_dir` (never `.` or `..`), the root folder names, and `is_listed_ext` (`.scr`), and reads the folder again when it changes.
- **A .scr is not loaded:** a number (or V then ENTER) on a `.scr` says "A screen picture", as `.rom` and `.pok` say what they are for. V on a `.scr` shows the picture. The three checks share one table and routine (`.notgames`, `.isext`), and V's ENTER now goes through them too (before, V then ENTER on a `.rom` or `.pok` tried to load it). ROM `spare_size` 0x880, exactly what the buffers need.

Help and docs:
- **Help rewritten** as a user guide (`Docs/Help.txt`, 21 pages): the NMI button, setting up the SD card, converting tapes and the loading screens, the ROMs, diagnostic and other ROMs, quick save, hidden folders, search, file types, editing game details and notes, pokes, the SD card on a PC, and Diagnostics (still the last page, which D goes to).
- `Docs/Instructions.txt` (the long development notes) is removed: what changed is in this file, and how to use it is in Help. `Docs/Instructions-example.txt` (an example Instructions.txt) stays.

Licences:
- **New font: Unscii 8** by Viznut, public domain / CC0, for the menu and Help screens. It replaces the Carton font (Damien Guard's ZX Origins), whose terms are informal rather than a licence and forbid redistributing the font files; `nmirom/Carton.zip` (its download, in the source since before 1.8) is removed. `tools/make_font.py` makes the ROM's font from `unscii-8.hex`; the Help font comes from the ROM's as before.
- `pico/sd_config.c` has its Apache 2.0 notice back (it is the SD driver's example `hw_config.c`, changed for the Picoface). `pico/usb_descriptors.c` and `pico/tusb_config.h` carry the MIT notices of the Pico SDK files they are based on (Raspberry Pi, Damien P. George).
- THIRD-PARTY-NOTICES.txt lists all of these (sections 9 and 10), and `pico/pico_sdk_import.cmake` (a copy of an SDK file, BSD 3-clause).
- The fork's own files that had no header now have the GPL v3 one: `extras.h`, `fsops.h`, `fwupdate.h`, `gameinfo.h`, `notes.h`, `screengrab.h`, `sdsetup.h`, the `usb_msc` files, `make_help.py` (and the `helptext.h` it writes) and `scripts/upload.py`.

`FIRMWARE_ID` is `Rev 1.9 (tapeloader v109)`.

## Rev 1.8 (2026-10-14)
Fix: V did not show the pictures the scraper downloads when the Instructions file is in the folder Scraper.
- The scraper program saves a game's loading screens in a `Screen Grabs` folder beside the Instructions file, so with `Instructions.txt` in `/Scraper` they are in `/Scraper/Screen Grabs`; V only looked in `/Screen Grabs` in the root of the card. It now looks in both: `/Scraper/Screen Grabs` first (the loading screens), then `/Screen Grabs` (the grabs made on the Spectrum). With the Instructions file in the root, nothing changes.
- A game's own picture (from a snapshot or tape) is still first of all; at most 8 pictures are cycled in all. D deletes the picture shown from whichever folder it is in (the scraper does not fetch a deleted one again: it keeps its own list). A Save Game of the game finds the same pictures.
- Grabs made on the Spectrum (G, or a double press of the NMI button) still go in `/Screen Grabs`, and names there are chosen from that folder only.
- No folder, no search: the 40 name lookups for a folder are skipped when it is not there.
- **F** on the game information (V in Browse Games, or I on the menu) switches between the details screen (small picture, software house, year, controls, instructions) and the picture on the whole screen. The pictures still cycle by themselves in either view; N, P and D do nothing on the full screen, and the key row (which now shows `F-Full` when there is a picture) is not drawn there. The choice stays until the power is off; M-Edit leaves the full view. With no picture F says "No picture to show". New ROM action `.infoop` 5 (about 20 bytes; `spare_size` is still above what the buffers need); key row strings changed (`key_help` has a `can_full` argument).
- Help: the "Seeing a game: V" page tells about F; the "Instructions file" page says the scraper's pictures are read from `/Scraper/Screen Grabs` too. `FIRMWARE_ID` is `Rev 1.8 (tapeloader v108)`. 

## Rev 1.7 (2026-10-14)
Edit a game's details on the Spectrum: software house, year, controls and instructions. Instructions.txt is still read, and still edited on a PC, as before.
- M on the game's information (V in Browse Games, I on the menu, or from Recently played) now opens a small menu, "Edit: <game>": **H** software house, **Y** year, **C** controls, **I** instructions, **N** your own notes (the old M screen, unchanged), **R** takes back everything typed in (asks Y or N), **X** back. The key row says `M-Edit` where it said `M-Notes`.
- Each of H, Y, C and I opens a box with what the screen shows now. H and Y are one line (30 and 8 letters; ENTER finishes). C is 512 letters and I is 2,048 letters, the same limits as the Instructions file, with a count ("57 of 512 letters"). The instructions box has room for 96 lines and scrolls; text from Instructions.txt is cut into lines of 30 letters when it is opened. BREAK finishes, then Y saves, N does not, anything else goes on.
- Where it is kept: `/Game Notes/<game>.info.txt`, in the same words as an Instructions.txt entry without the name (`Software house:`, `Year:`, `Controls`, `Instructions`), so a PC can read and change it. Only the parts you edited are in it; a part that is there (even an empty one, which hides what Instructions.txt says) replaces that part of the game's entry, and the rest still comes from Instructions.txt. Instructions.txt itself is never written. A Save Game of the game finds the same file, as it does for notes.
- A game with no entry in Instructions.txt gets one from what you type in. A line in your text that would be taken for a heading (`Controls`, `Year: ...`) is kept as text: the file has `* ` in front of it, and it is removed when read.
- It needs the Game Notes folder (as the notes do). If the save cannot be made the box stays open with the text in it.
- The details are not in the ROM: no change to `nmirom`. The editor (`notes.h`) is now a template (`EditorT<lines, columns>`; the notes editor is the same 18 by 30 as before) with a letter limit, scrolling and a one-line mode. `gameinfo::Details` reads and writes the file; `Info::apply` lays it over the entry.
- Help: the "Your notes: M" page is now "Edit and notes: M"; the Instructions file page says typed-in details win. `FIRMWARE_ID` is `Rev 1.7 (tapeloader v107)`.

## Rev 1.6 (2026-10-14)
The firmware is now called PicoMenu (the fork's name); the hardware is still Brian Apps' picoFace.
- The name on the screen: the big title on the menu (PICOMENU) and the title bar on every menu screen, "Update PicoMenu", "This PicoMenu", the update refusals ("Not a PicoMenu firmware" ...), "PicoMenu can load: ...", "The PicoMenu needs that folder", the Help text, and the example Instructions.txt comment. All are the same length as before, so nothing moves.
- On a PC the SD card now shows as "PicoMenu SD card" (vendor PicoMenu).
- Not changed on purpose: the version text the updater looks for (so Rev 1.0-1.5 uf2 files and this one still recognise each other), file and folder names, the licence headers (they credit picoFace), and the `.szx` creator field.
- README starts with PicoMenu and says it is a fork of picoFace.
- `FIRMWARE_ID` is `Rev 1.6 (tapeloader v106)`.

## Rev 1.5 (2026-10-14)
The help text, as edited by hand (no other change).
- `Docs/Help.txt` is now your edited version: 25 pages (was 28). Gone: "Tapes: .tap and .tzx", "After a tape" and the intro paragraph of the first page; the Test ROM text is part of the Change ROM section; File keys are "Commands" and "Commands: more"; "Saving: where, and over" is "Saving:"; "SD card on a PC" is "View the SD card on a PC".
- Fixed so it builds (the screen is 32 columns by 22 lines): lines over 32 letters were re-wrapped, a tab in the keys list became spaces, and three pages that were too long were split or tightened: "File types" (the 48.rom and limits part is now the page "Tapes: 48.rom"), "Change ROM: R" (the test ROM part is now "Test ROM: T") and "View the SD card on a PC" (same words, wrapped). The menu line says "T-Test ROM (see Test ROM)".
- Typos fixed: snaphot, "as game", ".sna or.z80", "press use", "directory of file", "48k"; the 48.rom paragraph was reworded to read properly.
- Brought up to date with Rev 1.3 and 1.4: C-Copy says a folder too (one drive), R-Rename says file or folder, and the folder paragraph is now "Moving or copying a folder" (with the bar and BREAK).
- Tests: `test_diag.py` expected the old first line of the first page; it now expects "The NMI button". `FIRMWARE_ID` is `Rev 1.5 (tapeloader v105)`.

## Rev 1.4 (2026-10-14)
One change on top of Rev 1.3.
- **Rename a folder.** R, then the folder's number (it used to say "That is a folder, not a file"). The name is edited without the `/`, 28 letters at most, like a file's; a file keeps its extension, a folder has none, so `Hello.v2` is edited whole.
  - The folder keeps everything in it (one rename, instant on both the SD card and the flash). Nothing is overwritten: a name already in use says so.
  - The Picoface's own folders (Scraper, Game Notes, Save Games, Screen Grabs, Favourites, Poke List) in the top of the card or the flash can't be renamed, because it looks for them by name: the message is "The Picoface needs that folder". A folder with one of those names deeper down renames normally.
  - Not updated for you: the recently played list, a chosen Save to: folder and a picked move or copy keep the old path, so a game in a renamed folder says "Cannot find that game" until it is loaded again from Browse, and the save folder has to be chosen again. Instructions.txt and notes do not depend on folder names except the Picoface's own folders.
  - The rename screen says "Rename" (was "Rename file"), the question is "Rename which file/folder?", and the error is "Could not rename it".
- ROM: `.renamehandler` sets `.pickdirs`, shows the pico's reason when the status is `FILEOP_CANT_MOVE_FOLDER` (11) after DESCRIBE. About 160 bytes of spare room are left (`spare_size` 0x925 against 0x880 needed).
- Pico: `nmi_action_snap_rename` takes folder entries (`any_entry_for`), works out the name without the `/`, refuses the Picoface's own folders at the top level (`is_picoface_folder`) and a path too long to browse into.
- Help: the File keys page says R-Rename is for a file or folder. `FIRMWARE_ID` is `Rev 1.4 (tapeloader v104)`.
- Not tried on a real card.

## Rev 1.3 (2026-10-14)
One change on top of Rev 1.2.
- **Copy a folder.** C, pick a folder (it used to say "That is a folder, not a file"), browse to where it should go and press C again. It stays on one drive: SD card to SD card, or flash to flash. A copy from the SD card to the flash (or back) is not done: it says "Folders can't change drive".
  - It asks "Are you sure you want to copy", and you type ok and ENTER (anything else copies nothing).
  - A bar fills while it works (it counts the bytes first, then copies; the bar is by bytes, so a few big files fill it unevenly). BREAK stops it: what was copied so far is removed, the original is never touched, and the picked folder stays picked so you can try again.
  - Refused with the reason shown: into the folder itself or a folder inside it, into the folder it is already in, a name already in use at the destination, a path too long, a tree nested more than 8 levels. Nothing is overwritten.
  - If it fails part way (card full, a read error) the partly made copy is removed and the message is "Could not copy the file".
  - "Move" a folder is unchanged; the wording after a wrong word is now "Not done: ok was not typed" for both.
- Pico: `fsops::copy_tree` (+ `walk_tree`, `TreeCopy`, `CopyStep` in `fsops.h`): walked without recursion (the open directory of each level is in `find_dirs`, the paths grow and shrink in two buffers) so the Pico's small stack is not used for depth; the tree is walked twice, once to count, once to copy. `copy_file` takes an optional step callback (bytes copied, false = stop; status `CANCELLED` = 12). On the SD card each new folder is made with the list's memory lent to FatFs, just for that call (`copy_folder_make`), because it is also the copy buffer.
- Progress: the Pico writes how much of the 28 cells in mailbox byte 3; the Spectrum puts 1 in byte 8 for BREAK (`ACTION_SNAP_MOVE_OP_COPY` for a folder).
- ROM: `.copybar` (the screen and the loop), the DESCRIBE request carries 1 (move) or 2 (copy) in byte 6, and `.pickdirs` is 1 for both. About 180 bytes of spare room are left (`spare_size` 0x932 against 0x880 needed).
- Help: the "Moving a folder" page is now "Moving or copying a folder". `FIRMWARE_ID` is `Rev 1.3 (tapeloader v103)`.
- **Not tried on a real card.** In particular: how long a big folder takes, whether making many folders on the SD card in a row is fine for the Pico (the New folder freeze of v23 was a stack problem in FatFs's f_mkdir; this copy makes them from a flat loop, not nested calls), and the speed of the bar.

## Rev 1.2 (2026-10-13)
One change on top of Rev 1.1.
- **Recently played: long names scroll.** The list showed only the first 21 letters of a name. A name longer than that now waits about 1.4 s, moves one letter every 0.25 s to its own end, waits and goes back (the same timing as the Reload line on the menu; all long names move together, a shorter one stops at its own end). The speed counts passes of the keyboard scan, so on a real +2 it is an estimate: tell me if it is too fast or slow. Nothing here has been tried on hardware.
- ROM: `ACTION_RECENT_LIST` (draw modes 0 and 1) takes a WORD in params 3-4 (`ld (ix+3), hl`, spare_space): the Pico writes each full name there, `RECENT_SLOT` (64) bytes per game, a name is at most 63 letters. `.recentscrollinit`, `.recentidle` (called from the `.recentkey` loop) and `.recentpaint` redraw the 21-letter window at column 2 of rows 2 to 11 with `putchar` (the colours the Pico drew stay). The ROM has about 100 bytes less spare room than Rev 1.1.
- **Source package tidied (no firmware change).** Removed `Commands.txt` (two lines of Windows clean/cmake commands), `pico/sjasmplus.exe` (the build uses `sjasmplus` from your PATH; see the README), the empty `pico/CMakeFiles/`, and the `tests/` folder (the simulator and host tests need tooling that is not in the package). Mentions of `tests/...` in older entries below are history.
- `FIRMWARE_ID` is `Rev 1.2 (tapeloader v102)`. Licence files (`THIRD-PARTY-NOTICES.txt`, `pico/FatFs_SPI/LICENSE-Apache-2.0.txt`) are included as added after Rev 1.1.

## Rev 1.1 (2026-10-13)
Six small changes on top of Rev 1.0.
- **Licence files added (no firmware change).** `THIRD-PARTY-NOTICES.txt` in the top folder lists every third-party part with its full licence text (FatFs, littlefs, LZ4, Apache 2.0 SD driver, Pico SDK, TinyUSB, newlib/libgcc). `pico/FatFs_SPI/LICENSE-Apache-2.0.txt` is the Apache 2.0 text the SD driver files point to. Ship the notices file with every release and uf2.
- **NMI menu: the Reload name scrolls.** A name too long for the line (28 letters after `R-`) now waits about 1.4 s at the start, moves one letter every 0.25 s to the end, waits there and goes back. The Pico now sends up to 56 letters of the name (it cut it to 20 before). A name that fits stays still. The speeds are counted in passes of the keyboard scan (about 0.7 ms each by the simulator's T-state count), so the real speed on a +2 is an estimate: tell me if it is too fast or slow.
- **Update Picoface screen fits the screen.** The text is broken into lines of at most 30 letters (it ran off the right edge: "Do not switch off while it runs.", "plugging in USB and copy the .uf2.", the long file name and the error lines). A long file name is cut in the middle with `..`.
- **Menu keys renamed:** **R**-Reload last game, **C**-Change ROM, **D**-Set up SD card (they were L, R and C). Help, the README and the "A ROM: use C-Change ROM" notice say so. Inside Set up SD card, D is still "set up this card?".
- **O-Info has column titles.** With O on, the folder moves up a row and the row above the entries reads **Type**, **Size** and **Mode** in yellow over the extension, size and 48/128 columns.
- **My notes: DELETE.** The editor says `DELETE deletes` instead of `CAPS+0 delete`, and Help says DELETE where it said CAPS SHIFT+0 (notes, Find, Save name, quick-save name, poke digits). The key is the same: the +2's DELETE key is CAPS SHIFT+0.
- **Save snapshot: the file type is yellow.** In `Type: auto` the word auto (or .sna, .z80, .szx, same) is bright yellow.
- NMI ROM: the Reload scrolling, the titles and the yellow type cost about 230 bytes, so the screen is now sent in 6 pieces of 1152 bytes instead of 4 of 1728 (`SCREEN_PIECE`) to keep the room the other buffers need; `spare_size` 0xA9F (2,719; 2,176 needed). Firmware text 212,564 bytes of 253,952. Help has 28 pages.
- The firmware id is `Rev 1.1 (tapeloader v101)`: the Update screen of a Rev 1.0 or older Picoface accepts it.
- Tests: `trom/test_menu.py` 66 checks (scrolling start, pause, steps, end, wrap, short names, 64-letter label, colours; the yellow Type), `trom/test_files.py` (the titles and the folder row), `thost/drv.cpp` 477 (the Update screen with a long name, nothing past column 30), `tinfo/test_fwupdate.cpp` 93.
- **Licence tidy-up: `sd_driver/demo_logging.c` removed.** It was a FreeRTOS example file (GPL version 2 with the FreeRTOS exception, not the project's GPL version 3) holding one function, `vLoggingPrintf`, that nothing calls and that the linker already left out of the firmware. It is taken out of `FatFs_SPI/CMakeLists.txt` and deleted. The firmware code is unchanged (same size, 212,564 bytes; only some branch addresses move because the link order changes).
- **Not tested on hardware.**

## Rev 1.0 (2026-10-13) - the v65 build, renamed
- **This is v65 under a new name: the first numbered release, and the point to roll back to.** The only code change is the version text, which Help > Diagnostics, the NMI Update screen and the update check now show as `Rev 1.0` (it said `tapeloader v65`). Everything else is v65 as delivered: moving folders, Set up SD card, double press screen grab and the rest.
- Later work (such as .tzx turbo loading) is built as separate Rev 1.1 test builds from a copy of this source, so this one stays untouched. To go back, flash `picoface-Rev1.0.uf2` (the Update screen accepts it as it does any other uf2) or rebuild from `picoface-tapeloader_Rev1.0.zip`.
- **Fix (the first Rev 1.0 file was refused by the Update screen with "Not a picoface firmware").** The updater accepts a file only if it holds the text `tapeloader v` and a number, and Rev 1.0 had dropped it. Now the image holds `Rev 1.0 (tapeloader v100)`: firmware up to v65 finds `tapeloader v100` (so it accepts the file), and from Rev 1.0 the updater looks for `Rev ` and a number such as 1.0 first, so the Update screen shows `Rev 1.0`. Every later build must keep both in `FIRMWARE_ID`. Tested with the real uf2 against the old updater code (accepts) and the new one (accepts, shows Rev 1.0), and the old updater against the first Rev 1.0 uf2 (refuses with error 7: the fault reproduced). `tinfo/test_fwupdate.cpp` 93 checks.
- Older entries below keep their v-numbers (v1 to v65).

