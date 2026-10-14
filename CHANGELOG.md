# Changelog

Changes on top of the original picoFace, in the order they were made. Base: `picoface-tapeloader`. This replaces the separate `.patch` files.

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

