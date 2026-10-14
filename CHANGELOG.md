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

## v65 (2026-10-13)
- **Folders can be moved** in Browse Games (SD card and internal flash). Press **M**, pick a folder by its number (before, only files could be picked), browse to where it should go, press **M** (Move here). The screen asks **"Are you sure you want to move"**, shows the folder's name and the From and To folders, and says "If so, type ok". Type **ok** and ENTER; anything else, or BREAK, moves nothing ("Not moved: ok was not typed").
- **No progress bar, on purpose.** A folder moved inside one drive is a single rename of the folder: the files in it are not copied, so a folder with hundreds of files in it moves as fast as an empty one. A bar would only flash up. If you want folder moves between the SD card and the flash (which do have to copy every file, and would need a bar), that is a separate, bigger job - say so.
- **Refused, with the reason on screen:** another drive ("Folders can't change drive"), the folder it is already in ("It is already in this folder"), into itself or a folder inside it ("Not into the folder itself"), a path too long, a name already in use at the target (as for files, nothing is replaced), and the Picoface's own folders in the root of the SD card (Scraper, Game Notes, Save Games, Screen Grabs, Favourites, Poke List: "The Picoface needs that folder"). **C-Copy does not pick folders** (a folder can be moved, not copied).
- NMI ROM: C and M share the picker; the folder flag is set for M only. The confirm screen is the Erase folder one (`.eraseprotected`) with the texts supplied by the Pico. `spare_size` 0xBAE (2,990; needs 2,752). Pico: `nmi_action_snap_move` has a folder branch (DESCRIBE gives status 4 and the texts, or status 11 and the reason; DO needs "ok" at the text address in bytes 6 and 7 and does `fsops::rename_path`). Firmware text 211,236 bytes of 253,952.
- Help has a new page "Moving a folder" (28 pages).
- Tests: `thost/drv.cpp` section 19 (49 more checks, 467 in all: the move, contents arriving, the confirmation texts, no "ok" or the wrong word moves nothing, copy refused, same folder, into a descendant, name in use, Save Games kept, other drive), `trom/test_files.py` (the real ROM: the confirm screen, ok sends the move once, a wrong word sends nothing, a file move is unchanged, a refusal shows its reason).
- **Not tested on hardware.**

## v64 (2026-10-13)
- **C - Set up SD card: fixes for the freeze when Y is pressed** (reported on the +2: it hung and made no folders). I could not make it hang in the simulator, so these are the differences from the paths that are known to work (New folder, the notes), and none of them is proved on hardware:
  - **Run from the main loop, not inside the request handler.** The folders are now made by `sd_setup_early()`, called by the main loop just before `processNmiRequest()`, so f_mkdir and the file calls run on about 600 bytes less stack than before (core 0 has only 2 KB and f_mkdir needs a lot of it). The request then only draws the result.
  - **Folders are made the way New folder makes them**: `walk_stop()` first (no directory of Browse Games left open) and FatFs lent the list's memory to clear the new folder (`make_sd_folder`). The first screen grab (G or the double press) makes `Screen Grabs` the same way; before, it made it with the small heap only.
  - **If it still freezes, Diagnostics says where:** a note is written in the flash before each step (as New folder does). After a power cycle Help > Diagnostics shows `Set up SD froze at step N` (0 Scraper, 1 Game Notes, 2 Save Games, 3 Screen Grabs, 4 Favourites, 5 Poke List, 6 the example file). Tell me that line.
- **The result is said plainly.** The heading of the result screen is **SD card created** (something was made), **SD card creation failed** (any item FAILED; the screen adds "Is the card full or locked?" and Diagnostics, Last error, names the first folder that failed) or **SD card already set up** (nothing was needed). Each folder still says made, there already, kept yours or FAILED.
- Help (Set up SD card page) says so. No change to the NMI ROM. Firmware text 209,876 bytes.
- Tests: `thost/drv.cpp` 416 checks (the heading in each case, a file in the way gives "creation failed" and the last error, `sd_setup_early` makes everything once and the request only draws, and the folder is made with the small heap refusing big buffers, via `tfs/lim.c`), `test_sdsetup_real.py` 17 (the bridge now calls `sd_setup_early` as the main loop does).
- **Not tested on hardware**: this is the part to check. Press C, D, Y and tell me what the screen says, or what Diagnostics shows after a power cycle if it freezes.

## v63 (2026-10-13)
- **The Screen Grabs folder is no longer hidden.** The Picoface makes `Screen Grabs` as an ordinary folder (G, the double press and C - Set up SD card), so a PC shows it without "show hidden files"; hide it on the PC if you like. C's list says `grabs` instead of `grabs (hidden)`.
- **Browse Games still does not list it** (V finds the pictures itself), as for `Poke List`: the listing leaves out a folder called `Screen Grabs` in the root of the SD card, so it can't be opened, moved or erased there. On the internal flash it is listed like any folder.
- **A folder made hidden by an earlier version stays hidden** (the Picoface does not change the attribute of a folder that is already there); clear it on the PC (Windows: right-click the folder, Properties, untick Hidden).
- Help (the V page and the Set up SD card page) and the README no longer say hidden. No change to the NMI ROM. Firmware text 208,788 bytes.
- Tests: `thost/drv.cpp` 408 checks (the folder is made not hidden, by G and by C; Screen Grabs is not listed on the SD card), `test_sdsetup_real.py` 17 (not hidden), and the rest of the v62 suites.
- **Not tested on hardware.**

## v62 (2026-10-13)
- **C - Set up SD card is back** (it was taken out after an early v56; this is that feature again, unchanged, on top of v61). A new item on the NMI menu (between Screen grab and Update). It shows what it will make; **D** asks "Set up this card?", **Y** does it, **N** cancels. It makes, on a card with nothing on it: `Scraper` (with an example `Instructions.txt` in it), `Game Notes`, `Save Games`, `Screen Grabs` (hidden), `Favourites` and `Poke List`. Nothing that is already there is replaced; the example file is made only if there is no Instructions file anywhere (`Scraper/` or the root), and the result screen says `made`, `there already`, `kept yours` or `FAILED` for each item. With no card it says "No SD card found." and makes nothing.
- NMI ROM: one new menu row (U, T and X move down a row; X is now on row 18) and the handler (`handle_sdsetup`, which asks the Pico for the game information screen with mode 2); `spare_size` 0xC02, still above the 2,752 bytes the code needs. Firmware: `sdsetup.h` and `sd_setup_*` in `picoface.cxx`; the SD calls are in a small routine called straight from the request handler, as for the notes (core 0 has a 2 KB stack). Firmware text is 208,948 bytes of the updater's 253,952.
- Help has the Set up SD card page again (27 pages); the menu page and the README table list C.
- Tests: `trom/test_sdsetup_real.py` (17 checks: the real ROM against the real Pico code), section 19 of `thost/drv.cpp` (20 more host checks, 407 in all), `test_menu.py` and `test_gameinfo.py` (the menu rows with C, C's three paths). Everything from v61 still passes.
- **Not tested on hardware**: making the folders and the hidden attribute on a real card.

## v61 (2026-10-12)
- **Double press of the NMI button saves the screen.** Press the button twice quickly (the second press within about a third of a second of letting go of the first) and the Picoface saves the game's screen as a `.scr` in the hidden `Screen Grabs` folder on the SD card, exactly as **G** on the menu does (same names: `Name.scr`, `Name1.scr` ... up to 39 for a game; the same limits). No menu opens and the game carries on.
- **The border says how it went:** **green** for saved, **red** for not saved (no SD card or the PC has it, could not write, too many grabs of that game, or no screen), for about 0.4 seconds, then the game's own border comes back. There is no text, so the cause of a red border is in Help > Diagnostics (Last error) or by trying G on the menu, which says why.
- **A single press now waits a moment.** To tell one press from two the button is watched for about 375 ms after you let go, so the menu opens that much later than before. Holding the button for one second still resets into the Spectrum's own ROM (decided at the one second, no wait).
- Like G, it saves the normal screen: a 128K game using the shadow screen gives its normal screen. The game stops for the moment the NMI runs (the screen goes to the Picoface and the file is written).
- NMI ROM: new startup action `STARTUP_ACTION_GRAB` (5) and `quickgrab` (+66 bytes; `spare_size` 0xC1C). Pico: `extras::classify_press` (the single/double/long decision, timed with `sleep_ms`, tested with a fake button) and the main loop. Help: the NMI button page and the screen grab page say so.
- Tests: `trom/test_grab.py` (33 checks: the real NMI ROM from its entry with the grab action: the screen is handed over, one grab request, no menu, no restore, green for saved, red for each of the four failures, the game's border comes back, the colour stays about 0.4 s), `tbutton/t.cpp` (17 checks: single, double, long, the edges of the window and of one second, bounces and glitches). `trom/test_recent.py` was changed so that its load stops at the start of the load (it used to end by chance when the code layout allowed it, and hung when the layout moved).
- **Not tested on hardware**: the double press timing on a real button (the 25 ms settle time and the 350 ms window are guesses; if a single press sometimes opens a grab or a double press opens the menu, tell me and they can be changed), the border colours and length on a real Spectrum, and the grab on a real SD card.

## v60 (2026-10-12)
- **The firmware is 44 KB smaller.** The libraries (FatFs, littlefs, the SD driver, TinyUSB, the Pico SDK) are now built for size (`-Os`) instead of the SDK's `-O3` for a Release build; `pico/CMakeLists.txt` says so. The code size (text and read-only data) goes from 248,804 to 204,276 bytes, so there are now about 49 KB under the updater's limit of 253,952 bytes, not 5 KB.
- `picoface.cxx` (all of the Picoface's own code) is still built at `-O3`, so its code and its stack use are as they were. This matters because core 0 has only 2 KB of stack and the earlier freezes were stack overflows. The library functions that the SD card and flash calls go through use the same or a little less stack than before (checked in the compiled code: `f_mkdir` 736 against 768 bytes, `f_rename` 848 against 888, `f_unlink` 832 against 840, `pico_rename` 1840 against 2496, the deepest chains that I can count). The Z80 bus is served by `piohandler.S` (assembly), which the flag does not change. No function that runs from RAM calls into the flash (the same check as before).
- The libraries run a little slower (SD card and flash file calls, USB); the SD card and the USB link are the limit there, so it should not be noticed.
- Nothing else changed: the same features as v59, the same NMI ROM.
- **Not tested on hardware.** A change of compiler flags is the kind of change that can only be proved on a real Picoface. If anything behaves differently from v59 (freezes, USB or SD card problems, a slow load) go back to v59 and tell me.
- Build with `-DCMAKE_BUILD_TYPE=Release` (the default of the SDK) as before; the flags are set in the `CMakeLists.txt`.

## v59 (2026-10-12)
- **Tape summary on the game information.** In Browse Games press **V** on a `.tap` or `.tzx` (or V in the recently played list), then **N**: a **Tape** page counts the blocks in the file and says whether the Picoface can load it:
  - `Data blocks: 14  Standard: 12`, `Turbo blocks`, `Pure data blocks`, `Pulse/direct recording` (pulse and tone blocks, direct recording, CSW and generalised data), and for a `.tzx` `Loops: 2  Jumps: 0  Calls: 0`. A `.tap` shows its number of blocks only.
  - **Picoface can load: Yes / Maybe / Probably not / No.** Yes: standard blocks only. Maybe: turbo or pure data blocks (works only if the game loads them with the ROM's loader). Probably not: pulse or direct recording blocks (the game times the signal itself). No: damaged, an unknown block, or no data blocks. A `.tap` can only be Yes or No: a turbo loader cannot be seen in a `.tap`.
  - With no entry for the tape in the Instructions file, the first page also says `Picoface can load: ...` and `N-Tape details`. With an entry, the entry stays on the first page and the Tape page is the next one (before My notes).
  - The blocks are counted as they are in the file: loops, jumps and calls are not followed. The file is not read through: the block headers are skipped over with a seek.
  - Only for a tape previewed in Browse Games or the recently played list. The I page of the menu (the game last loaded) has no tape page.
- Help has a new page (Tape summary). The verdict is the Picoface's guess from the blocks: it cannot know how a game's own loader works.
- No change to the NMI ROM. The firmware is about 5 KB under the .uf2 limit of the updater (text 248,804 of 253,952 bytes).
- **Not tested on hardware**; the tape files in the tests are made up to the format (`tests/drv.cpp`).

## v58 (2026-10-12)
- **.tzx loops, jumps and calls are followed.** Loop start/end (0x24, 0x25: the blocks inside play the number of times given), jump (0x23, relative, forwards or backwards), call sequence and return (0x26, 0x27: each call in turn, then on after the sequence; one level, the first 32 calls). Before, the blocks of a loop were read once and the jumps and calls were ignored. A jump of 0 (which would loop for ever) is ignored. A loop with no data block in it is not repeated. So that a damaged or endless tape can't hang the Picoface, at most 2000 control blocks in a row and 4096 data blocks per run of the tape are followed. Pauses, "stop the tape" (0x20 with 0, 0x2A), tones, pulses, text and the like are still skipped (the ROM trap only gives a block when the Spectrum asks for one, so a stop is how it works anyway). Select blocks (0x28) are still skipped.
- **The tape stops at the end.** Before, when the blocks ran out the tape started again from the first block (and gave it to the loader at once). Now the load fails ("R Tape loading error", as the end of a real tape would leave the Spectrum waiting), and the tape is rewound so that the next load starts from the first block again. This is for `.tap` and `.tzx` alike.
- Behind it: `FileHandle` has `seek` and `tell`; the .tzx block reader is split into a block header reader and the playing of the control blocks, and the tape being served keeps its own place (looking at another `.tzx` with V does not move it).
- Help (Tapes: 48.rom and limits) says so. No change to the NMI ROM.
- Tests: `tests/test_tzx_loops.cpp` (22 cases: loops, jumps, calls, limits, block counting), `tests/test_tzx_reader.cpp` (the earlier block reader tests, unchanged), and `tests/drv.cpp` (the real code on the SD card and the flash: the end of the tape, rewind, a loop followed, a preview not disturbing the tape).
- **Not tested on hardware**, and no real `.tzx` file with loops was available: the files in the tests are made up to the format.

## v57 (2026-10-12)
- **Coloured PICOFACE title.** The big title on the main NMI menu now has each letter in its own colour, in turn: red, yellow, green, cyan (P I C O F A C E), bright, on the blue bar. The letters are 2 pixels higher in the bar. Every other screen drawn by the NMI ROM has a one-row blue bar with PICOFACE in green where it said "Pico Menu"; the screens the Pico draws itself (Help, game information, Recently played, Update) keep their own title bars.
- Each letter is two colour cells wide, so the colours do not clash. A black shadow can't be drawn (two colours per cell), so there is none.
- NMI ROM: the screen goes to and from the Pico in 4 pieces of 1728 bytes instead of 3 of 2304, which frees ROM space (about 400 bytes are spare).
- (The SD card setup of an unreleased first attempt at v56 was removed; v56 was the plain green title.)
- **Not tested on hardware**; the real ROM runs in the simulator and its screen is checked (`trom/test_menu.py`, `test_info.py`).

## v56 (2026-10-12)
- **A PICOFACE title bar.** The main NMI menu now has a blue bar across its top three rows with **PICOFACE** in bright green letters twice the menu's size (the menu's own font, doubled). Every other screen drawn by the NMI ROM (Browse Games, Change ROM, Save snapshot, Poke, ...) has a one-row blue bar with PICOFACE in green where it said "Pico Menu" in white. The screens the Pico draws itself (Help, game information, Recently played, Update) keep their own title bar.
- **No black shadow.** The mock-up with a black shadow cannot be drawn on a Spectrum: a cell has only two colours and the shadow needs a third (blue bar, green letter, black shadow). The title is green on blue with no shadow.
- NMI ROM: the screen goes to and from the Pico in 4 pieces of 1728 bytes instead of 3 of 2304, which frees ROM space for the title (about 450 bytes are spare).
- **Not tested on hardware**; the real ROM runs in the simulator and its screen is checked (`trom/test_menu.py`, `test_info.py`).

## v55 (2026-10-11)
- **The Instructions file is read from a folder called `Scraper`.** Put `Instructions.txt` (or `Instructions`) in a folder named `Scraper` in the root of the SD card. The firmware looks there first: `Scraper/Instructions.txt`, `Scraper/Instructions`, and only if neither is there, as before, `Instructions.txt` and `Instructions` in the root. (The root is kept as a fall-back so an existing card keeps working; remove it from `pico/picoface.cxx`, `info_read_file`, if you do not want it.)
- Only the Instructions file moves. **Screen Grabs and Game Notes are still in the root of the card.** The scraper program puts the loading screens it downloads in a `Screen Grabs` folder beside the Instructions file, which would now be `Scraper/Screen Grabs`; the Picoface does not look there, so move them to the root `Screen Grabs`, or change where the scraper puts them.
- Help (Instructions file page), the README and the notes in the source say so. No change to the NMI ROM.
- **Not tested on hardware.**

## v54 (2026-10-11)
- **The notes are shown first and can't be changed until you press E.** **M** now opens a My notes screen that just shows what is there ("No notes yet." if there is nothing), with the key line `E-Edit  X-Back`. Letters typed there do nothing. **E** starts editing (cursor at the end; the editor's keys are as before, BREAK finishes, Y saves, N throws the changes away). After a save or a discard you are back on the notes screen showing the notes as they are in the file; **X** or BREAK there goes back to the information, at the notes page if you saved something.
- A fix found on the way: if the NMI was left (or the Picoface reset) while the notes screen was up, the next visit to the information could come up as the notes screen. A normal request now always forgets the notes screen.
- Help (Your notes) is rewritten for this. No change to the NMI ROM.
- **Not tested on hardware**; tested in the simulator against the real Pico code, as v53.

## v53 (2026-10-11)
- **The Game Notes folder is made on a PC.** Put a folder named `Game Notes` in the root of the SD card. The Picoface no longer makes it (creating a folder is the deepest thing the SD card does). Without the folder, **M** does not open the editor and says **No Game Notes folder - add one**; if the folder goes away while you are editing, the save says the same, the editor stays open with your text, and it saves once the folder is back. A folder made on a PC is not hidden, so Browse Games lists it; set the Hidden attribute on the PC if you do not want to see it.
- Help (Your notes) and the README say so.
- **Still not tested on hardware.** The real Pico code now also runs against the real NMI ROM in the simulator (the keys M, letters, BREAK, Y on a simulated SD card, saving, saving over, discarding, no folder); nothing hangs there, so if the Spectrum still hangs on a save the cause is something only the real Picoface does. Please say which version the Picoface shows (the update screen shows it) and exactly when it freezes.

## v52 (2026-10-11)
- **Fixed: saving My notes hung the Spectrum.** On the Spectrum the editor showed its flashing cursor and typing worked, but BREAK, Y then froze everything until the power was switched off, and no notes were saved. Cause (found from the compiled code, not reproduced on hardware): the SD card's file calls ran on top of the big stack frame of the game-information routine (about 1.5 KB of the 2 KB-plus the first core has for its stack, with the SD card's rename needing about 1.1 KB on top). The save is now done by a small routine of its own, called before the game-information routine starts, the file handle is kept in the spare buffer instead of on the stack, and the page text is drawn by its own routine so that it does not add to the frame. At the worst point the save now needs about 1.7 KB instead of about 3 KB.
- The notes are now written straight to `Game Notes/<game>.txt` (as a screen grab is) instead of to a `.new` file that was then renamed over the old one, which removes the two deepest SD card calls from the save. If a save fails the editor stays open with your text and says "Could not save the notes", so nothing is lost and you can try again. (The old notes file is lost by a failed save; before it was kept.)
- **Cursor keys.** The +2's cursor keys are the same keys as CAPS SHIFT+5 6 7 8, so they move the cursor in the editor. The editor's screen and Help now say "cursor keys or CAPS+5-8".
- **Not tested on hardware**: the fix is checked on a PC, in the simulator and in the compiled code's stack use, not on a Picoface. Please try M, type, BREAK, Y and report what happens.

## v51 (2026-10-11)
- **Software house and Year** in the Instructions file. Under a game's name write `Software house: Microsphere` and `Year: 1985` (also `Publisher:` and `Year released:`), then Controls and Instructions as before. They are shown at the top of the right-hand column next to the picture, in yellow and white, with Controls below them (so Controls get one or two fewer lines on the first page; games without them look exactly as before). A game written as `[Name]` can have just these. They must come straight after the name line (or the `[Name]` lines): after other text a `Year:` line is taken as belonging to the next game.
- **My notes.** While the information is showing (V in Browse Games, V in the recently played list, or I on the menu) press **M** to write notes on the game. BREAK finishes: Y saves, N throws the changes away, any other key goes on editing (BREAK with no changes just leaves). ENTER starts a new line, CAPS SHIFT+0 deletes, CAPS SHIFT+5 6 7 8 move the cursor. The editor holds 18 lines of 30 letters; typing past the end of a line carries on below.
- The notes are kept in a small text file for each game: `Game Notes/<game>.txt` in the root of the SD card (a hidden folder, like Screen Grabs, not listed by Browse Games). They are saved to a new file first, so a failure keeps the old notes; saving an empty note removes the file. Your Instructions file is never changed. A numbered quick save (`Exolon Save Game 3`) shares the notes of `Exolon`. A notes file made on a PC that is too long for the editor can still be read (the last section, My notes, up to about 1000 letters) but M says so and does not open it, so nothing is cut.
- My notes is the last section of the information, so a game with notes has an extra page. The key line now shows M-Notes (`ENT-Load N/P D-Del M-Notes X-Bk` when a screen grab shows D-Del). After saving the screen goes to the page with the notes.
- Help: a Your notes page and the Instructions file page mention these.
- NMI ROM: 30 bytes spare of the 3328 needed. The editor itself is in the Pico; the ROM just sends the keys.
- **Not tested on hardware**: the screens and keys are tested in the simulator and the editor on a PC. Each key redraws the whole screen, so fast typing may drop a key when the screen is still being sent.

## v50 (2026-10-11)
- **The update shows what it is doing.** After Y the screen shows "Updating - do not switch off" and a bar: first "1 of 2 Copying the update" (the firmware is copied into the internal flash, checked as it goes), then "2 of 2 Writing the Picoface" (the bar fills as each part of the program is written). It ends with an **Update complete / Switch the Spectrum off and on again.** screen. The Picoface restarts itself about 3 seconds after that.
- If the write cannot be completed the screen says **Update failed** with the BOOTSEL instructions, and the Picoface goes into BOOTSEL mode by itself (the RPI-RP2 drive appears on the PC). If the install was refused before anything was written, the reason is shown and nothing is changed.
- How: the copy is done in parts, with the Spectrum drawing the bar between them. The install can't return to the main program (it is being replaced), so the Pico now writes its progress where the Spectrum reads it and the second core keeps serving the Spectrum from RAM during the install (before, the second core was stopped, which froze the Spectrum). The Spectrum waits in a small loop in its own RAM at the end, so the screen stays while the Picoface restarts.
- **Not tested on hardware**: the screens and the bar are tested in the simulator and the steps on a PC. Whether the Spectrum keeps running its menu while the Pico erases and writes the program flash is the thing to watch on the real machine (it does while the Pico saves to the internal flash).

## v49 (2026-10-11)
- **U-Update Picoface** on the main menu installs a new firmware from the SD card. Put the new `picoface.uf2` in the root of the card (only one `.uf2` there), press the NMI button, then U. The screen shows the file, this firmware's version and the file's version. **Y** installs it, **N** or X cancels. When it has finished the Picoface restarts; switch the Spectrum off and on again.
- The `.uf2` is looked at **only** by U. Browse Games does not list it and no other menu item sees it.
- **Checks before anything is changed:** the file is a whole `.uf2` for the Pico (RP2040), its blocks are complete and in order, it starts at the right address, its start-up code checksum is right, it fits in front of the internal file system (253952 bytes; this firmware is 236032, so about 17 KB of growth is left), and it is a Picoface (contains "tapeloader v<number>"). It is then copied into the internal flash (`fwnew.bin`) and read back against its checksum. If there is not room on the internal flash (about 70 blocks free are needed) it says so and changes nothing.
- **The simple version:** the copy over the running program is done by code that runs from RAM, with interrupts off and the second core stopped, one 4096-byte sector at a time, then the whole program is read back and checked (three tries). If the power fails in the middle, the Picoface will not start: hold BOOTSEL while plugging in USB and copy the `.uf2` to the RPI-RP2 drive, as for a first install. `fwnew.bin` is removed at start-up.
- Help has an "Update Picoface" page. The erase warnings' texts moved out of the NMI ROM into the Pico to make room (spare_size 0xEA9).
- **Not tested on hardware**: the checks, the copy into the flash, the map of the copy and the final write are tested on a PC with a pretend flash; the real write to the program flash and the restart can only be tried on a Picoface.

## v48 (2026-10-11)
- **Erasing /Favourites or /Save Games asks for "ok" with its own warning.** E on the folder in Browse Games shows "If you delete this folder you will lose all your quick saves," (or "... all your favourites,") "type ok to proceed and delete". Any other text erases nothing. /Favourites is protected on both drives, /Save Games on the SD card only (the flash has no quick-save folder).
- **/Poke List is no longer shown in Browse Games** on the SD card (in the root; only a PC puts files there). The poke lists still work. A folder of that name on the flash is listed as before.
- The "testrom.rom is under 16384 bytes" message is now "testrom.rom is too short" (like DiagROM's) to make room in the NMI ROM.

## v47 (2026-10-11)
- **Delete a screen grab from V.** When a screen grab is showing in V (Browse Games), the key line shows `D-Del`. D asks "Delete this grab? Y-Yes N-No"; Y deletes that file from the hidden Screen Grabs folder and shows the next picture (or "No Picture" if it was the last), any other key cancels. D does nothing on the picture that is in the game's own file, and never on the I (menu) screen. The pictures do not change by themselves while the question is shown.
- The main menu item is now **I-Game Instructions** (was I-Instructions).
- NMI ROM: the repeated text of the Erase testrom.rom and DiagROM.rom screens is shared, which freed the space for D.

## v46 (2026-10-11)
- **Q (quick save) needs a game.** If nothing has been loaded since the Spectrum was switched on, or since a reset (4, 1, T or a long press of the button), Q shows "No game loaded" and goes back to the menu instead of saving the empty machine under the name of the last session's game. Loading from Browse Games, L or E counts. S (Save snapshot) is not affected.
- The L message for nothing to reload now also says "No game loaded". Two grab messages were shortened ("Could not save the grab", "Too many grabs") to make room in the NMI ROM.
- The hardware RESET key cannot be seen by the Picoface, so Q after it still works as long as a game was loaded earlier.

## v45 (2026-10-11)
- **Quick save slots.** Q on a game that already has a quick save now asks "Do you want to overwrite the existing save? Y or N?". Y overwrites it as before. N keeps it and saves under the next free number: `<game> Save Game 2`, then `3`, `4` ... (up to 99; a number whose file was deleted is used again first). Games with no save yet save at once, as before.
- A numbered save counts as its game for Instructions, screen grabs and poke lists (`Exolon Save Game 3` is `Exolon`), and a quick save made after loading one goes to the game's first slot (`Exolon Save Game`), not to `... 3 Save Game`.
- Help: the Quick save page tells this. The NMI ROM has only 2 spare bytes left over the 3328 it needs, so the next ROM feature will have to make room.

## v44 (2026-10-11)
- **V-View** in the recently played list: press V, then a number, to see that game's picture, controls and instructions (as V does in Browse Games, with its screen grabs). ENTER there loads the game; X goes back to the list. X on the "View which?" question cancels it.

## v43 (2026-10-11)
- **E-Recently played** on the main menu: the last 10 games loaded (from Browse Games, L, or the list itself), newest first, with the type and `SD` or `FL` (flash). Press 1 to 9, or 0 for the tenth, to load that game; X leaves. A game played again moves to the top. The list is kept after power off in `recent.cfg` on the internal flash (written only when it changes).
- An SD card game is shown dim while the card is not in; choosing it or a game that was deleted shows "Cannot find that game". Names of 64 letters or more are not recorded.
- The menu has one more row: R and everything below it moves down one. Help's menu page lists E (the "keys are single letters" note was dropped to make room).

## v42 (2026-10-10)
- Screen grabs are saved as `.scr` only; the `.bmp` of v41 is gone.
- V: the pictures of a game now change by themselves (about every 3 seconds, on the first page); the S key is gone. A file with no screen of its own (or an all-zero one) adds nothing: only the screen grabs are cycled. With no screen and no grabs the picture area says "No Picture". A file that cannot be read shows the information too, instead of a message.

## v41 (2026-10-10)
- **G-Screen grab** on the main menu saves the game's screen (as it was when the menu opened) in the hidden `Screen Grabs` folder in the root of the SD card, named like the game last loaded: `<game>.scr` (the Spectrum screen). A second grab is `<game>1.scr`, then `<game>2.scr` ... The folder is hidden (FAT hidden attribute), so Browse Games does not list it; a PC shows it when "show hidden files" is on.
- **V** now cycles through the pictures of a game: the screen in its file (if any) and its screen grabs (up to 8). **S** shows the next picture, round and round; the title row says `Pic 2/3`. The key line shows `S-Pic` only when there is more than one.
- The Instructions screen (I on the menu) is unchanged: it still shows the running game's screen.
- FatFs `f_chmod` is turned on (`FF_USE_CHMOD 1`) to hide the folder. Help: menu page has G, V page explains grabs.

## v40 (2026-10-10)
- Controls and instructions (V and I): N and P go round the pages (after the last comes the first, before the first the last). Only X (or BREAK) leaves, and ENTER loads the game when viewing from Browse Games; other keys do nothing.

## v39 (2026-10-10)
- **Controls and instructions.** V on a game now shows a small (half size) copy of its screen with the game's *Controls* beside it and its *Instructions* below, read from the file `Instructions` (or `Instructions.txt`) in the root of the SD card. Long text goes on more pages (N and P). No entry, no file or no card: "No information available".
- **I-Instructions** on the main menu shows the same for the game last loaded, so they can be read after the game has loaded (the picture is the game's screen as it is when the menu opened).
- The file has one entry per game: the game's name on a line, then `Controls` and `Instructions` (see `Docs/Instructions-example.txt`). Help is 23 pages (new page: Instructions file). On the menu T and X move down one row.

## v38 (2026-10-10)
- **The SD card is a USB drive.** With the Spectrum off, plug the Pico's USB cable into a PC: the SD card appears as a drive (no menu, no button), to copy files to and from. Use Safely Remove (eject) on the PC before unplugging.
- While the PC has the card the Spectrum's menu cannot use it: the Disk row says "SD in use by the PC" and only the internal flash works; Diagnostics says "In use by the PC". After the PC ejects or the cable is pulled the Pico mounts the card again and Browse starts at the top.
- The USB serial port (`usb.py`, flash files) still works, but the Pico now shows two USB devices (serial and drive). Updating the firmware needs BOOTSEL (hold it while plugging in) because the old automatic reset over the serial port is gone. Help is 22 pages (new page: SD card on a PC).

## v37 (2026-10-10)
- Poke list (P, L): a cheat name too long for the row scrolls, as the names in Browse Games do. Names are kept up to 64 letters (were cut at 28).

## v36 (2026-10-10)
- Browse Games: the drive being browsed is shown in white on blue on the Disk row (S-SD Card or I-Internal Flash); the other keeps the key colours. The Browse Help page says so (and no longer says that Info cuts names to 16 letters).

## v35 (2026-10-10)
- Browse Games: Copy is now **C** (was K). **F** is Find, and reads **Clear** while a search is in force (C-Clear is gone).

## v34 (2026-10-10)
- Browse Games: the key area is in groups with a yellow name (File, Game, Find, Disk, Go; Here while moving, copying or choosing a folder). Same keys. `Previous` is `Prev`, `Info (size, 48/128)` is `Info`.

## v33 (2026-10-10)
- T-Test ROM asks which test ROM: zx-diagnostics (`testrom.rom`) or Phil Ruston's DiagROM (`DiagROM.rom`, on the flash). Erasing the flash's `DiagROM.rom` shows its own warning and needs `ok`.

## v32 (2026-10-09)
- Quick save goes in the `Save Games` folder in the root of the SD card.

## v31 (2026-10-09)
- Erasing the flash's `testrom.rom` shows its own warning and needs `ok`.

## v30 (2026-10-09)
- T-Test ROM: the question also says how to exit the test ROM.

## v29 (2026-10-09)
- **T-Test ROM**: resets the Spectrum into a diagnostic ROM (zx-diagnostics `testrom.bin`) kept on the flash as `testrom.rom`. Help is 21 pages.

## v28 (2026-10-09)
- `.pok` files are listed in Browse Games.

## v27 (2026-10-09)
- Poke lists (`.pok`) are read from the `Poke List` folder in the root of the SD card.

## v26 (2026-10-09)
- Erasing the flash's 48.rom shows its own warning (needed for .tap) and needs `ok`.

## v25 (2026-10-09)
- New folder on the SD card: FatFs is lent 32 KB for clearing the new folder (quicker); a note in the flash lets Diagnostics say if it froze inside FatFs.

## v24 (2026-10-09)
- Fix: New folder on the SD card froze the menu (too much stack used).
- Erasing a `.rom` file asks you to type `ok`, like a folder.

## v23 (2026-10-09)
- The internal flash has folders and the same file keys as the SD card: move, copy, rename, erase, new folder, view, favourites, info, search, save-to folder.
- Files move or copy between the SD card and the internal flash.
- Erase works on folders (both drives): you must type `ok` after the warning.
- `.rom` files are listed; Browse points you to R-Change ROM instead of loading them.
- O-Info reads "Info (size, 48/128)". Help is 20 pages.
- Fix: littlefs `pico_dir_close` leaked the directory handle.

## v22 (2026-10-09)
- The L item shows the last game's name (`L-Reload: Exolon`), or `L-No game detected` if there is none or it is on an SD card that is not in.
- The last game (and the name Save starts with) is kept in the internal flash, so it survives power off.

## v21 (2026-10-09)
- **Q-Quick save**: `<game> Save Game` in the game's folder, same file type; asks before overwriting.
- **L-Reload last game**.
- **4 / 1**: reset into 48K or 128K from the menu (with a Y/N question).
- **O** in Browse Games: file type, size and a `128` flag.
- **L in Poke**: cheats from a `<game>.pok` file.
- **Help > Diagnostics** (also D): firmware, free flash and SD space, last error.
- Save type `same` (fifth in the Type cycle).
- Browse Games: a blue bar with "Letter: command" separates the entries from the letter keys; the number prompt is shorter; O-Info shows 48 or 128.
- Help is 19 pages.

## v19
- `.szx` snapshots (Spectaculator / Fuse), loaded and saved; 128K saved with sound chip. RAM stored uncompressed.

## v18 and earlier 128K fixes
- Saving a 128K game keeps the screen and ROM bits of port 0x7FFD from the load (blank display after a save fixed).

## v16
- Save type: auto, `.sna`, `.z80` (Symbol Shift R / T).
- 48K can be saved as `.z80`; 128K can be saved as `.sna` (no sound chip registers).

## On-screen help (v15)
- H in the menu: pages of instructions on the Spectrum's screen (`Docs/Help.txt`).

## Browse Games, sorting and favourites
- Names sorted, folders first; favourites; scrolling long names; merged scrolling.

## Tapes
- `.tzx` tapes (standard, turbo and pure data blocks).
- Tapes load by themselves (the Spectrum types `LOAD ""`).
- 48K snapshots and tapes from the 128 menu.
- Long press (1 second): reset into the internal ROM.
- Paging port left unlocked after a tape.

## Screen
- V in the load menu: preview a game's screen.
- Screen and colour changes.

## SD card
- Folders, search, make folder, move, rename, erase.
- Save into a chosen folder; save over an existing file; smaller save chunks.
- Copy files.

## 128K
- 128K `.sna` and `.z80` loading; saving a 128K snapshot as `.z80`.
