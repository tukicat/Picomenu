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

    SLDOPT COMMENT WPMEM, LOGPOINT, ASSERTION

    DEVICE ZXSPECTRUM48

SCREEN: equ 0x4000
SCREEN_ATTRIB: equ 0x5800
BORDCR: equ  0x5C48

// The communication between the pico and the spectrum nmi rom is via special
// memory locations.

// Memory location zero is treated as special indicator:
// - When this value is 255, this lets the pico know there is work to do.
// - When work is complete, the pico sets to a value other than 255. 0 indicates
//   success, anything else an error of some sort



ACTION_BEGIN_SNAP_READ: equ 0x01
// param1 (word) = name pointer - -- if name pointer is zero load from snapshot currently in
// the pico memory.
// param2 (byte) = drive - 0 for internal pico flash, 1 for external sdcard
// param3 (word) = sna or z80 header offset
// on success
// byte param1 snapshot type -- 0 for sna, 1 for z80, 2 for a 128K snapshot.
// byte param1 + 1: for types 0 and 1, the value to write to port 0x7FFD before loading
// A 128K snapshot (from a 128K sna file or a z80 file for a 128K machine) has
// a z80 style header followed by Z80X_SIZE - 30 more bytes (see Z80X_* below),
// and then the eight RAM banks in the order of page_order, one 16K page after
// the other, which are read with ACTION_SNAP_READ_NEXT like the other types.


ACTION_SNAP_READ_NEXT equ 0x02
// param1 = destination for data
// param2 = length to read
// returns, bytes read in param2

ACTION_SNA_BEGIN_WRITE equ 0x03
// param1 = destination of the sna header

ACTION_SNA_NEXT_WRITE equ 0x04
// param1 = source of data
// param2 = length to write


ACTION_SNAP_LIST equ 0x05
// WORD param1  = start number
// BYTE drive = 0 for internal flash, 1 for external sd
// WORD param3 = destination to write results
// BYTE param4 = flags: bit 0 set to stop a search that is still running
//               bit 1 set to send the size and "128" flag of each file too
//  
// RESULTS are
//     BYTE countReturned  (will be no more than 10)
//     BYTE moreAviable  (non-zero if true)
//     WORD namepointer[10]
//     BYTE hasParent  (non-zero if the directory listed is not the root)
//     BYTE busy  (non-zero if the page is not complete yet: ask again for more)
//     BYTE path[SNAPLIST_PATH_SIZE]  (the directory listed, as a string to show)
//     String data follows
//
// Each entry is two strings: the name and then the extension (and, with bit 1 of
// the flags, a third for a file: eight characters, its size and "128" if it needs
// a 128K machine). Sub directories
// of the SD card are listed first, their names end with '/' and they are
// entered with ACTION_SNAP_CHDIR.

SNAPLIST_HASPARENT equ 22
SNAPLIST_BUSY equ 23
SNAPLIST_PATH equ 24
SNAPLIST_PATH_SIZE equ 32
SNAPLIST_STRINGS equ SNAPLIST_PATH + SNAPLIST_PATH_SIZE


ACTION_SNA_SAVE equ 0x06
// Saves the snapshot as a .sna or .z80 file in the place set by ACTION_SNA_SAVE_DIR
// WORD param1 = pointer to file name to save
// BYTE param2 = overwrite flag, force overwrite is non-zero
// BYTE param3 = non-zero to save in the folder of the game last loaded or
//               saved (see ACTION_SNA_SAVE_DIR_OP_LAST) instead
// BYTE param4 = the file type
ACTION_SNA_SAVE_TYPE_AUTO equ 0     // .sna for a 48K machine, .z80 for a 128K one
ACTION_SNA_SAVE_TYPE_SNA equ 1      // (a 128K .sna has no place for the sound chip registers)
ACTION_SNA_SAVE_TYPE_Z80 equ 2
ACTION_SNA_SAVE_TYPE_SZX equ 3
ACTION_SNA_SAVE_TYPE_SAME equ 4     // the kind of file last loaded or saved (a quick-save)
ACTION_SNA_SAVE_NAME_MAX equ 28     // the longest name the save screen takes
// status return is 0 for success, 1 for file exists, anything else is an error


ACTION_ROM_LIST equ 0x07
// as ACTION_SNAP_LIST
// WORD param1  = start number
// WORD param2 = destination to write results
//  
// RESULTS are
//     BYTE countReturned  (will be no more than 10)
//     BYTE moreAviable  (non-zero if true)
//     WORD namepointer[countReturned]
//     String data follows


ACTION_ROM_CHANGE equ 0x08
// WORD param1 = pointer to file name to load
// BYTE param2 = non zero if rom should be writable


ACTION_WRITE_DATA_TO_PICO equ 0x09
// WORD param1 pointer to source location of data (in spectrum rom)
// WORD param2 offset address to write data must be >= 16384
// WORD param3 length of source data


ACTION_READ_DATA_FROM_PICO equ 0x0A
// WORD param1 pointer to destination location (in spectrum rom)
// WORD param2 offset address to read from must be >= 16384
// WORD param3 length of source data


ACTION_SNAP_CHDIR equ 0x0B
// Changes the directory that ACTION_SNAP_LIST lists and that snapshots are
// loaded from. The pico remembers the directory until it is changed again.
// WORD param1 = pointer to the directory name: either a name returned by
//               ACTION_SNAP_LIST or ".." for the parent directory
// BYTE param2 = drive - 0 for internal pico flash, 1 for external sdcard


ACTION_SNAP_FIND equ 0x0C
// Sets the text to search for. While it is set ACTION_SNAP_LIST returns only
// the entries whose names contain it and, on the SD card, looks in all the
// directories below the current one as well. ACTION_SNAP_CHDIR ends the
// search, as does leaving the NMI rom.
// WORD param1 = pointer to the text, an empty string ends the search
// BYTE param2 = drive - 0 for internal pico flash, 1 for external sdcard

FIND_MAXLEN equ 12


ACTION_SNAP_MOVE equ 0x0D
// Moves a snapshot on the SD card from one directory to another.
// WORD param1 = for PICK the name of an entry returned by ACTION_SNAP_LIST,
//               for DESCRIBE where to write three SNAPLIST_PATH_SIZE byte
//               strings to show: the file, where it is and where it would go
// BYTE param2 = drive - 0 internal flash, 1 SD card
// BYTE param3 = one of:
ACTION_SNAP_MOVE_OP_PICK equ 0       // this is the file to move
ACTION_SNAP_MOVE_OP_DESCRIBE equ 1   // describe the move
ACTION_SNAP_MOVE_OP_DO equ 2         // move it to the directory being browsed
ACTION_SNAP_MOVE_OP_CANCEL equ 3     // forget the file picked
ACTION_SNAP_MOVE_OP_COPY equ 4       // copy it to the directory being browsed
ACTION_SNAP_MOVE_OP_FAVOURITE equ 5  // copy the list entry at param1 to /Favourites
                                     // (made if it is not there); status 3 if it
                                     // is in /Favourites already
// status return is 0 for success, 1 if the name is already in use there,
// 2 for any other error, 3 if the file is already in that directory


ACTION_SNAP_MKDIR equ 0x0E
// Makes a directory in the directory being browsed on the SD card.
// WORD param1 = pointer to the name
// BYTE param2 = drive - 0 internal flash, 1 SD card
// status return is 0 for success, 1 if the name is already in use, 2 if
// the name is not allowed or for any other error

ACTION_SNAP_RENAME equ 0x0F
// Renames a snapshot on the SD card, keeping its extension.
// WORD param1 = the name of an entry returned by ACTION_SNAP_LIST
// BYTE param2 = drive - 0 internal flash, 1 SD card
// WORD param3 = for DESCRIBE where to write two SNAPLIST_PATH_SIZE byte
//               strings: the name to start editing from (empty if it is
//               too long to edit) and "Was: " and the whole name to show.
//               For DO, the new name (without the extension).
// BYTE param4 = one of:
ACTION_SNAP_RENAME_OP_DESCRIBE equ 0
ACTION_SNAP_RENAME_OP_DO equ 1
// status return is 0 for success, 1 if the name is already in use, 2 if
// the name is not allowed or for any other error


ACTION_SNAP_ERASE equ 0x10
// Deletes a file, or a directory and everything in it, on the SD card or the internal flash.
// WORD param1 = the name of an entry returned by ACTION_SNAP_LIST
// BYTE param2 = drive (0 internal flash, 1 SD card)
// WORD param3 = for DESCRIBE where to write two SNAPLIST_PATH_SIZE byte
//               strings: the file and the directory it is in.  For DO on a directory
//               the address of the text the user typed, which must be "ok"
// BYTE param4 = one of:
ACTION_SNAP_ERASE_OP_DESCRIBE equ 0
ACTION_SNAP_ERASE_OP_DO equ 1
// status return is 0 for success, 2 for an error.  DESCRIBE returns FILEOP_IS_DIRECTORY
// for a directory, and FILEOP_IS_ROM for a .rom file, which DO only erase with the text "ok" (FILEOP_NOT_CONFIRMED if not)
FILEOP_IS_DIRECTORY equ 4
FILEOP_NOT_CONFIRMED equ 5
FILEOP_IS_ROM equ 6
FILEOP_IS_48ROM equ 7
FILEOP_CANT_MOVE_FOLDER equ 11
FILEOP_CANCELLED equ 12              // a folder copy was stopped with BREAK
FILEOP_IS_TESTROM equ 8
FILEOP_IS_DIAGROM equ 9
FILEOP_IS_PROTECTED equ 10          // the pico wrote four texts after the name and folder: title, 2 lines, what to type

ACTION_SNA_SAVE_DIR equ 0x11
// Where ACTION_SNA_SAVE puts snapshots: the internal flash or a directory of
// the SD card. The pico remembers it, even after power off.
// WORD param1 = for DESCRIBE where to write a SNAPLIST_PATH_SIZE byte string
//               saying where snapshots go
// BYTE param2 = for SET the drive being browsed
// BYTE param3 = one of:
ACTION_SNA_SAVE_DIR_OP_DESCRIBE equ 0   // say where snapshots go
ACTION_SNA_SAVE_DIR_OP_BROWSE equ 1     // start choosing: ACTION_SNAP_LIST lists
                                        // the save directory; status is its drive
ACTION_SNA_SAVE_DIR_OP_SET equ 2        // save to the directory being browsed
ACTION_SNA_SAVE_DIR_OP_CANCEL equ 3     // stop choosing, change nothing
ACTION_SNA_SAVE_DIR_OP_LAST equ 4       // the name of the game last loaded or saved,
                                        // written at param1; status is its length,
                                        // 0 if there is none to save over
ACTION_SNA_SAVE_DIR_OP_QUICK equ 5      // the name for a quick-save of that game: its name and
                                        // " Save Game", written at param1; status is its
                                        // length, 0 if there is no game to name it after
// DESCRIBE with param2 non-zero describes that game's folder instead.
// Both SET and CANCEL put back the directory that was browsed before BROWSE.

ACTION_SNA_BEGIN_WRITE128 equ 0x12
// Starts saving a 128K snapshot.  Instead of ACTION_SNA_BEGIN_WRITE.
// param1 = address of sna_on_entry, which has SAVE128_INFO_SIZE bytes: the 27 byte
// sna header, then save128_extra (the PC, last 7FFD value the ROM wrote, a hash of
// the top 9472 bytes of the paged in bank, and the 16 AY registers).
// Then the eight banks follow with ACTION_SNA_NEXT_WRITE, in page_order.

ACTION_SNA_END_WRITE128 equ 0x13
// All the banks have been sent.  Returns in byte param1 the value that must be
// written to port 0x7FFD to page back in the bank that was there on entry.
// ACTION_SNA_SAVE then saves a .z80 file.

SAVE128_INFO_SIZE equ 27 + 23

// The screen is not kept in the NMI ROM while the menu is open. On entry it is
// sent to the pico, SCREEN_PIECE bytes at a time through spare_space, and it
// comes back the same way on exit. Snapshots take the screen from the pico's
// copy, so spare_space is free for the menu meanwhile.
ACTION_SCREEN_STASH equ 0x16
// param1 (word) = where the piece is (in spare_space)
// param2 (word) = the screen address the piece is from (0x4000 to 0x5AFF)
// param3 (word) = length
ACTION_SCREEN_RESTORE equ 0x17
// as ACTION_SCREEN_STASH, but the pico puts that piece of its copy at param1
ACTION_SNA_WRITE_SCREEN equ 0x18
// adds the pico's copy of the screen (6912 bytes) to the snapshot being saved,
// as an ACTION_SNA_NEXT_WRITE of the screen would

ACTION_SNAP_PREVIEW equ 0x19
// param1 (word) = the name of a file in the list (as ACTION_BEGIN_SNAP_READ)
// param2 (byte) = drive
// The pico reads the file's screen: from a .sna or .z80 snapshot (48K or 128K),
// or a .tap's SCREEN$ block. Status 0 when it has one, which ACTION_PREVIEW_READ
// then fetches; 1 if the file has no screen; 2 if it could not be read.
ACTION_PREVIEW_READ equ 0x1A
// as ACTION_SCREEN_RESTORE, but a piece of the screen ACTION_SNAP_PREVIEW read

ACTION_HELP_PAGE equ 0x1B
// param1 (byte) = the page of the help text to draw, from 0 (a page past the last
// gives the last)
// The pico draws it as a screen, which ACTION_PREVIEW_READ then fetches.
// Status: the number of pages
// A page of the help text may have {tokens} that the pico fills in (the Diagnostics page).

ACTION_PREPARE_LOAD equ 0x1C
// Gets a snapshot ready in the pico without a list entry; the NMI rom then loads it
// with ACTION_BEGIN_SNAP_READ and a name of 0.
// BYTE param1 = PREPARE_LOAD_RELOAD: the file last loaded from the Browse Games list
//               PREPARE_LOAD_RESET_48K: a reset into the 48K ROM
//               PREPARE_LOAD_RESET_128K: a reset into the 128K start up
// Status: 0, or 1 if there is nothing to reload
PREPARE_LOAD_RELOAD equ 0
// BYTE param1 = PREPARE_LOAD_DIAGNOSTIC, BYTE param2 = 0 for testrom.rom (zx-diagnostics) or
// 1 for DiagROM.rom, both on the flash: that ROM is put in place of the Spectrum ROM and a
// reset is made ready. Status 0, 1 if there is no file, 2 if it is short
PREPARE_LOAD_RESET_48K equ 1
PREPARE_LOAD_RESET_128K equ 2
PREPARE_LOAD_DIAGNOSTIC equ 3
// BYTE param1 = PREPARE_LOAD_RECENT, BYTE param2 = the game from 0 in the recent list
PREPARE_LOAD_RECENT equ 4

ACTION_POKE_LIST equ 0x1D
// The cheats in the .pok file of the game last loaded (a file with its name, or the
// name without " Save Game", in the Poke List folder of the SD card).
// WORD param1 = the first cheat to send (0 reads the file again)
// WORD param2 = where to write the page: BYTE count (at most 10), BYTE more,
//               WORD name pointer[10], then the names
// Status: 0, or 1 if there are no cheats

ACTION_GAME_INFO equ 0x1F
// The controls and instructions of a game, from the file Instructions in the root of
// the SD card, drawn as a screen (a small picture of the game's screen, the controls
// beside it, the instructions below) which ACTION_PREVIEW_READ then fetches.
// BYTE param1 = 0: the file last previewed (ACTION_SNAP_PREVIEW); 1: the game last loaded
// BYTE param2 = the page, from 0 (a page past the last gives the last)
// BYTE param3 = the picture, from 0 (mode 0: the game file's screen and its screen grabs)
// Status: the number of pages (never 0) in the low four bits, the number of pictures in
// the high four bits

ACTION_SCREEN_GRAB equ 0x20
// Draws the games played recently as a screen for ACTION_PREVIEW_READ. Status: how many games (0 to 10)
ACTION_RECENT_LIST equ 0x21
// Replaces the Picoface's firmware with the .uf2 file in the root of the SD card.
// BYTE param1 = 0 check the file and draw the question as a screen for ACTION_PREVIEW_READ
// (status 0: ready, else the screen says why not), 1 copy the firmware to the internal
// flash (status 0 done, else a screen), 2 install it (does not come back)
ACTION_UPDATE equ 0x22
// Saves the game's screen (the pico has it from when the menu opened) in the hidden
// Screen Grabs folder of the SD card, named after the game last loaded.
// WORD param1 = where the pico writes its message, "Saved as <name>" (40 bytes)
// Status: 0 saved, 1 no SD card (or the PC has it), 2 could not save, 3 too many grabs
// of the game, 4 no screen to save

ACTION_POKE_GET equ 0x1E
// The pokes of one cheat from the last ACTION_POKE_LIST.
// WORD param1 = the cheat number
// WORD param2 = where to write them: BYTE count (at most POKE_MAX), BYTE non-zero if a
//               value must be chosen by the user, then for each poke
//               WORD address, WORD value (256 = the user's), BYTE bank (0-7 or 8 for none)
// Status: 0, or 1 if there is no such cheat
POKE_MAX equ 16

// The browse screen: banner (row 0), title (1), what the number keys do (2), the
// folder (4), ten entries from LIST_TOP, a bar saying what the letter keys do, then the
// key rows from KEY_ROW
LIST_TOP equ 5
BAR_ROW equ LIST_TOP + 10
KEY_ROW equ BAR_ROW + 1
MENU_ROWS equ KEY_ROW + 6          // the rows clearscreen clears
INFO_ATTR equ 0b01000110           // the instruction lines: bright yellow on black

// Menu colours: the key letter of each option and the option's text
KEY_ATTR equ 0b01000010         // bright red ink, black paper
OPTION_ATTR equ 0b01000100      // bright green ink, black paper
// the Reload item of the menu scrolls a long name, see startmenu.scrollidle
// the 28 letters of it shown now: in spare_space, which nothing uses while the top menu waits for a key
SCROLL_WINDOW equ 0x3C00
    ASSERT SCROLL_WINDOW + SCROLL_WIDTH + 1 <= 0x3D00
SCROLL_ROW equ 6
SCROLL_COL equ 4
SCROLL_WIDTH equ 28
SCROLL_STEP equ 350             // calls of .scrollidle (one pass of the keyboard scan, about 0.7 ms) per letter: 0.25 s
SCROLL_HOLD equ 2000            // and at each end: 1.4 s
// the Recently played list: a name shows RECENT_WINDOW letters from column 2 and the pico leaves
// every name (RECENT_SLOT bytes each) at spare_space, so the longer ones can scroll
RECENT_WINDOW equ 21
RECENT_SLOT equ 64
BANNER_ATTR equ 0b01001111      // the bars across the screens: bright white on blue
TITLE_ATTR equ 0b01001100       // the PICOFACE title on row 0: bright green on blue

FOLDER_MAXLEN equ 28
MOVE_INFO equ 1024          // where in spare_space the move description goes (a list is under 600 bytes)

// The names in the Load snapshot list are shown NAME_WINDOW characters at a
// time from column 4, with the extension in its own column (28-31). Longer
// names scroll: they wait SCROLL_PAUSE steps at each end and move one
// character every SCROLL_DELAY times round the wait-for-a-key loop.
NAME_WINDOW equ 24
// with the file details on (O-Info) the names are narrower: the extension is at column 20
// and the size and 128K flag at 24 to 31
NAME_WINDOW_INFO equ 16
SCROLL_DELAY equ 200
SCROLL_PAUSE equ 6
// the pictures of a game change after this many times round the wait-for-a-key loop
// (about 3 seconds)
PICTURE_DELAY equ 5000
// the cheat names of the poke list, from column 4 to the right edge; longer ones scroll the same way
POKE_NAME_WINDOW equ 28
// the longest name the pico sends, longer ones are cut down in the middle
SNAPLIST_NAME_MAX equ 64



// status return is 0 for success, 1 for file exists, anything else is an error


STARTUP_ACTION_TRANSFER_SNAP equ 0x1
STARTUP_ACTION_LOAD_SNAP equ 0x2

STARTUP_ACTION_SAVE_MEMORY equ 0x3
// PARAM1 is source location
// PARAM2 is total length
STARTUP_ACTION_LOAD_MEMORY equ 0x4
// PARAM1 is destination location
// PARAM2 is total length
STARTUP_ACTION_GRAB equ 0x5
// how long the border stays coloured: that many times 3,300 T-states (about 0.4 s)
GRAB_FLASH equ 420
// a double press of the button: saves the screen with no menu; the border is green for
// saved, red for not, then the game carries on



    MACRO MENU_ITEM key, y, x, msg, handler
        BYTE key
        BYTE y
        BYTE x
        WORD msg
        WORD handler
    ENDM

    MACRO MENU_END
        BYTE 255
    ENDM



    org 0x0
action: defb 0
action_params:

    org 0x38
im1handler:
    ld b, 1
    reti
    //jp startmenu

im2handler:
    ld b, 2
    reti

 

IM2HANDLER_SIZE: equ $ - im2handler 





TEST_AREA: equ 32768
PIXEL_SIZE: equ 192 * 32


nmientry:
    org 0x66
    nop

    ld (sna_on_entry + SNAHEADER.SP), SP
    ld (sna_on_entry + SNAHEADER.HL), HL
    ld (sna_on_entry + SNAHEADER.DE), DE
    ld (sna_on_entry + SNAHEADER.BC), BC
    ld (sna_on_entry + SNAHEADER.IX), IX
    ld (sna_on_entry + SNAHEADER.IY), IY
    ld sp, stack_top

    push af, bc, de, hl, ix

    push af
    pop hl
    ld (sna_on_entry + SNAHEADER.AF), HL
    exx
    ld (sna_on_entry + SNAHEADER.HLx), HL
    ld (sna_on_entry + SNAHEADER.DEx), DE
    ld (sna_on_entry + SNAHEADER.BCx), BC
    exx
    ex af, af'
    push af
    pop hl
    ld (sna_on_entry + SNAHEADER.AFx), HL
    ex af, af'

    ld a, (BORDCR)
    rra
    rra
    rra
    and 0x7
    ld (sna_on_entry + SNAHEADER.BORD), a    


    ld (sna_on_entry + SNAHEADER.IX), IX
    ld (sna_on_entry + SNAHEADER.IY), IY

    xor a
    ld (iff2save), a
    ld (sna_on_entry + SNAHEADER.IFF2), a

    ld a, i
    ld (sna_on_entry + SNAHEADER.I), a
    ;p/v contains iff2, p/v = 1 is even parity
    jp po,  .interruptsweredisbled

    ld a, 1
    ld (iff2save), a
    ld a, 0b100
    ld (sna_on_entry + SNAHEADER.IFF2), a


.interruptsweredisbled:    
    ld a, (start_up_action)

    cp STARTUP_ACTION_LOAD_MEMORY
    jr nz, 1F
    call startupactionloadmemory
    jr .exitnoscreenrestore

1:  cp STARTUP_ACTION_SAVE_MEMORY
    jr nz, 1F
    call startupactionsavememory
    jr .exitnoscreenrestore

1:  cp STARTUP_ACTION_LOAD_SNAP
    jr nz, 1F
    ld hl, 0
    xor a
    call loadsnapshot
    jr .exitnoscreenrestore

    // The pico keeps the screen while the menu is open (snapshots take it
    // from there too)
1:  call screentopico

    ld a, (start_up_action)
    cp STARTUP_ACTION_TRANSFER_SNAP
    jr nz, 1F
    call sendsnapshottopico
    jr .exitnoscreenrestore     // the screen was not touched

1:  cp STARTUP_ACTION_GRAB
    jr nz, 1F
    call quickgrab
    jr .exitnoscreenrestore     // the screen was not touched

1:  call startmenu

.exittopscreen
    call screenfrompico

.exitnoscreenrestore
    pop ix, hl, de, bc

    ld a, (iff2save)
    cp 0
    jr nz, .exitwithei
    pop af
    ld sp, (sna_on_entry + SNAHEADER.SP)
    jr exitnmi
.exitwithei:    
    pop af
    ld sp, (sna_on_entry + SNAHEADER.SP)
ei_exitnmi:
    ei
exitnmi:
    ret

iff2save:
    .byte 0

; start_up_action: BYTE 0    
; start_up_param1: WORD 0
; start_up_param2: WORD 0


start_up_action: BYTE 0  
start_up_param1: WORD 40000
start_up_param2: WORD 20000

start_up_sdcard_present: BYTE 0
// non-zero when the pico started the program that is running and it is a 48K one
// (a 48K snapshot or a tape): it is saved as a 48K snapshot even on a 128K machine,
// and the paging is left alone, since BANK_M is only the 128K ROM's and a 48K
// program's memory there says nothing about the paging
start_up_machine48: BYTE 0
    ASSERT start_up_machine48 == start_up_sdcard_present + 1     // the pico writes it there
// what the L item on the menu says, filled in by the pico each time the menu opens:
// "Reload: <name>", or "No game detected" (64 letters and the end; the menu shows 28 of them
// and scrolls a longer one, see .scrollidle)
start_up_reload_label: DZ "No game detected"
    BLOCK 65 - 17

spare_size_hi .equ (spare_size - 255) / 256


// Double press: the pico saves the screen it was given; the border says how it went
quickgrab:
    ld ix, 0
    ld hl, spare_space
    ld (ix + 2), hl         // where the pico writes its message (not shown)
    ld (ix + 1), ACTION_SCREEN_GRAB
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B
    or a
    ld a, 4                 // green: saved
    jr z, 2F
    ld a, 2                 // red: not saved
2:  out (0xfe), a
    ld de, GRAB_FLASH
3:  ld b, 0
4:  djnz 4B
    dec de
    ld a, d
    or e
    jr nz, 3B
    ld a, (sna_on_entry + SNAHEADER.BORD)
    and 7
    out (0xfe), a
    ret

startupactionloadmemory:
    ld de, (start_up_param1)
    ld bc, (start_up_param2)
    ld ix, 0

.transfer:
    ld a, spare_size_hi
    cp b
    jr nc, .last_block
    push bc
    ld b, spare_size_hi

    // WORD param1 pointer to source location of data (in spectrum rom)
    // WORD param2 offset address to write data must be >= 16384
    // WORD param3 length of source data
    ld hl, spare_space
    ld (ix + 2), hl
    ld (ix + 4), de
    ld (ix + 6), bc
    ld (ix + 1), ACTION_READ_DATA_FROM_PICO
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B 
    ldir

    pop bc

    sub hl, de

    ld a, b
    sub spare_size_hi
    ld b, a
    ld c, 0
    jr .transfer

.last_block:
    // last block could be nothing so return
    ld a, b
    or c
    ret z

    ld hl, spare_space
    ld (ix + 2), hl
    ld (ix + 4), de
    ld (ix + 6), bc
    ld (ix + 1), ACTION_READ_DATA_FROM_PICO
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B 
    ldir
    ret


startupactionsavememory:
    ld hl, (start_up_param1)
    ld bc, (start_up_param2)
    ld ix, 0

.transfer:
    

    ld a, spare_size_hi
    cp b
    jr nc, .last_block
    push bc
    ld b, spare_size_hi


    // WORD param1 pointer to source location of data (in spectrum rom)
    // WORD param2 offset address to write data must be >= 16384
    // WORD param3 length of source data
    ld de, spare_space
    ld (ix + 2), de
    ld (ix + 4), hl
    ld (ix + 6), bc
    ld (ix + 1), ACTION_WRITE_DATA_TO_PICO
    ldir
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B
    pop bc

    ld a, b
    sub spare_size_hi
    ld b, a
    ld c, 0
    jr .transfer

.last_block:
    // last block could be nothing so return
    ld a, b
    or c
    ret z

    ld de, spare_space
    ld (ix + 2), de
    ld (ix + 4), hl
    ld (ix + 6), bc
    ld (ix + 1), ACTION_WRITE_DATA_TO_PICO
    ldir
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B
    ret


; -------------------------------------------
; Start up menu

startmenu:

.showtopscreen
    call .scrollinit
    ld hl, .startmenu_def
    call showmenu
    call bigtitle
    ld hl, .startmenu_def
    ld de, 0                    // no handler for other keys
    ld bc, .scrollidle          // while waiting for a key
    jp menuhandler_with_idle

// The Reload item says "Reload: <name>". The name can be longer than the 28 letters that fit after
// "R-", so then the text scrolls: it waits at the start, moves one letter at a time to the end,
// waits there and starts again. SCROLL_WINDOW holds the 28 letters shown now.

.scrollinit:
    xor a
    ld (.scrollpos), a
    ld hl, start_up_reload_label
    ld b, 0
1:  ld a, (hl)
    or a
    jr z, 2F
    inc hl
    inc b
    ld a, b
    cp 64
    jr c, 1B
2:  ld a, b
    ld (.scrolllen), a
    ld hl, SCROLL_HOLD
    ld (.scrolltimer), hl
// the window at the position in .scrollpos
.scrollfill:
    ld a, (.scrollpos)
    ld e, a
    ld d, 0
    ld hl, start_up_reload_label
    add hl, de
    ld de, SCROLL_WINDOW
    ld b, SCROLL_WIDTH
1:  ld a, (hl)
    or a
    jr z, 2F
    ld (de), a
    inc hl
    inc de
    djnz 1B
2:  xor a
    ld (de), a
    ret

// called over and over while the menu waits for a key (any register may change)
.scrollidle:
    ld a, (.scrolllen)
    cp SCROLL_WIDTH + 1
    ret c                       // it fits: nothing to scroll
    ld hl, (.scrolltimer)
    dec hl
    ld (.scrolltimer), hl
    ld a, h
    or l
    ret nz
    ld a, (.scrolllen)
    sub SCROLL_WIDTH            // the last position
    ld b, a
    ld a, (.scrollpos)
    cp b
    jr c, 1F
    xor a                       // at the end: round to the start
    ld hl, SCROLL_HOLD
    jr 2F
1:  inc a                       // one letter on
    ld hl, SCROLL_STEP
    cp b
    jr nz, 2F
    ld hl, SCROLL_HOLD          // reached the end
2:  ld (.scrollpos), a
    ld (.scrolltimer), hl
    call .scrollfill
    ld a, OPTION_ATTR
    ld (text_attr), a
    ld de, SCROLL_ROW * 256 + SCROLL_COL
    ld hl, SCROLL_WINDOW
    call putstring
    xor a
    ld (text_attr), a
    ret

.scrollpos BYTE 0
.scrolllen BYTE 0
.scrolltimer WORD 0

.handle_save:
    call savesnapshotscreen
    cp 0
    jp nz, .showtopscreen
    ret

.handle_exit:
    ret

// Q: save to "<game> Save Game" in the game's folder, with no questions apart from
// overwriting. Without a game to name it after it is the ordinary Save screen.
.handle_quicksave:
    ld a, 1
    ld (savesnapshotscreen.quick), a
    call savesnapshotscreen
    cp 0
    jp nz, .showtopscreen
    ret

// L: load the game that was last loaded from Browse Games again
.handle_reload:
    ld a, PREPARE_LOAD_RELOAD
    call .prepareload
    jr z, .doload
    ld hl, .noreloadmsg
    call loadscreen.message
    jp .showtopscreen
.doload
    ld hl, 0                // no list entry: what the pico has ready
    xor a
    jp loadsnapshot

// Asks the pico to get what a (PREPARE_LOAD_*) ready. Returns z if it has.
.prepareload
    ld ix, 0
    ld (ix + 2), a
    ld (ix + 1), ACTION_PREPARE_LOAD
    call loadscreen.waitpico
    or a
    ret

.handle_reset48:
    ld hl, .reset48msg
    call .confirmreset
    jp nz, .showtopscreen
    ld a, PREPARE_LOAD_RESET_48K
    jp .doreset
// T: reset the Spectrum into a diagnostic ROM kept on the flash: testrom.rom (zx-diagnostics)
// or DiagROM.rom (Phil Ruston's DiagROM), whichever is chosen first
.handle_test:
    call clearscreen
    ld de, 0x0101
    ld hl, .testchoosetitle
    call putstring
    ld de, 0x0301
    ld hl, .testchoose
    call putstring
    ld de, 0x0501
    ld hl, .testchoose1
    call putstring
    ld de, 0x0601
    ld hl, .testchoose2
    call putstring
    ld de, 0x0801
    ld hl, .testchoosex
    call putstring
.testchoosekey:
    call waitforkeypress
    ld c, 0
    cp '1'
    jr z, .testchosen
    inc c
    cp '2'
    jr z, .testchosen
    cp 'x'
    jr z, .testchoosequit
    cp 0x18
    jr nz, .testchoosekey
.testchoosequit:
    call waitforkeyrelease
    jp .showtopscreen
.testchosen:
    ld a, c
    ld (.testwhich), a
    call waitforkeyrelease
    call clearscreen
    ld de, 0x0101
    ld hl, .resettitle
    call putstring
    ld de, 0x0301
    ld hl, .testmsg
    ld a, (.testwhich)
    or a
    jr z, 1F
    ld hl, .testmsg2
1:  call putstring
    ld de, 0x0501
    ld hl, .resetlost
    call putstring
    ld de, 0x0701
    ld hl, .testexit1
    call putstring
    ld de, 0x0801
    ld hl, .testexit2
    call putstring
    ld de, 0x0901
    ld hl, .testexit3
    call putstring
    ld de, 0x0B01
    ld hl, .resetask
    call putstring
    call .resetkey
    jp nz, .showtopscreen
    ld ix, 0
    ld a, (.testwhich)
    ld (ix + 3), a
    ld a, PREPARE_LOAD_DIAGNOSTIC
    call .prepareload
    jp z, .doload
    ld b, a                 // the status the pico gave: 1 no file, 2 too short
    ld a, (.testwhich)
    or a
    jr nz, 3F
    ld hl, .notestmsg
    ld a, b
    cp 2
    jr nz, 4F
    ld hl, .badtestmsg
    jr 4F
3:  ld hl, .nodiagmsg
    ld a, b
    cp 2
    jr nz, 4F
    ld hl, .baddiagmsg
4:  call loadscreen.message
    jp .showtopscreen
.testwhich: BYTE 0
.handle_reset128:
    ld hl, .reset128msg
    call .confirmreset
    jp nz, .showtopscreen
    ld a, PREPARE_LOAD_RESET_128K
.doreset
    call .prepareload
    jp .doload

// Asks whether to reset the Spectrum, with the question's second line at hl.
// Returns z for yes.
.confirmreset
    push hl
    call clearscreen
    ld de, 0x0101
    ld hl, .resettitle
    call putstring
    ld de, 0x0301
    pop hl
    call putstring
    ld de, 0x0501
    ld hl, .resetlost
    call putstring
    ld de, 0x0701
    ld hl, .resetask
    call putstring
.resetkey
    call waitforkeypress
    cp 'y'
    jr z, .resetyes
    cp 'n'
    jr z, .resetno
    cp 0x18
    jr nz, .resetkey
.resetno
    call waitforkeyrelease
    or 1                    // nz
    ret
.resetyes
    call waitforkeyrelease
    xor a
    ret

// H: the help pages. The pico draws each page as a screen; N or SPACE goes to the
// next, P to the previous, X, ENTER or BREAK back to the menu.
.handle_help:
    xor a
    ld (.helppage), a
.showhelp:
    ld ix, 0
    ld a, (.helppage)
    ld (ix + 2), a
    ld (ix + 1), ACTION_HELP_PAGE
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B
    ld (.helppages), a
    call previewfrompico
.helpkey:
    call waitforkeypress
    push af
    call waitforkeyrelease
    pop af
    ld hl, .helppage
    cp 'n'
    jr z, .helpnext
    cp ' '
    jr z, .helpnext
    cp 8                    // CAPS SHIFT+8, cursor right
    jr z, .helpnext
    cp 'p'
    jr z, .helpprev
    cp 5                    // CAPS SHIFT+5, cursor left
    jr z, .helpprev
    cp 'd'                  // the Diagnostics page, the last one
    jr z, .helpdiag
    cp 'x'
    jr z, .helpexit
    cp 0x13                 // ENTER
    jr z, .helpexit
    cp 0x18                 // BREAK
    jr z, .helpexit
    jr .helpkey
.helpnext:
    ld a, (.helppages)
    dec a
    cp (hl)
    jr z, .helpkey          // already on the last page
    inc (hl)
    jr .showhelp
.helpprev:
    ld a, (hl)
    or a
    jr z, .helpkey          // already on the first page
    dec (hl)
    jr .showhelp
.helpdiag:
    ld a, (.helppages)
    dec a
    ld (hl), a
    jr .showhelp
.helpexit:
    call screenfrompico     // the game's screen again under the menu
    jp .showtopscreen
.helppage: BYTE 0
.helppages: BYTE 0

// C: set up the SD card. The pico draws it, and shows the same keys as the game information
.handle_sdsetup:
    ld a, 2
    jr .infocall

// I: the controls and instructions of the game last loaded
.handle_info:
    ld a, 1
.infocall:
    call showgameinfo
    call screenfrompico     // the game's screen again under the menu
    jp .showtopscreen

// E: the last ten games loaded, 1 to 9 and 0 load one
.handle_recent:
    xor a
    ld (.recentmode), a
.recentdraw:
    ld ix, 0
    ld a, (.recentmode)
    ld (ix + 2), a          // 0 the list, 1 the list asking which to view
    ld hl, spare_space
    ld (ix + 3), hl         // where the full names go
    ld (ix + 1), ACTION_RECENT_LIST
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B
    ld (.recentcount), a
    call previewfrompico
    call .recentscrollinit
.recentkey:
    call .recentidle
    call getkeypress
    or a
    jr z, .recentkey
    cp 255
    jr z, .recentkey
    push af
    call waitforkeyrelease
    pop af
    cp 'x'
    jr z, .recentback
    cp 0x13                 // ENTER
    jr z, .recentback
    cp 0x18                 // BREAK
    jr z, .recentback
    cp 'v'
    jr nz, 2F
    ld a, (.recentcount)
    or a
    jr z, .recentkey        // nothing to view
    ld a, 1
    ld (.recentmode), a
    jr .recentdraw
2:  cp '0'
    jr nz, 1F
    ld a, ':'               // 0 is the tenth
1:  sub '1'
    jr c, .recentkey
    ld c, a
    ld hl, .recentcount
    cp (hl)
    jr nc, .recentkey       // no game with that number
    ld a, c
    ld (.recentindex), a
    ld a, (.recentmode)
    or a
    jr nz, .recentview
.recentload:
    ld ix, 0
    ld a, (.recentindex)
    ld (ix + 3), a
    ld a, PREPARE_LOAD_RECENT
    call .prepareload
    jp z, .doload
    ld hl, .norecentmsg
    call loadscreen.message
.recentexit:
    call screenfrompico     // the game's screen again under the menu
    jp .showtopscreen
// X: from the question which to view back to the list, from the list to the menu
.recentback:
    ld a, (.recentmode)
    or a
    jr z, .recentexit
    xor a
    ld (.recentmode), a
    jp .recentdraw
// V then a number: its controls and instructions (ENTER on them loads the game)
.recentview:
    ld ix, 0
    ld a, (.recentindex)
    add a, 2
    ld (ix + 2), a
    ld (ix + 1), ACTION_RECENT_LIST
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B
    xor a                   // the file just previewed
    call showgameinfo
    cp 0x13                 // ENTER
    jr z, .recentload
    xor a
    ld (.recentmode), a
    jp .recentdraw
// The long names scroll together: all wait SCROLL_HOLD at the start, move a letter every SCROLL_STEP
// passes, wait at the end and go back (a shorter name stops at its own end)
.recentscrollinit:
    xor a
    ld (.recentpos), a
    ld (.recentmax), a
    ld hl, SCROLL_HOLD
    ld (.recenttimer), hl
    ld a, (.recentcount)
    or a
    ret z
    ld b, a
    ld hl, spare_space
1:  push bc
    ld (.recentslot), hl
    ld c, 0
2:  ld a, (hl)
    or a
    jr z, 3F
    inc hl
    inc c
    jr 2B
3:  ld a, (.recentmax)
    cp c
    jr nc, 4F
    ld a, c
    ld (.recentmax), a
4:  ld hl, (.recentslot)
    ld de, RECENT_SLOT
    add hl, de
    ld (.recentslot), hl
    pop bc
    djnz 1B
    ret
.recentidle:
    ld a, (.recentmax)
    cp RECENT_WINDOW + 1
    ret c                       // every name fits
    ld hl, (.recenttimer)
    dec hl
    ld (.recenttimer), hl
    ld a, h
    or l
    ret nz
    ld a, (.recentmax)
    sub RECENT_WINDOW           // the last position
    ld b, a
    ld a, (.recentpos)
    cp b
    jr c, 1F
    xor a                       // at the end: round to the start
    ld hl, SCROLL_HOLD
    jr 2F
1:  inc a
    ld hl, SCROLL_STEP
    cp b
    jr nz, 2F
    ld hl, SCROLL_HOLD
2:  ld (.recentpos), a
    ld (.recenttimer), hl
// draws each name longer than the window from the letter .recentpos (or its last place)
.recentpaint:
    xor a
    ld (text_attr), a           // the colours stay
    ld a, (.recentcount)
    or a
    ret z
    ld b, a
    ld hl, spare_space
    ld c, 2                     // the screen row
1:  ld (.recentslot), hl
    ld e, 0
2:  ld a, (hl)
    or a
    jr z, 3F
    inc hl
    inc e
    jr 2B
3:  ld hl, (.recentslot)
    ld a, e
    sub RECENT_WINDOW
    jr c, 8F                    // it fits
    jr z, 8F
    ld e, a
    ld a, (.recentpos)
    cp e
    jr c, 4F
    ld a, e
4:  ld e, a
    ld d, 0
    add hl, de
    push bc
    ld d, c
    ld e, 2
    ld b, RECENT_WINDOW
5:  ld c, (hl)
    inc hl
    call putchar
    djnz 5B
    pop bc
8:  ld hl, (.recentslot)
    ld de, RECENT_SLOT
    add hl, de
    inc c
    djnz 1B
    ret
.recentpos: BYTE 0
.recentmax: BYTE 0
.recenttimer: WORD 0
.recentslot: WORD 0
.recentcount: BYTE 0
.recentmode: BYTE 0
.recentindex: BYTE 0

// U: install the firmware from the .uf2 file in the root of the SD card
.handle_update:
    xor a
    call .updaterequest     // check the file, draw the question
    ld (.updatestatus), a
    call previewfrompico
.updatekey:
    call getkeypress
    or a
    jr z, .updatekey
    cp 255
    jr z, .updatekey
    push af
    call waitforkeyrelease
    pop af
    cp 'y'
    jr nz, 1F
    ld a, (.updatestatus)
    or a
    jr z, .updatego
    jr .updatekey
1:  cp 'x'
    jr z, .updateexit
    cp 'n'
    jr z, .updateexit
    cp 0x13                 // ENTER
    jr z, .updateexit
    cp 0x18                 // BREAK
    jr nz, .updatekey
.updateexit:
    call screenfrompico     // the game's screen again under the menu
    jp .showtopscreen
.updatego:
    call clearscreen
    ld de, 0x0801
    ld hl, .updatingmsg
    call putstring
    ld de, 0x0A01
    ld hl, .updatestep1
    call putstring
    xor a
    call .updatebar
.stageloop:
    ld a, 1
    call .updaterequest     // copies a part of the firmware inside the Pico
    cp 8                    // more to do
    jr nz, .stagedone
    ld a, (ix + 3)          // how much of the bar
    call .updatebar
    jr .stageloop
.stagedone:
    or a
    jr z, 1F
.updateshowwhy:
    ld (.updatestatus), a
    call previewfrompico    // why not
    jp .updatekey
1:  ld de, 0x0A01
    ld hl, .updatestep2
    call putstring
    ld a, 28
    call .updatebar
    xor a
    call .updatebar
    ld ix, 0                // the Pico writes the bar's progress in byte 3 while byte 0 stays 255
    ld (ix + 3), 0
    ld (ix + 2), 2
    ld (ix + 1), ACTION_UPDATE
    ld (ix + 0), 255
1:  ld a, (ix + 3)
    call .updatebar
    ld a, (ix)
    cp 255
    jr z, 1B
    or a
    jr z, .updatedone
    cp 9
    jr nz, .updateshowwhy   // nothing was changed
    call clearscreen        // 9: the program may be damaged
    ld de, 0x0401
    ld hl, .updatefailmsg
    call putstring
    ld de, 0x0601
    ld hl, .updatefail2
    call putstring
    ld de, 0x0701
    ld hl, .updatefail3
    jr .updatestop
.updatedone:
    call clearscreen
    ld de, 0x0401
    ld hl, .updatedonemsg
    call putstring
    ld de, 0x0601
    ld hl, .updatedone2
    call putstring
    ld de, 0x0701
    ld hl, .updatedone3
.updatestop:
    call putstring
    ld hl, .updatespin      // the Pico is about to restart: wait in RAM, not in its ROM
    ld de, 0x8000
    ld bc, 3
    ldir
    jp 0x8000
.updatespin: BYTE 0xF3, 0x18, 0xFE      // di, jr $
// A = cells of the bar to fill (0 to 28), on row 12
.updatebar:
    ld d, a
    ld hl, SCREEN_ATTRIB + 12 * 32 + 2
    ld b, 28
1:  ld a, d
    or a
    jr z, 2F
    dec d
    ld a, 0b01100000        // bright green
    jr 3F
2:  ld a, 0b00001000        // blue
3:  ld (hl), a
    inc hl
    djnz 1B
    ret
// A = what to do, returns the status
.updaterequest:
    ld ix, 0
    ld (ix + 2), a
    ld (ix + 1), ACTION_UPDATE
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B
    ret
.updatestatus: BYTE 0

// G: saves the game's screen in the hidden Screen Grabs folder on the SD card
.handle_grab:
    ld ix, 0
    ld hl, spare_space
    ld (ix + 2), hl         // where the pico writes "Saved as <name>"
    ld (ix + 1), ACTION_SCREEN_GRAB
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B
    ld hl, spare_space
    or a
    jr z, 2F
    ld hl, .grabnocard
    dec a
    jr z, 2F
    ld hl, .grabtoomany
    cp 2                    // status 3
    jr z, 2F
    ld hl, .grabnoscreen
    cp 3                    // status 4
    jr z, 2F
    ld hl, .graberror
2:  push hl
    call clearscreen
    pop hl
    ld de, 0x0301
    call putstring
    ld de, 0x0501
    ld hl, .grabanykey
    call putstring
    call waitforkeypress
    call waitforkeyrelease
    call screenfrompico     // the game's screen again under the menu
    jp .showtopscreen

.handle_load:
    call loadscreen
    jp .showtopscreen

.handle_change_rom:
    call changeromscreen
    jp .showtopscreen

.handle_poke:
    call pokescreen
    jp .showtopscreen

.startmenu_def:
    MENU_ITEM 's', 3, 2, .savemsg, .handle_save
    MENU_ITEM 'q', 4, 2, .quickmsg, .handle_quicksave
    MENU_ITEM 'b', 5, 2, .loadmsg, .handle_load
    MENU_ITEM 'r', 6, 2, SCROLL_WINDOW, .handle_reload
    MENU_ITEM 'e', 7, 2, .recentmsg, .handle_recent
    MENU_ITEM 'c', 8, 2, .chamgemsg, .handle_change_rom
    MENU_ITEM 'p', 9, 2, .pokemsg, .handle_poke
    MENU_ITEM '4', 10, 2, .reset48menumsg, .handle_reset48
    MENU_ITEM '1', 11, 2, .reset128menumsg, .handle_reset128
    MENU_ITEM 'h', 12, 2, .helpmsg, .handle_help
    MENU_ITEM 'i', 13, 2, .infomsg, .handle_info
    MENU_ITEM 'g', 14, 2, .grabmsg, .handle_grab
    MENU_ITEM 'd', 15, 2, .sdsetupmsg, .handle_sdsetup
    MENU_ITEM 'u', 16, 2, .updatemsg, .handle_update
    MENU_ITEM 't', 17, 2, .testromshow, .handle_test
    MENU_ITEM 'x', 18, 2, .exitmsg,  .handle_exit
    MENU_END
.quickmsg: DZ "Quick save"
.testromshow: DZ "Test ROM (diagnostics)"
.testmsg: DZ "into the zx-diagnostics ROM?"
.testmsg2: DZ "into the DiagROM?"
.testchoosetitle: DZ "Test ROM"
.testchoose: DZ "Which test ROM?"
.testchoose1: DZ "1-zx-diagnostics (testrom)"
.testchoose2: DZ "2-DiagROM (Phil Ruston)"
.testchoosex: DZ "X-Back to the menu"
.nodiagmsg: DZ "Save DiagROM.rom on the flash"
.baddiagmsg: DZ "DiagROM.rom is too short"
.testexit1: DZ "To exit the test ROM press"
.testexit2: DZ "the NMI button, choose Change"
.testexit3: DZ "ROM, then choose Internal Rom."
.notestmsg: DZ "Save testrom.rom on the flash"
.badtestmsg: DZ "testrom.rom is too short"
.reset48menumsg: DZ "Reset to 48K"
.reset128menumsg: DZ "Reset to 128K"
.noreloadmsg: DZ "No game loaded"
.resettitle: DZ "Reset the Spectrum"
.reset48msg: DZ "into 48K mode (48 BASIC)?"
.reset128msg: DZ "into 128K mode (the menu)?"
.resetlost: DZ "The running game is lost."
.resetask: DZ "Y-Yes, reset it   N-No"
.helpmsg: DZ "Help"
.infomsg: DZ "Game Instructions"
.recentmsg: DZ "Recently played"
.updatemsg: DZ "Update PicoMenu"
.sdsetupmsg: DZ "Set up SD card"
.updatingmsg: DZ "Updating - do not switch off"
.updatestep1: DZ "1 of 2  Copying the update  "
.updatestep2: DZ "2 of 2  Writing the PicoMenu"
.updatedonemsg: DZ "Update complete"
.updatedone2: DZ "Switch the Spectrum off"
.updatedone3: DZ "and on again."
.updatefailmsg: DZ "Update failed"
.updatefail2: DZ "Hold BOOTSEL, plug in USB,"
.updatefail3: DZ "copy the .uf2 to RPI-RP2."
.norecentmsg: DZ "Cannot find that game"
.grabmsg: DZ "Screen grab"
.grabnocard: DZ "No SD card (or the PC has it)"
.graberror: DZ "Could not save the grab"
.grabtoomany: DZ "Too many grabs"
.grabnoscreen: DZ "No screen to save"
.grabanykey: DZ "Press any key"
.savemsg: DZ "Save snapshot"
.loadmsg: DZ "Browse Games"
.chamgemsg: DZ "Change ROM"
.exitmsg: DZ "Exit"
.pokemsg: DZ "Poke"



sna_on_entry: BLOCK SNAHEADER
// filled in by savesnapshot, must follow sna_on_entry
save128_extra:
.pc: BLOCK 2        // the PC that NMI pushed
.bankm: BLOCK 1     // BANK_M, the 7FFD value the ROM last wrote
.s1: BLOCK 2        // hash of the bank that was paged in at 0xC000, see savesnapshot
.s2: BLOCK 2
.ay: BLOCK 16       // AY registers 0 to 15
    ASSERT $ - sna_on_entry == SAVE128_INFO_SIZE

BANK_M equ 0x5B5C
HASH_START equ 0xDB00       // the hash skips the part of bank 5 that the menu draws on
HASH_LEN equ 0x10000 - HASH_START




// HL points to menu info
showmenu:
    push hl
    call clearscreen
    pop hl

1:  xor a                   // titles are plain white
    ld (text_attr), a
    ld a, (hl)
    cp 255   // 255 indicates end of menu items
    ret z
    // bit 7 set indicates to ignore this item
    bit 7, a
    jr z, 3F
    ld de, 7
    add hl, de
    jr 1B

3:  inc hl
    ld d, (hl)
    inc hl
    ld e, (hl)
    inc hl
    cp 0
    jr z, .dont_show_char

    


    // if key is lower case then show it as a capital
    cp 'a'
    jr c, 2F
    cp 'z' + 1
    jr nc, 2F
    add a, 'A' - 'a'
2:   
    ld c, a
    ld a, KEY_ATTR
    ld (text_attr), a
    call putchar
    ld c, '-'
    call putchar
    ld a, OPTION_ATTR
    ld (text_attr), a

.dont_show_char:    
    ld c, (hl)
    inc hl
    ld b, (hl)
    inc hl
    inc hl
    inc hl

    ld a, b    // don't show message if NULL
    or c
    jr z, 1B


    push hl
    ld hl, bc
    call putstring
    pop hl
    jr 1B




// hl points to menu on entry
menukeyhandler:
    ld de, 0

; de points to unmatched key handler. This routine is called
; with the c register set the key press
menuhandler_with_keydown_handler:
    ld bc, 0

; bc points to a routine called over and over while waiting for a key, or
; is 0 for none. It may change any register.
menuhandler_with_idle:
    ld (.idle_handler), bc
    ld (.keydown_handler), de

.scan_next_key
    push hl
    call .waitforkey
    pop hl
    push hl

    // c is now the key pressed
    ld c, a

.checkitems
    ld a, (hl)
    cp 255
    jr z, .checkdone
    bit 7, a
    jr nz, .continuecheck
    cp c
    jr z, .foundmatch
.continuecheck
    ld de, 7
    add hl, de
    jr .checkitems

.checkdone
    ld hl, (.keydown_handler)
    ld a, l
    or h
    jr z, 1F
    call .jp_hl
1: call waitforkeyrelease
    pop hl
    jr .scan_next_key
.jp_hl jp (hl)

.foundmatch
    push bc    // we want to restore c because that holds the keypressed

    inc hl
    ld d, (hl)
    inc hl
    ld e, (hl)
    inc hl
    inc hl
    inc hl
    ld c, (hl)
    inc hl
    ld b, (hl)

    push bc
    push de
    call reverseattr
    call waitforkeyrelease
    pop de
    call reverseattr

    pop hl  // handler routine
    pop bc // c is the key pressed

    ld a, h
    or l
    jr z, .nohandler
    pop de // this is the menu pointer that we no longer need
    ld a, c
    jp (hl)

.nohandler
    pop hl
    jr .scan_next_key


.keydown_handler WORD 0
.idle_handler WORD 0

// waitforkeypress, calling the idle routine while no key is down
.waitforkey
    call getkeypress
    cp 0
    jr z, 1F
    cp 255
    ret nz
1:  ld hl, (.idle_handler)
    ld a, h
    or l
    jr z, .waitforkey
    call .jp_hl
    jr .waitforkey


; hl points to list data
; de points to first menu item to update
; it expects the order to be previous, next, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0
updatemenulist:
    ld bc, 7
    ex hl, de
    ; check for previous 
    ld a, (ix + 2)
    or (ix + 3)
    jr z, .noprev
    res 7, (hl)
    jr 1F
.noprev
    set 7, (hl)
1:
    add hl, bc
    inc de
    ld a, (de)
    cp 0
    jr z, .nomore
    res 7, (hl)
    jr 1F
.nomore:
    set 7, (hl)
1:
    add hl, bc
    ex hl, de
    dec hl

    ld c, 255
    ld a, 10
    sub (hl)
    inc hl
    inc hl

    ld b, 10
1:
    cp b
    ex de, hl
    jr nc, .hideitem
    res 7, (hl)
    ex de, hl
    inc de
    inc de
    inc de
    ldi   
    ldi  ; ignore the decrement of bc because c is large enought that it doesn't affect b
    jr 2F
.hideitem
    set 7, (hl)
    ex de, hl
.advancetonext
    inc hl
    inc hl
    .5 inc de
2:
    inc de
    inc de
    djnz 1B

    ret

changeromscreen:
    ld ix, 0
    ld de, 0

.fetchlist
    ld (ix + 1), ACTION_ROM_LIST
    ld (ix + 2), de
    ld (.startpos), de
    ld hl, spare_space
    ld (ix + 4), hl
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B    


.updatefromdata
    ld de, .menuprev
    call updatemenulist

    ld hl, .menu
    call showmenu
.redisplay
    ld a, (.writable)
    cp 0
    jr z, 1F
    ld a, 'x'-' '
1:  add ' '
    ld c, a
2:  ld de, 0x0F0E
    call putchar


    ld hl, .menu
    jp menukeyhandler

.exithandler:
    ret

.nextkeyhandler:
    ld hl, (.startpos)
    ld de, 10
    add hl, de
    ex hl, de
    jp .fetchlist

.prevkeyhandler:
    ld hl, (.startpos)
    ld de, 10
    sub hl, de
    ex hl, de
    jp .fetchlist


.romhandlerzero
    ld a, 9
    jr 1F

.romhandler:
    sub '1'
1:
    ld b, 0
    ld c, a
    ld hl, spare_space + 2
    add hl, bc
    add hl, bc
    ld e, (hl)
    inc hl
    ld d, (hl)
    ex hl, de
    ld a, (.writable)
    call changerom
    jr .redisplay

.writehandler:
    ld a, (.writable)
    xor 1
    ld (.writable), a
    jr .redisplay

.writable BYTE 0
.startpos WORD 0
.titlemsg DZ "Change Rom"
.snap0 BYTE 0
.exit DZ "Exit"
.next DZ "Next"
.prev DZ "Previous"
.writablemsg DZ "Writable [ ]"

.menu     
    MENU_ITEM 0, 1, 1, .titlemsg, 0
    MENU_ITEM 'w', 15, 2, .writablemsg, .writehandler
    MENU_ITEM 'x', 14, 2, .exit, .exithandler
.menuprev
    MENU_ITEM 'p', 14, 9, .prev, .prevkeyhandler
    MENU_ITEM 'n', 14, 20, .next, .nextkeyhandler
    MENU_ITEM '1', 3, 2, .snap0, .romhandler
    MENU_ITEM '2', 4, 2, .snap0, .romhandler
    MENU_ITEM '3', 5, 2, .snap0, .romhandler
    MENU_ITEM '4', 6, 2, .snap0, .romhandler
    MENU_ITEM '5', 7, 2, .snap0, .romhandler
    MENU_ITEM '6', 8, 2, .snap0, .romhandler
    MENU_ITEM '7', 9, 2, .snap0, .romhandler
    MENU_ITEM '8', 10, 2, .snap0, .romhandler
    MENU_ITEM '9', 11, 2, .snap0, .romhandler
    MENU_ITEM '0', 12, 2, .snap0, .romhandlerzero
    MENU_END


// Entry with HL pointing to rom name
// and a holding the writable flag (non zero for a load rom as writable)
changerom:
    ld (ix + 4), a
    ld (ix + 2), hl
    ld (ix + 1), ACTION_ROM_CHANGE
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B

    cp 0
    ret nz

    // Use some self modifying code to jump to address zero
    // When the instruction has been fully read the pico knows
    // so disable the nmi rom and enter the new rom (or the internal one)
    // as address 0
    ld a, 0xC3 // JMP instruction
    ld (exitnmi - 2), a
    xor a
    ld (exitnmi - 1), a
    ld (exitnmi - 0), a
    jp exitnmi - 2



; -------------------------------------------
; Load snapshot menu
loadscreen:
    ld a, (start_up_sdcard_present)
    cp 1
    jr z, 1F
    // disable the drive menu items because we don't need them
    ld a, 0x80
    ld (.menusd), a
    ld (.menuint), a
    xor a                   // and use the internal flash
    ld (.drive), a
1:
    // Save snapshot uses this screen to choose where snapshots go
    ld hl, .titlemsg
    ld a, (.choosing)
    or a
    jr z, 1F
    xor a
    ld (.findtext), a
    ld ix, 0
    ld (ix + 5), ACTION_SNA_SAVE_DIR_OP_BROWSE
    ld (ix + 1), ACTION_SNA_SAVE_DIR
    call .waitpico
    ld (.drive), a
    ld hl, .choosetitle
1:  ld (.menu + 3), hl
    ld de, 0

.fetchlist
    // reading the results of a search can take a while
    ld a, (.findtext)
    or a
    jr z, 1F
    push de
    ld hl, .findbusy
    call .row13
    pop de
1:
    call .request

.updatefromdata
    // only offer "Up" when we are in a sub directory (hl must be preserved)
    ld a, (spare_space + SNAPLIST_HASPARENT)
    or a
    ld a, 'u'
    jr nz, 1F
    or 0x80
1:  ld (.menuup), a
    // F is "Find", or "Clear" when a search is in force
    ld hl, .find
    ld de, .findhandler
    ld a, (.findtext)
    or a
    jr z, 1F
    ld hl, .clear
    ld de, .clearhandler
1:  ld (.menufind + 3), hl
    ld (.menufind + 5), de

    // H (save here) when choosing where to save
    ld a, (.choosing)
    or a
    ld a, 'h'
    jr nz, 1F
    or 0x80
1:  ld (.menuhere), a
    // V (view) except when choosing
    ld a, (.choosing)
    or a
    ld a, 'v'
    jr z, 1F
    or 0x80
1:  ld (.menuview), a

    // The file keys, on the SD card and the internal flash:
    //   normal              M-Move R-Rename E-Erase C-Copy, and D-New folder
    //   moving a file       M-Move here, Q-Cancel
    //   copying a file      C-Copy here, Q-Cancel
    //   choosing where to save, D-New folder only
    // Start with them all hidden.
    ld a, 'm' | 0x80
    ld (.menumove), a
    ld a, 'r' | 0x80
    ld (.menurename), a
    ld a, 'e' | 0x80
    ld (.menuerase), a
    ld a, 'c' | 0x80
    ld (.menucopy), a
    ld a, 'd' | 0x80
    ld (.menunewfolder), a
    ld a, 'q' | 0x80
    ld (.menucancelmove), a
    ld a, 'a' | 0x80
    ld (.menufavourite), a
    ld hl, .movemsg
    ld (.menumove + 3), hl
    ld hl, .copymsg
    ld (.menucopy + 3), hl
    ld a, KEY_ROW + 1
    ld (.menucopy + 1), a
    ld a, 6
    ld (.menucopy + 2), a
    ld a, 'd'
    ld (.menunewfolder), a
    ld a, 13
    ld (.menunewfolder + 2), a
    ld a, (.choosing)
    or a
    jr z, 2F
    ld a, 6                     // choosing where to save: New folder is alone, under Here
    ld (.menunewfolder + 2), a
    jr .filekeysdone
2:
    ld a, (.moving)
    cp 1
    jr z, .movingkeys
    cp 2
    jr z, .copyingkeys
    ld a, 'm'
    ld (.menumove), a
    ld a, 'r'
    ld (.menurename), a
    ld a, 'e'
    ld (.menuerase), a
    ld a, 'c'
    ld (.menucopy), a
    ld a, 'a'
    ld (.menufavourite), a
    jr .filekeysdone
.movingkeys
    ld a, 'm'
    ld (.menumove), a
    ld hl, .moveheremsg
    ld (.menumove + 3), hl
    jr 1F
.copyingkeys
    ld a, 'c'
    ld (.menucopy), a
    ld hl, .copyheremsg
    ld (.menucopy + 3), hl
    ld a, KEY_ROW           // where M-Move here would be
    ld (.menucopy + 1), a
1:  ld a, 'd' | 0x80
    ld (.menunewfolder), a
    ld a, 'q'
    ld (.menucancelmove), a
.filekeysdone

    ld ix, 0
    ld de, (.startpos)
    ld (ix + 2), de                     // updatemenulist looks at the start position here
    ld hl, spare_space
    ld de, .menuprev
    call updatemenulist
    // the menu shows just "1-" and so on; .drawnames shows the names
    ld hl, .menuprev + 2 * 7 + 3
    ld de, 7
    ld b, 10
1:  ld (hl), 0
    inc hl
    ld (hl), 0
    dec hl
    add hl, de
    djnz 1B
.showthemenu
    ld hl, .menu
    call showmenu
    call .grouplabels
    // the bar between the entries and the letter keys, with what the letters do
    ld de, BAR_ROW * 256
    ld b, 32
    ld a, BANNER_ATTR
    call putattributes
    ld de, BAR_ROW * 256 + 8
    ld hl, .info2
    call putstring
    ld a, (.choosing)
    or a
    jr nz, 1F
    ld a, INFO_ATTR
    ld (text_attr), a
    ld de, 0x0201
    ld hl, .info1
    call putstring
    xor a
    ld (text_attr), a
1:
    xor a
    ld (.scrollpos), a
    ld hl, SCROLL_DELAY
    ld (.scrolltimer), hl
    call .drawnames
    call .drawcolumns
    ld a, (spare_space + SNAPLIST_BUSY)
    or a
    jp nz, .stillsearching
    ld hl, .menu
    ld de, 0
    ld bc, .scrollnames
    jp menuhandler_with_idle

// Draws the name and extension of each entry on the page. A name longer
// than NAME_WINDOW shows the part .scrollpos says.
.drawnames
    ld a, NAME_WINDOW
    ld c, a
    ld a, (.showinfo)
    or a
    ld a, c
    jr z, 1F
    ld a, NAME_WINDOW_INFO
1:  ld (.namewin), a
    xor a
    ld (.scrollmax), a
    ld a, (spare_space)     // entries on this page
    or a
    ret z
    ld b, a
    ld c, 0
1:  push bc
    call .drawname
    pop bc
    inc c
    djnz 1B
    ret

// c = the entry to draw
.drawname
    ld b, 0
    ld hl, spare_space + 2
    add hl, bc
    add hl, bc
    ld e, (hl)
    inc hl
    ld d, (hl)
    ld a, c
    add a, LIST_TOP         // the first entry's row
    ld (.namerow), a
    ex de, hl               // hl is the name, then comes the extension
    push hl
    ld b, 0
1:  ld a, (hl)
    or a
    jr z, 2F
    inc hl
    inc b
    jr 1B
2:  inc hl
    ld (.nameext), hl
    pop hl
    ld a, (.namewin)
    ld d, a
    ld a, b                 // how far this name can scroll
    sub d
    jr nc, 3F
    xor a
3:  ld c, a
    ld a, (.scrollmax)
    cp c
    jr nc, 4F
    ld a, c
    ld (.scrollmax), a
4:  ld a, (.scrollpos)      // start at scrollpos - SCROLL_PAUSE, between 0 and c
    sub SCROLL_PAUSE
    jr nc, 5F
    xor a
5:  cp c
    jr c, 6F
    ld a, c
6:  ld e, a
    ld d, 0
    add hl, de
    ld a, (.namerow)
    ld d, a
    ld e, 4
    ld a, (.namewin)
    ld b, a
7:  ld a, (hl)
    or a
    jr z, 8F
    ld c, a
    inc hl
    jr 9F
8:  ld c, ' '               // pad a short name
9:  call putchar
    djnz 7B
    ld hl, (.nameext)
    ld a, (hl)
    or a
    ret z                   // a directory has none
    ld a, (.showinfo)
    or a
    jr nz, .drawinfo
    ld e, 28
    ld c, '.'
    call putchar
    ld b, 3
1:  ld a, (hl)
    or a
    ret z
    ld c, a
    call putchar
    inc hl
    djnz 1B
    ret

// With the details on (O-Info): the folder moves up a row, and the titles of the columns
// (Type, Size and Mode, in yellow) take its place
.drawcolumns
    ld a, (.showinfo)
    or a
    ret z
    ld de, 0x0401
    ld b, 31
1:  ld c, ' '
    call putchar            // blanks the folder's row (text_attr is 0, the colours stay)
    djnz 1B
    ld de, 0x0301
    ld hl, spare_space + SNAPLIST_PATH
    call putstring
    ld a, INFO_ATTR
    ld (text_attr), a
    ld de, 0x0412
    ld hl, .typetitle
    call putstring
    ld de, 0x0417
    ld hl, .sizetitle
    call putstring
    ld de, 0x041C
    ld hl, .modetitle
    call putstring
    xor a
    ld (text_attr), a
    ret
.typetitle DZ "Type"
.sizetitle DZ "Size"
.modetitle DZ "Mode"

// With the details on: the extension (no dot) at column 20, then the eight characters
// the pico sent after it, the size and "128", at column 24. hl is the extension.
.drawinfo
    ld e, 20
    push hl
    ld b, 3
1:  ld a, (hl)
    or a
    jr z, 2F
    ld c, a
    inc hl
    jr 3F
2:  ld c, ' '
3:  call putchar
    djnz 1B
    pop hl
4:  ld a, (hl)              // on to the string after the extension
    inc hl
    or a
    jr nz, 4B
    ld e, 24
5:  ld a, (hl)
    or a
    ret z
    ld c, a
    inc hl
    call putchar
    jr 5B

// O: show the size and 128K flag of each file, or the long names again
.infohandler:
    ld a, (.showinfo)
    xor 2
    ld (.showinfo), a
    jp .refresh

// The idle routine while waiting for a key: every SCROLL_DELAY calls moves
// the long names on one character, and back to the start at the end.
.scrollnames
    ld hl, (.scrolltimer)
    dec hl
    ld (.scrolltimer), hl
    ld a, h
    or l
    ret nz
    ld hl, SCROLL_DELAY
    ld (.scrolltimer), hl
    ld a, (.scrollmax)
    or a
    ret z
    ld b, a
    ld a, (.scrollpos)
    inc a
    ld c, a
    ld a, b
    add a, 2 * SCROLL_PAUSE
    cp c
    jr nc, 1F
    ld c, 0
1:  ld a, c
    ld (.scrollpos), a
    jp .drawnames

// The pico has not finished filling this page yet. Show what it has found so
// far and keep asking, until the page is done or BREAK is pressed.
.stillsearching
    ld hl, .findstop
    call .row13
.pollagain
    call getkeypress
    cp 0x18
    jr z, .stopsearch
    ld a, (spare_space)     // how many results there were
    push af
    ld de, (.startpos)
    call .request
    pop bc
    ld a, (spare_space + SNAPLIST_BUSY)
    or a
    jp z, .updatefromdata   // finished
    ld a, (spare_space)
    cp b
    jp nz, .updatefromdata  // more results to show
    jr .pollagain           // nothing new, leave the screen alone

.stopsearch
    call waitforkeyrelease
    ld a, 1
    ld (.stopflag), a
    ld de, (.startpos)
    jp .fetchlist

// Asks the pico for the page starting at entry de
.request
    ld ix, 0
    ld (ix + 1), ACTION_SNAP_LIST
    ld (ix + 2), de
    ld (.startpos), de
    ld a, (.drive)
    ld (ix + 4), a
    ld hl, spare_space
    ld (ix + 5), hl
    ld a, (.stopflag)
    ld b, a
    ld a, (.showinfo)       // 2 asks for the file details
    or b
    ld (ix + 7), a
    xor a
    ld (.stopflag), a
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B
    ret

.snaphandlerzero
    ld a, 9
    jr 1F

.snaphandler:
    sub '1'
1:
    ld b, 0
    ld c, a
    ld hl, spare_space + 2
    add hl, bc
    add hl, bc
    ld e, (hl)
    inc hl
    ld d, (hl)
    ex hl, de

    // hl points to the name. If it ends with '/' then it is a directory
    // to enter rather than a snapshot to load.
    push hl
    xor a
    ld b, a
    ld c, a
    cpir        // hl is now one past the end of the name
    dec hl
    dec hl
    ld a, (hl)
    pop hl
    cp '/'
    jr z, .changedir

    ld a, (.choosing)       // only folders can be chosen to save in
    or a
    jp nz, .showthemenu
    // a .rom is not a snapshot: it is picked with C-Change ROM
    push hl
    xor a
    ld b, a
    ld c, a
    cpir                    // hl is now the start of the extension
    ld a, (hl)
    or 0x20
    cp 'r'
    jr nz, .notarom
    inc hl
    ld a, (hl)
    or 0x20
    cp 'o'
    jr nz, .notarom
    inc hl
    ld a, (hl)
    or 0x20
    cp 'm'
    jr nz, .notarom
    inc hl
    ld a, (hl)
    or a
    jr nz, .notarom
    pop hl
    ld hl, .romnotice
    call .message
    jp .refresh
.notarom
    pop hl
    // a .pok is a list of pokes: it is used with Poke
    push hl
    xor a
    ld b, a
    ld c, a
    cpir                    // hl is now the start of the extension
    ld a, (hl)
    or 0x20
    cp 'p'
    jr nz, .notapok
    inc hl
    ld a, (hl)
    or 0x20
    cp 'o'
    jr nz, .notapok
    inc hl
    ld a, (hl)
    or 0x20
    cp 'k'
    jr nz, .notapok
    inc hl
    ld a, (hl)
    or a
    jr nz, .notapok
    pop hl
    ld hl, .poknotice
    call .message
    jp .refresh
.notapok
    pop hl
    ld a, (.drive)
    jp loadsnapshot

.uphandler:
    ld hl, .dotdot

// hl points to the name of the directory to move to
.changedir:
    xor a       // changing directory ends a search
    ld (.findtext), a
    ld ix, 0
    ld (ix + 2), hl
    ld a, (.drive)
    ld (ix + 4), a
    ld (ix + 1), ACTION_SNAP_CHDIR
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B
    ld de, 0
    jp .fetchlist

// Asks for a line of text. hl points to WORD title, WORD label, WORD help
// line 1, WORD help line 2, BYTE maximum length, BYTE keep (non-zero to
// start from the text already in .editbuf). Returns with carry set if
// BREAK was pressed, otherwise the text is in .editbuf
.textinput:
    push hl
    call clearscreen
    pop hl
    ld de, 0x0101
    call .putindirect
    ld de, 0x0301
    call .putindirect
    ld de, 0x0601
    call .putindirect
    ld de, 0x0701
    call .putindirect
    push hl                 // one more line, row 8, if .textextra says what
    ld hl, (.textextra)
    ld a, h
    or l
    jr z, 1F
    ld de, 0x0801
    call putstring
    ld hl, (.textextra2)    // and row 9
    ld a, h
    or l
    jr z, 1F
    ld de, 0x0901
    call putstring
1:  pop hl
    ld a, (hl)
    ld (.inputmax), a
    inc hl
    ld a, (hl)
    ld (.inputkeep), a
    ld a, (.inputmax)
    add a, 2
    ld b, a
    ld de, 0x0401
    ld a, 0b001111
    call putattributes
    ld a, (.inputkeep)      // start from what is in .editbuf?
    or a
    jr nz, 1F
    xor a
    ld (.editbuf), a
1:  ld de, 0x0401
    ld hl, .editbuf
    call putstring
    ld c, '_'
    call putchar

.inputnextkey
    call waitforkeypress
    cp 0x18
    jr z, .inputcancel
    cp 0x13
    jr z, .inputaccept
    ld c, a
    ld de, 0x0401
    ld hl, .editbuf
    cp 0x0A
    jr z, .inputdelete
    cp ' '
    jr c, .inputrelease
    ld a, (.inputmax)
    ld b, a
    call append_edit_char
    jr .inputrelease
.inputdelete
    call delete_edit_char
.inputrelease
    call waitforkeyrelease
    jr .inputnextkey

.inputcancel
    call waitforkeyrelease
    scf
    ret

.inputaccept
    call waitforkeyrelease
    or a
    ret

// Shows the string whose address is at hl, at de. Returns with hl moved on.
.putindirect
    ld c, (hl)
    inc hl
    ld b, (hl)
    inc hl
    push hl
    ld hl, bc
    call putstring
    pop hl
    ret

// Prints the message at hl on the first key row in white, over the coloured keys there
.row13
    push hl
    ld de, KEY_ROW * 256 + 1
    ld b, 31
    ld a, 0b000111
    call putattributes
    pop hl
    ld de, KEY_ROW * 256 + 1
    jp putstring

// The names of the groups of keys, in yellow at the left of the key rows
.grouplabels
    ld a, INFO_ATTR
    ld (text_attr), a
    ld hl, .labelfile
    ld a, (.choosing)
    ld b, a
    ld a, (.moving)
    or b
    jr z, 1F
    ld hl, .labelhere           // a file is picked to move or copy, or a folder to save in
1:  ld de, KEY_ROW * 256 + 1
    call putstring
    ld de, (KEY_ROW + 2) * 256 + 1
    ld hl, .labelgame
    call putstring
    ld de, (KEY_ROW + 3) * 256 + 1
    ld hl, .labelfind
    call putstring
    ld a, (start_up_sdcard_present)
    cp 2
    jr nz, 3F
    ld de, (KEY_ROW + 4) * 256 + 1      // the SD card is a USB drive of a PC right now
    ld hl, .labeldisk
    call putstring
    ld de, (KEY_ROW + 4) * 256 + 6
    ld hl, .sdinusemsg
    call putstring
    jr 1F
3:  cp 1
    jr nz, 1F                   // only the internal flash, no drive to choose
    ld de, (KEY_ROW + 4) * 256 + 1
    ld hl, .labeldisk
    call putstring
    ld a, (.drive)              // the drive being browsed is shown in white on blue
    or a
    ld de, (KEY_ROW + 4) * 256 + 16
    ld b, 16                    // I-Internal Flash
    jr z, 2F
    ld de, (KEY_ROW + 4) * 256 + 6
    ld b, 9                     // S-SD Card
2:  ld a, BANNER_ATTR
    call putattributes
1:  ld de, (KEY_ROW + 5) * 256 + 1
    ld hl, .labelgo
    call putstring
    xor a
    ld (text_attr), a
    ret

// Shows a message until a key is pressed. hl points to the message.
.message
    push hl
    call clearscreen
    pop hl
    ld de, 0x0301
    call putstring
    ld de, 0x0501
    ld hl, .anykey
    call putstring
    call waitforkeypress
    jp waitforkeyrelease

// Sends the request set up at ix and waits. Returns with the status in a.
.waitpico
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B
    ret

// Lists the same page again, after something has changed
.refresh
    ld de, (.startpos)
    jp .fetchlist

// Asks for the text to search for
.findhandler:
    ld hl, .findinput
    call .textinput
    jp c, .showthemenu
    ld hl, .editbuf
    ld de, .findtext
    ld bc, FIND_MAXLEN + 1
    ldir
    ld de, 0x0901
    ld hl, .findbusy
    call putstring
    jp .sendfind

// Makes a new folder in this one
.newfolderhandler:
    ld hl, .folderinput
    call .textinput
    jp c, .showthemenu
    ld a, (.editbuf)
    or a
    jp z, .showthemenu
    ld ix, 0
    ld hl, .editbuf
    ld (ix + 2), hl
    ld a, (.drive)
    ld (ix + 4), a
    ld (ix + 1), ACTION_SNAP_MKDIR
    call .waitpico
    or a
    jr z, .refresh
    ld hl, .nameinuse
    cp 1
    jr z, 1F
    ld hl, .foldererror
1:  call .message
    jr .refresh

// Asks which file on this page (1-0) with the question at hl on row 13.
// Returns with hl pointing to the file's name, or with carry set if BREAK
// was pressed (a = 0) or a folder was chosen (a = 1, after saying so).
.pickfile:
    call .row13
.pickkey
    call waitforkeypress
    cp 0x18
    jr z, .pickcancel
    cp '0'
    jr c, .pickkey
    cp '9' + 1
    jr nc, .pickkey
    sub '1'
    jr nc, 1F
    ld a, 9                 // 0 is the tenth
1:  ld c, a
    ld a, (spare_space)     // entries on this page
    cp c
    jr z, .pickkey
    jr c, .pickkey
    ld b, 0
    ld hl, spare_space + 2
    add hl, bc
    add hl, bc
    ld e, (hl)
    inc hl
    ld d, (hl)
    push de
    call waitforkeyrelease
    pop hl
    // a directory's name ends with '/'
    push hl
    xor a
    ld b, a
    ld c, a
    cpir
    dec hl
    dec hl
    ld a, (hl)
    pop hl
    cp '/'
    jr z, .pickedfolder
    or a                    // clear carry
    ret
.pickedfolder
    ld a, (.pickdirs)       // a folder is fine when erasing
    or a
    jr z, 1F
    ld a, 0
    or a                    // clear carry, hl is the folder's name
    ret
1:  ld hl, .onlyfiles
    call .message
    ld a, 1
    scf
    ret
.pickcancel
    call waitforkeyrelease
    xor a
    scf
    ret

// After .pickfile returns with carry set: redraw, or list again after a message
.pickfailed
    or a
    jp z, .updatefromdata
    jp .refresh

// M: pick the file to move or, once one is picked, move it here
.movehandler:
    ld a, (.moving)
    cp 1
    jp z, .movehere
    ld a, 1
    ld hl, .movewhich
    jr 1F

// K: pick the file to copy or, once one is picked, copy it here
.copyhandler:
    ld a, (.moving)
    cp 2
    jp z, .movehere
    ld a, 2
    ld hl, .copywhich
1:  ld (.pickmode), a
    ld a, 1                 // a folder can be moved or copied
    ld (.pickdirs), a
    call .pickfile
    push af
    xor a
    ld (.pickdirs), a
    pop af
    jp c, .pickfailed
    ld ix, 0
    ld (ix + 2), hl
    ld a, (.drive)
    ld (ix + 4), a
    ld (ix + 5), ACTION_SNAP_MOVE_OP_PICK
    ld (ix + 1), ACTION_SNAP_MOVE
    call .waitpico
    or a
    jp nz, .refresh
    ld a, (.pickmode)       // 1 moving, 2 copying
    ld (.moving), a
    jp .updatefromdata

.movehere
    ld ix, 0
    ld hl, spare_space + MOVE_INFO
    ld (ix + 2), hl
    ld a, (.drive)
    ld (ix + 4), a
    ld (ix + 5), ACTION_SNAP_MOVE_OP_DESCRIBE
    ld a, (.moving)
    ld (ix + 6), a          // 1 moving, 2 copying (for a folder)
    ld (ix + 1), ACTION_SNAP_MOVE
    call .waitpico
    or a
    jr z, 1F
    cp FILEOP_IS_DIRECTORY  // a folder: type ok (as for erasing one)
    jp z, .eraseprotected
    ld hl, spare_space + MOVE_INFO  // it can't go here: the pico wrote why
    call .message
    jp .refresh
1:  call clearscreen
    ld de, 0x0101
    ld hl, .movetitle
    ld a, (.moving)
    cp 2
    jr nz, 1F
    ld hl, .copytitle
1:  call putstring
    ld de, 0x0301
    ld hl, spare_space + MOVE_INFO
    call putstring
    ld de, 0x0501
    ld hl, .movefrom
    call putstring
    ld de, 0x0601
    ld hl, spare_space + MOVE_INFO + SNAPLIST_PATH_SIZE
    call putstring
    ld de, 0x0801
    ld hl, .moveto
    call putstring
    ld de, 0x0901
    ld hl, spare_space + MOVE_INFO + 2 * SNAPLIST_PATH_SIZE
    call putstring
    ld de, 0x0B01
    ld hl, .moveask
    ld a, (.moving)
    cp 2
    jr nz, 1F
    ld hl, .copyask
1:  call putstring
.moveaskkey
    call waitforkeypress
    cp 'y'
    jr z, .movedo
    cp 'n'
    jr z, .movenotnow
    cp 0x18
    jr nz, .moveaskkey
.movenotnow
    call waitforkeyrelease
    jp .refresh

.movedo
    call waitforkeyrelease
    ld ix, 0
    ld a, (.drive)
    ld (ix + 4), a
    ld (ix + 5), ACTION_SNAP_MOVE_OP_DO
    ld a, (.moving)
    cp 2
    jr nz, 1F
    ld (ix + 5), ACTION_SNAP_MOVE_OP_COPY
1:  ld (ix + 1), ACTION_SNAP_MOVE
    ld a, (.movebar)
    or a
    jr z, 3F
    xor a
    ld (.movebar), a
    call .copybar           // a folder copy: the bar fills, BREAK stops it
    jr 4F
3:  call .waitpico
4:  or a
    jr nz, 1F
    ld (.moving), a         // done
    jp .refresh
1:  ld hl, .nameinuse
    cp 1
    jr z, 2F
    ld hl, .movesame
    cp 3
    jr z, 2F
    ld hl, .copystopped
    cp FILEOP_CANCELLED
    jr z, 2F
    ld hl, .moveerror
    ld a, (.moving)
    cp 2
    jr nz, 2F
    ld hl, .copyerror
2:  call .message
    jp .refresh

.copybar:
    call clearscreen
    ld de, 0x0401
    ld hl, .copybarmsg
    call putstring
    ld de, 0x0601
    ld hl, .copybarmsg2
    call putstring
    xor a
    call startmenu.updatebar         // the empty bar
    ld ix, 0
    ld (ix + 8), 0          // 1 when BREAK is pressed
    ld (ix + 3), 0          // the pico fills in how much is done
    ld (ix + 0), 255
1:  ld ix, 0
    ld a, (ix + 3)
    call startmenu.updatebar
    call getkeypress
    cp 0x18                 // BREAK
    jr nz, 2F
    ld ix, 0
    ld (ix + 8), 1
    call waitforkeyrelease  // so the message that follows is not dismissed by the same key
2:  ld ix, 0
    ld a, (ix)
    cp 255
    jr z, 1B
    ret

// R: rename a file or a folder
.renamehandler:
    ld a, 1                 // a folder is fine
    ld (.pickdirs), a
    ld hl, .renamewhich
    call .pickfile
    push af
    xor a
    ld (.pickdirs), a
    pop af
    jp c, .pickfailed
    ld (.picked), hl
    ld ix, 0
    ld (ix + 2), hl
    ld a, (.drive)
    ld (ix + 4), a
    ld hl, spare_space + MOVE_INFO
    ld (ix + 5), hl
    ld (ix + 7), ACTION_SNAP_RENAME_OP_DESCRIBE
    ld (ix + 1), ACTION_SNAP_RENAME
    call .waitpico
    or a
    jr z, 1F
    cp FILEOP_CANT_MOVE_FOLDER  // a folder of the Picoface's own: the pico wrote why
    jp nz, .refresh
    ld hl, spare_space + MOVE_INFO
    call .message
    jp .refresh
1:  // start from the name it has now
    ld hl, spare_space + MOVE_INFO
    ld de, .editbuf
    ld bc, FOLDER_MAXLEN + 1
    ldir
    xor a
    ld (.editbuf + FOLDER_MAXLEN), a
    ld hl, .renameinput
    call .textinput
    jp c, .refresh
    ld a, (.editbuf)
    or a
    jp z, .refresh
    ld ix, 0
    ld hl, (.picked)
    ld (ix + 2), hl
    ld a, (.drive)
    ld (ix + 4), a
    ld hl, .editbuf
    ld (ix + 5), hl
    ld (ix + 7), ACTION_SNAP_RENAME_OP_DO
    ld (ix + 1), ACTION_SNAP_RENAME
    call .waitpico
    or a
    jp z, .refresh
    ld hl, .nameinuse
    cp 1
    jr z, 1F
    ld hl, .renameerror
1:  call .message
    jp .refresh

// V: show a game's screen. ENTER loads the game, any other key goes back.
.viewhandler:
    ld hl, .viewwhich
    call .pickfile
    jp c, .pickfailed
    ld (.picked), hl
    ld ix, 0
    ld (ix + 2), hl
    ld a, (.drive)
    ld (ix + 4), a
    ld (ix + 1), ACTION_SNAP_PREVIEW
    call .waitpico          // whatever it found, the information is shown
    xor a                   // the file just previewed
    call showgameinfo
    cp 0x13                 // ENTER
    jr z, 1F
    call screenfrompico     // the game's screen again under the menu
    jp .updatefromdata
1:  ld hl, (.picked)
    ld a, (.drive)
    jp loadsnapshot

// A: copy a file to the Favourites folder at the root of the card
.favouritehandler:
    ld hl, .favouritewhich
    call .pickfile
    jp c, .pickfailed
    ld ix, 0
    ld (ix + 2), hl
    ld a, (.drive)
    ld (ix + 4), a
    ld (ix + 5), ACTION_SNAP_MOVE_OP_FAVOURITE
    ld (ix + 1), ACTION_SNAP_MOVE
    call .waitpico
    ld hl, .favouritedone
    or a
    jr z, 1F
    ld hl, .favouritethere
    cp 1
    jr z, 1F
    cp 3
    jr z, 1F
    ld hl, .copyerror
1:  call .message
    jp .refresh

// E: erase a file
.erasehandler:
    ld a, 1
    ld (.pickdirs), a
    ld hl, .erasewhich
    call .pickfile
    push af
    xor a
    ld (.pickdirs), a
    pop af
    jp c, .pickfailed
    ld (.picked), hl
    ld ix, 0
    ld (ix + 2), hl
    ld a, (.drive)
    ld (ix + 4), a
    ld hl, spare_space + MOVE_INFO
    ld (ix + 5), hl
    ld (ix + 7), ACTION_SNAP_ERASE_OP_DESCRIBE
    ld (ix + 1), ACTION_SNAP_ERASE
    call .waitpico
    cp FILEOP_IS_DIRECTORY
    jp z, .eraseprotected
    cp FILEOP_IS_ROM        // 6 to 10 are a ROM file or a protected folder
    jr c, 1F
    cp FILEOP_IS_PROTECTED + 1
    jp c, .eraseprotected
1:  or a
    jp nz, .refresh
    call clearscreen
    ld de, 0x0101
    ld hl, .erasetitle
    call putstring
    ld de, 0x0301
    ld hl, spare_space + MOVE_INFO
    call putstring
    ld de, 0x0501
    ld hl, .erasein
    call putstring
    ld de, 0x0601
    ld hl, spare_space + MOVE_INFO + SNAPLIST_PATH_SIZE
    call putstring
    ld de, 0x0801
    ld hl, .eraseask
    call putstring
.eraseaskkey
    call waitforkeypress
    cp 'y'
    jr z, .erasedo
    cp 'n'
    jr z, .erasenot
    cp 0x18
    jr nz, .eraseaskkey
.erasenot
    call waitforkeyrelease
    jp .refresh
.erasedo
    call waitforkeyrelease
    ld ix, 0
    ld hl, (.picked)
    ld (ix + 2), hl
    ld a, (.drive)
    ld (ix + 4), a
    ld (ix + 7), ACTION_SNAP_ERASE_OP_DO
    ld (ix + 1), ACTION_SNAP_ERASE
    call .waitpico
    or a
    jp z, .refresh
    ld hl, .eraseerror
    call .message
    jp .refresh

// A folder, a ROM file, /Favourites or /Save Games: the pico wrote five texts after the name
// and the folder (title, two lines, two more) and the user has to type "ok"
.eraseprotected:
    ld hl, spare_space + MOVE_INFO + SNAPLIST_PATH_SIZE * 2
    ld (.eraseinput), hl
    ld de, SNAPLIST_PATH_SIZE
    add hl, de
    ld (.eraseinput + 4), hl
    add hl, de
    ld (.eraseinput + 6), hl
    add hl, de
    push hl
    add hl, de
    ld (.textextra2), hl
    pop hl
    jp .eraseconfirm
.eraseconfirm:
    push hl
    ld hl, spare_space + MOVE_INFO      // the file's or folder's name, for the label
    ld (.eraseinput + 2), hl
    pop hl
    ld (.textextra), hl
    ld hl, .eraseinput
    call .textinput
    push af
    ld hl, 0
    ld (.textextra), hl
    ld (.textextra2), hl
    pop af
    jp c, .refresh
    ld a, (.editbuf)
    or 0x20
    cp 'o'
    jr nz, .erasedirno
    ld a, (.editbuf + 1)
    or 0x20
    cp 'k'
    jr nz, .erasedirno
    ld a, (.editbuf + 2)
    or a
    jr nz, .erasedirno
    ld a, (.moving)         // moving a folder, not erasing: the pico gets the text too
    or a
    jr z, 1F
    ld hl, .editbuf
    ld (6), hl
    dec a                   // copying (not moving) a folder takes time: show a bar
    ld (.movebar), a
    jp .movedo
1:  ld ix, 0
    ld hl, (.picked)
    ld (ix + 2), hl
    ld a, (.drive)
    ld (ix + 4), a
    ld hl, .editbuf
    ld (ix + 5), hl
    ld (ix + 7), ACTION_SNAP_ERASE_OP_DO
    ld (ix + 1), ACTION_SNAP_ERASE
    call .waitpico
    or a
    jp z, .refresh
    ld hl, .eraseerror
    call .message
    jp .refresh
.erasedirno
    ld hl, .erasedirnot
    ld a, (.moving)
    or a
    jr z, 1F
    ld hl, .movedirnot
1:  call .message
    jp .refresh

// Q: forget about moving the file
.cancelmovehandler:
    ld ix, 0
    ld (ix + 5), ACTION_SNAP_MOVE_OP_CANCEL
    ld (ix + 1), ACTION_SNAP_MOVE
    call .waitpico
    xor a
    ld (.moving), a
    jp .updatefromdata

.clearhandler:
    xor a
    ld (.findtext), a

// tell the pico what to search for and list again
.sendfind:
    ld ix, 0
    ld hl, .findtext
    ld (ix + 2), hl
    ld a, (.drive)
    ld (ix + 4), a
    ld (ix + 1), ACTION_SNAP_FIND
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B
    ld de, 0
    jp .fetchlist

.nextkeyhandler:
    ld hl, (.startpos)
    ld de, 10
    add hl, de
    ex hl, de
    jp .fetchlist

.prevkeyhandler:
    ld hl, (.startpos)
    ld de, 10
    sub hl, de
    ex hl, de
    jp .fetchlist

.sdcardhandler:
    ld a, 1
    ld (.drive), a
    ld de, 0
    jp .fetchlist

.intflashhandler:
    xor a
    ld (.drive), a
    ld de, 0
    jp .fetchlist

// X: leave. When choosing where to save, nothing is changed.
.exithandler:
    ld c, ACTION_SNA_SAVE_DIR_OP_CANCEL
    jr 1F
// H: save snapshots in the folder being shown
.herehandler:
    ld c, ACTION_SNA_SAVE_DIR_OP_SET
1:  ld a, (.choosing)
    or a
    ret z
    xor a
    ld (.findtext), a
    ld ix, 0
    ld (ix + 5), c
    ld a, (.drive)
    ld (ix + 4), a
    ld (ix + 1), ACTION_SNA_SAVE_DIR
    jp .waitpico

.drive BYTE 1                       // the SD card, unless there is none
.startpos WORD 0
.choosing BYTE 0                    // non-zero when choosing where to save
.scrollpos BYTE 0
.scrollmax BYTE 0
.scrolltimer WORD 0
.namerow BYTE 0
.nameext WORD 0
.titlemsg DZ "Browse Games"
.info1 DZ "Number: open folder/load game"
.labelfile DZ "File"
.labelhere DZ "Here"
.labelgame DZ "Game"
.labelfind DZ "Find"
.labeldisk DZ "Disk"
.sdinusemsg DZ "SD in use by the PC"
.labelgo DZ "Go"
.info2 DZ "Letter: command"
.choosetitle DZ "Save snapshots in..."
.saveheremsg DZ "Save here"
.snap0 BYTE 0
.exit DZ "Exit"
.next DZ "Next"
.prev DZ "Prev"
.sdcard DZ "SD Card"
.intflash DZ "Internal Flash"
.up DZ "Up"
.dotdot DZ ".."
.find DZ "Find"
.clear DZ "Clear"
.findtitle DZ "Find a game"
.findlabel DZ "Name contains:"
.findhelp1 DZ "ENTER to search, BREAK to exit"
.findhelp2 DZ "Looks in this folder and below"
.findbusy DZ "Searching...                  "
.findstop DZ "Searching... BREAK to stop    "
.stopflag BYTE 0
.showinfo BYTE 0            // 2 while the file details are shown, else 0
.namewin BYTE NAME_WINDOW   // how many characters of a name are shown
.findtext BLOCK FIND_MAXLEN + 1     // what is being searched for, empty if nothing
.findinput WORD .findtitle, .findlabel, .findhelp1, .findhelp2
    BYTE FIND_MAXLEN, 0
.folderinput WORD .foldertitle, .folderlabel, .folderhelp1, .folderhelp2
    BYTE FOLDER_MAXLEN, 0
.renameinput WORD .renametitle, .renamelabel, .renamehelp1, spare_space + MOVE_INFO + SNAPLIST_PATH_SIZE
    BYTE FOLDER_MAXLEN, 1
.renametitle DZ "Rename"
.renamelabel DZ "New name:"
.renamehelp1 DZ "ENTER to rename, BREAK to exit"
.renamewhich DZ "Rename which file/folder? 1-0 "
.renameerror DZ "Could not rename it"
.rename DZ "Rename"
.erase DZ "Erase"
.erasewhich DZ "Erase which file/folder? 1-0   "
.viewwhich DZ "View which game? 1-0          "
.viewmsg DZ "View"
.infomsg DZ "Info"
.erasetitle DZ "Erase file"
.erasein DZ "In:"
.eraseask DZ "Y-Yes, erase it   N-No"
.eraseerror DZ "Could not erase it"
.eraseinput WORD 0, 0, 0, 0
    BYTE 2, 0
.erasedirnot DZ "Not erased: ok was not typed"
.movedirnot DZ "Not done: ok was not typed"
.copybarmsg DZ "Copying the folder..."
.copybarmsg2 DZ "BREAK stops it"
.copystopped DZ "Copy stopped"
.movebar BYTE 0
.textextra WORD 0
.textextra2 WORD 0
.pickdirs BYTE 0
.romnotice DZ "A ROM: use C-Change ROM"
.poknotice DZ "A poke list: use Poke, then L"
.onlyfiles DZ "That is a folder, not a file"
.picked WORD 0
.inputkeep BYTE 0
.foldertitle DZ "New folder"
.folderlabel DZ "Name:"
.folderhelp1 DZ "ENTER to make it, BREAK to exit"
.folderhelp2 DZ "It goes in this folder"
.nameinuse DZ "That name is already in use"
.foldererror DZ "Could not make that folder"
.newfolder DZ "New folder"
.movemsg DZ "Move"
.moveheremsg DZ "Move here"
.cancelmove DZ "Cancel"
.movewhich DZ "Move which file/folder? 1-0   "
.movetitle DZ "Move file"
.copytitle DZ "Copy file"
.copyask DZ "Y-Yes, copy it   N-No"
.copyerror DZ "Could not copy the file"
.favouritewhich DZ "Favourite which file? 1-0     "
.favouritemsg DZ "Add fav"
.favouritedone DZ "Copied to Favourites"
.favouritethere DZ "It is in Favourites already"
.copywhich DZ "Copy which file? 1-0          "
.copymsg DZ "Copy"
.copyheremsg DZ "Copy here"
.pickmode BYTE 0
.movefrom DZ "From:"
.moveto DZ "To:"
.moveask DZ "Y-Yes, move it   N-No"
.movesame DZ "It is already in this folder"
.moveerror DZ "Could not move the file"
.anykey DZ "Press any key"
.moving BYTE 0                      // non-zero once a file to move is picked
.inputmax BYTE 0
.editbuf BLOCK FOLDER_MAXLEN + 1

.menu     
    MENU_ITEM 0, 1, 1, .titlemsg, 0
    // the directory being listed, filled in by the pico
    MENU_ITEM 0, 4, 1, spare_space + SNAPLIST_PATH, 0
    // The key rows, in groups with a yellow name at the left (.grouplabels), keys from column 6:
    //   File   M-Move R-Rename E-Erase          (Here: M-Move here, Q-Cancel / H-Save here)
    //          C-Copy D-New folder
    //   Game   O-Info V-View A-Add fav
    //   Find   F-Find, or F-Clear and what is searched for
    //   Disk   S-SD Card I-Internal Flash   (the drive being browsed in white on blue)
    //   Go     U-Up P-Prev N-Next X-Exit
    MENU_ITEM 'x', KEY_ROW + 5, 25, .exit, .exithandler
.menusd
    MENU_ITEM 's', KEY_ROW + 4, 6, .sdcard, .sdcardhandler
.menuint
    MENU_ITEM 'i', KEY_ROW + 4, 16, .intflash, .intflashhandler
.menuup
    MENU_ITEM 'u', KEY_ROW + 5, 6, .up, .uphandler
.menufind
    MENU_ITEM 'f', KEY_ROW + 3, 6, .find, .findhandler
    MENU_ITEM 0, KEY_ROW + 3, 14, .findtext, 0
.menumove
    MENU_ITEM 'm', KEY_ROW + 0, 6, .movemsg, .movehandler
.menurename
    MENU_ITEM 'r', KEY_ROW + 0, 13, .rename, .renamehandler
.menuerase
    MENU_ITEM 'e', KEY_ROW + 0, 22, .erase, .erasehandler
.menucopy
    MENU_ITEM 'c', KEY_ROW + 1, 6, .copymsg, .copyhandler
.menucancelmove
    MENU_ITEM 'q', KEY_ROW + 0, 18, .cancelmove, .cancelmovehandler
.menunewfolder
    MENU_ITEM 'd', KEY_ROW + 1, 13, .newfolder, .newfolderhandler
.menuinfo
    MENU_ITEM 'o', KEY_ROW + 2, 6, .infomsg, .infohandler
.menuview
    MENU_ITEM 'v', KEY_ROW + 2, 13, .viewmsg, .viewhandler
.menufavourite
    MENU_ITEM 'a', KEY_ROW + 2, 20, .favouritemsg, .favouritehandler
.menuhere
    MENU_ITEM 'h', KEY_ROW + 0, 6, .saveheremsg, .herehandler
.menuprev
    MENU_ITEM 'p', KEY_ROW + 5, 11, .prev, .prevkeyhandler
    MENU_ITEM 'n', KEY_ROW + 5, 18, .next, .nextkeyhandler
    MENU_ITEM '1', 5, 2, .snap0, .snaphandler
    MENU_ITEM '2', 6, 2, .snap0, .snaphandler
    MENU_ITEM '3', 7, 2, .snap0, .snaphandler
    MENU_ITEM '4', 8, 2, .snap0, .snaphandler
    MENU_ITEM '5', 9, 2, .snap0, .snaphandler
    MENU_ITEM '6', 10, 2, .snap0, .snaphandler
    MENU_ITEM '7', 11, 2, .snap0, .snaphandler
    MENU_ITEM '8', 12, 2, .snap0, .snaphandler
    MENU_ITEM '9', 13, 2, .snap0, .snaphandler
    MENU_ITEM '0', 14, 2, .snap0, .snaphandlerzero
    MENU_END



; -------------------------------------------
; Save snapshot menu

savesnapshotscreen:

    ld a, (sna_on_entry + SNAHEADER.BORD)
    ld (.bordercolour), a
    xor a
    ld (.savetype), a       // the type of file the machine would make

    // start with the name of the game last loaded or saved, to save over it
    ld ix, 0
    ld hl, .nametext
    ld (ix + 2), hl
    ld (ix + 5), ACTION_SNA_SAVE_DIR_OP_LAST
    ld (ix + 1), ACTION_SNA_SAVE_DIR
    call loadscreen.waitpico
    ld (.namelen), a
    ld (.overlast), a       // non-zero: it goes in that game's folder

    ld a, (.quick)
    or a
    jr z, .redisplay
    // Quick-save: the name is "<game> Save Game" and the type is the game's. Without
    // a game to name it after it is the ordinary screen.
    ld ix, 0
    ld hl, .nametext
    ld (ix + 2), hl
    ld (ix + 5), ACTION_SNA_SAVE_DIR_OP_QUICK
    ld (ix + 1), ACTION_SNA_SAVE_DIR
    call loadscreen.waitpico
    cp 254                  // (255 means the request is still being worked on)
    jr z, .nogame
    or a
    jr nz, 1F
    ld (.quick), a
    jr .redisplay
1:  ld (.namelen), a
    ld a, 2                 // 2 = a quick-save: the pico puts it in the Save Games folder
    ld (.overlast), a
    ld a, ACTION_SNA_SAVE_TYPE_SAME
    ld (.savetype), a
    jr .redisplay

.nogame:
    xor a
    ld (.quick), a
    ld hl, startmenu.noreloadmsg
    call loadscreen.message
    ld a, 1                 // back to the menu
    ret

.redisplay:
    xor a
    ld (.saveto), a
    call clearscreen
    ld de, 0x0101
    ld hl, .titlemsg
    call putstring

    ld de, 0x0301
    ld hl, .namelabel
    call putstring
    ld de, 0x0401
    ld b, 30
    ld a, 0b001111
    call putattributes

    ld de, 0x0601
    ld hl, .borderlabel
    call putstring

    ld de, 0x0801
    ld hl, .savetolabel
    call putstring
    // ask the pico where it will go
    ld ix, 0
    ld hl, spare_space + MOVE_INFO
    ld (ix + 2), hl
    ld a, (.overlast)
    ld (ix + 4), a
    ld (ix + 5), ACTION_SNA_SAVE_DIR_OP_DESCRIBE
    ld (ix + 1), ACTION_SNA_SAVE_DIR
    call loadscreen.waitpico
    ld de, 0x0901
    ld hl, spare_space + MOVE_INFO
    call putstring

    ld hl, .help1
    ld de, 0x0B01
1:  call putstring      // the help lines follow each other
    inc hl
    inc d
    ld e, 1
    ld a, d
    cp 0x11
    jr c, 1B


    call .showbordername
    call .showtype

    ld a, (.namelen)
    ld b, a
    ld de, 0x0401
    ld hl, .nametext
    cp 0
    jr z, 2F
1:  
    ld a, (hl)
    inc hl
    ld c, a
    call putchar
    djnz 1B
2:
    ld c, '_'
    call putchar

    ld a, (.quick)          // a quick-save goes on to save: the screen shows what it saves
    or a
    jr z, .waitfornextkey
    xor a
    ld (.quick), a
    jp .dosave

.waitfornextkey
    call waitforkeypress

    cp 0x18
    jp z, .exit

    cp 0x13
    jp z, .dosave

    cp 0x0A
    jp z, .backspace

    cp 31
    jp nc, .handlecharkey

    cp 0x9
    jp z, .choosefolder
    jr nc, .waitfornextkey

.handleborder
    and 0b111
    ld (.bordercolour), a
    call .showbordername
    jr .waitforkeyrelease    


.handlecharkey
    cp '<'                  // Symbol Shift R or T change the type: neither can be in a file name
    jr z, .nexttype
    cp '>'
    jr z, .nexttype
    ld c, a
    ld a, (.overlast)       // typing over the last game's name starts a new one
    or a
    jr z, 1F
    xor a
    ld (.namelen), a
1:  ld a, (.namelen)
    cp ACTION_SNA_SAVE_NAME_MAX
    jr nc, .waitforkeyrelease
    call .newname

    ld hl, .nametext
    ld d, 0
    ld e, a
    add hl, de
    ld (hl), c
    
    ld d, 4
    ld e, a
    inc e
    call putchar
    ld c, '_'
    call putchar

    inc a
    ld (.namelen), a

.waitforkeyrelease
    call waitforkeyrelease
    ld a, (.saveto)         // the name was changed: show where it goes now
    or a
    jp nz, .redisplay

1:   jr .waitfornextkey

.nexttype
    ld a, (.savetype)
    inc a
    cp 4
    jr c, 1F
    xor a
1:  ld (.savetype), a
    call .showtype
    jr .waitforkeyrelease

// Shows the type of file at the end of the Border line
.showtype
    ld hl, .typeauto
    ld a, (.savetype)
    or a
    jr z, 2F
    ld b, a
    ld de, 11
1:  add hl, de
    djnz 1B
2:  ld de, 0x0612
    call putstring
    ld de, 0x0618           // auto, .sna, .z80, .szx or same: bright yellow
    ld b, 4
    ld a, INFO_ATTR
    jp putattributes

// Called before the name is changed: a changed name goes in the chosen folder,
// not the last game's. Keeps a and c.
.newname
    push af
    ld a, (.overlast)
    ld (.saveto), a         // redraw once the key is let go, if it was the last game's
    xor a
    ld (.overlast), a
    pop af
    ret



.exit
    call waitforkeyrelease
    ld a, 1
    ret

.backspace
    ld a, (.namelen)
    cp 0
    jr z, .waitforkeyrelease
    call .newname
    dec a
    ld (.namelen), a
    ld d, 4
    inc a
    ld e, a
    ld c, '_'
    call putchar
    ld c, ' '
    call putchar
    jr .waitforkeyrelease


// Shift 9: choose where it goes, with the Load snapshot screen
.choosefolder
    call waitforkeyrelease
    ld a, (loadscreen.drive)
    push af
    ld a, 1
    ld (loadscreen.choosing), a
    call loadscreen
    xor a
    ld (loadscreen.choosing), a
    pop af
    ld (loadscreen.drive), a
    xor a                   // a folder was chosen: not the last game's any more
    ld (.overlast), a
    jp .redisplay

.dosave
    ld a, (.namelen)    // a name is needed
    or a
    jp z, .waitforkeyrelease
    ld a, (.bordercolour)
    ld (sna_on_entry + SNAHEADER.BORD), a
    call savesnapshot
    xor a ; don't overwrite in first instance
.saveit
    push ix
    ld ix, 0
    ld (ix + 4), a 
    ld a, (.overlast)
    ld (ix + 5), a
    ld a, (.savetype)
    ld (ix + 6), a
    ld (ix + 1), ACTION_SNA_SAVE
    ld a, (.namelen)
    ld e, a
    ld d, 0
    ld hl, .nametext
    ld (ix + 2), hl
    add hl, de
    ld (hl), 0
    ld (ix), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B
    pop ix    

    cp 0
    ret z ; save was a success
    cp 1
    jr z, .prompttooverwrite

    // some error: say so and let the name or place be changed
    call waitforkeyrelease      // ENTER may still be down
    ld hl, .saveerror
    call loadscreen.message
    jp .redisplay

.prompttooverwrite:
    ld a, (.overlast)
    cp 2
    jr z, .promptquick
    call overwritescreen
    jp nc, .redisplay ; don't overwrite, give the user a chance to change the name
    ld a, 1          ; otherwise have another go with a forced overwrite
    jr .saveit    

// A quick save of a game that has been saved: overwrite it (Y), or keep it and save
// under the next number, "<game> Save Game 2", 3 ... (N)
.promptquick:
    ld hl, overwritescreen.quickmenu_def
    call showmenu
    ld hl, overwritescreen.quickmenu_def
    call menukeyhandler
    ld a, 1
    jp c, .saveit
    inc a                   // 2: the next number
    jp .saveit

.showbordername:
    ld a, (.bordercolour)
    add a, a
    add a, a
    add a, a
    ld d, 0
    ld e, a
    ld hl, .border0
    add hl, de
    ld de, 0x0609
    call putstring
    ld a, (.bordercolour)
    rla
    rla
    rla
    and 0b111000
    cp 0b100000
    jr nc, .keepinkblack
    or 0b111
.keepinkblack
    ld de, 0x0609
    ld b, 7
    call putattributes    
    ret



.bordercolour DB 1
.nametext BLOCK 32
.namelen DB 1
.overlast DB 0          // non-zero while the name is the last game's, unchanged
.savetype DB 0          // ACTION_SNA_SAVE_TYPE_*
.quick DB 0             // non-zero for a quick-save, set by the menu
.saveto DB 0            // non-zero to redraw the Save to: line
    ASSERT ACTION_SNA_SAVE_NAME_MAX < 32 && ACTION_SNA_SAVE_NAME_MAX <= 30

.titlemsg DZ "Save Snapshot"
.help1 DZ "ENTER to save, BREAK To exit"
.help2 DZ "Shift 1-8 to change saved"
.help3 DZ "border colour"
.help4 DZ "Shift 9 to change where it goes"
.help5 DZ "Sym Shift R or T changes the"
.help6 DZ "file type (.sna .z80 .szx)"
.typeauto DZ "Type: auto"      // each 11 bytes
.typesna DZ "Type: .sna"
.typez80 DZ "Type: .z80"
.typeszx DZ "Type: .szx"
.typesame DZ "Type: same"
.savetolabel DZ "Save to:"
.saveerror DZ "Could not save the snapshot"
.namelabel DZ "Name:"
.borderlabel DZ "Border: "
.border0 DZ "Black  "
.border1 DZ "Blue   "
.border2 DZ "Red    "
.border3 DZ "Magenta"
.border4 DZ "Green  "
.border5 DZ "Cyan   "
.border6 DZ "Yellow "
.border7 DZ "White  "


overwritescreen:
    ld hl, .menu_def
    call showmenu

    ld hl, .menu_def
    jp menukeyhandler

.handle_no:
    scf
    ccf
    ret

.handle_yes:
    scf
    ret

.menu_def:
    MENU_ITEM 0, 1, 1, .title1, 0
    MENU_ITEM 0, 2, 1, .title2, 0
    MENU_ITEM 'y', 4, 2, .yes, .handle_yes
    MENU_ITEM 'n', 5, 2, .no, .handle_no
    MENU_END
.quickmenu_def:
    MENU_ITEM 0, 1, 1, .quicktitle1, 0
    MENU_ITEM 0, 2, 1, .quicktitle2, 0
    MENU_ITEM 'y', 4, 2, .yes, .handle_yes
    MENU_ITEM 'n', 5, 2, .quickno, .handle_no
    MENU_END
.quicktitle1 DZ "Do you want to overwrite"
.quicktitle2 DZ "the existing save? Y or N?"
.quickno: DZ "No, save as a new number"
.title1 DZ "A file with this name exists"
.title2 DZ "Overwrite this file?"
.yes: DZ "Yes, overwrite this file"
.no: DZ "No, cancel"

; -------------------------------------------
; poke screen menu


show_char_at_end_of_string:
    ld a, (hl)
    cp 0
    jp z, putchar
1:  inc hl
    inc e
    jr show_char_at_end_of_string


show_caret:
    ld c, '_'
    jp show_char_at_end_of_string

hide_caret:
    ld c, ' '
    jp show_char_at_end_of_string

; de screen position
; hl string pointer
; b max length
; c char to append
append_edit_char:
    push bc
    ld c, 0
1:  ld a, (hl)
    cp 0
    jr z, .foundend
    inc hl
    inc e
    inc c
    jr 1B
.foundend:
    ld a, c
    cp b
    pop bc
    ret nc
.canappend
    ld (hl), c
    inc hl
    ld (hl), 0
    call putchar
    ld c, '_'
    jp putchar

; de screen position
; hl string pointer
delete_edit_char:
    ld a, (hl)
    cp 0
    ret z
1:
    inc hl
    ld a, (hl)
    cp 0
    jr nz, 2F
    dec hl
    ld (hl), a
    ld c, '_'
    call putchar
    ld c, ' '
    jp putchar
2:
    inc e
    jr 1B


; hl points to string
; returns with hl as the decimal value
; carry flag set if string is too large

atoi:
    ld a, (hl)
    cp 0
    jr nz, 1F
    scf
    ret

1:  ld de, hl
    ld hl, 0

1:
    ld a, (de)
    cp 0
    ret z   ; carry flag also is clear at this point
    sub '0'

    ; hl := hl * 10 == hl * 2 + hl * 8
    push de
    add hl, hl
    jr c, .getoutoverflow
    ld de, hl  ; de = originalhl * 2
    add hl, hl  
    jr c, .getoutoverflow
    add hl, hl
    jr c, .getoutoverflow
    add hl, de
    jr c, .getoutoverflow
    ld d, 0
    ld e, a
    add hl, de
    jr c, .getoutoverflow
    pop de
    inc de
    jr 1B
    
.getoutoverflow:
    pop de
    ret


printbytevalue:
    cp 100
    jr nc, .hundreds
    cp 10
    jr nc, .tens
    jr .units


.hundreds
    ld c, '0'
1:  sub 100
    jr c, 2F
    inc c
    jr 1B
2:  call putchar
    add 100

.tens
    ld c, '0'
1:  sub 10
    jr c, 2F
    inc c
    jr 1B
2:  call putchar
    add 10

.units
    add '0'
    ld c, a
    jp putchar




pokescreen:
    xor a
    ld (.address_input), a
    ld (.value_input), a
    inc a
    ld (.active_edit), a


.showtopscreen
    ld hl, .startmenu_def
    call showmenu

    ld de, 0x030D
    ld b, 6
    ld a, 0b001111
    call putattributes

    ld de, 0x030D
    ld c, '_'
    call putchar
    
    ld de, 0x050D
    ld b, 4
    ld a, 0b001111
    call putattributes


.scan_keys
    ld de, .unmatched_key
    ld hl, .startmenu_def
    jp menuhandler_with_keydown_handler

.unmatched_key:
    ld a, c
    cp 0xA
    jr z, .handle_edit_key
1:  cp '0'
    ret c
    cp '9' + 1
    ret nc
.handle_edit_key
    ld a, (.active_edit)
    cp 1
    jr nz, 1F
    ld hl, .address_input
    ld de, 0x030D
    ld b, 5
    jr 2F
1:  cp 2
    ret nz
    ld hl, .value_input
    ld de, 0x050D
    ld b, 3
2:
    ld a, 0xA
    cp c
    jr nz, 3F
    call delete_edit_char
    jr .showcurrentvalue
3:
    call append_edit_char

.showcurrentvalue
    ld hl, .address_input
    call atoi
    ld de, 0x0314
    jr c, .clearcurrentvalue
    ld a, 63  ; 64 * 256 == 16384
    cp h
    jr nc, .clearcurrentvalue ; don't show the value for addresses less than 16384 because this could cause the NMI exit to be trigger.
    ld c, '('
    call putchar
    ld a, (hl)
    call printbytevalue
    ld c, ')'
    call putchar

.clearcurrentvalue
    ld c, ' '
    ld b, 5
1:  call putchar
    djnz 1B
    ret




.handle_address:
    ld de, 0x050D
    ld hl, .value_input
    call hide_caret
    ld de, 0x030D
    ld hl, .address_input
    call show_caret
    ld a, 1
    ld (.active_edit), a
    jr .scan_keys

.handle_value:
    ld de, 0x030D
    ld hl, .address_input
    call hide_caret
    ld de, 0x050D
    ld hl, .value_input
    call show_caret
    ld a, 2
    ld (.active_edit), a
    jp .scan_keys

.handle_poke:
    ld hl, .value_input
    call atoi
    jr c, .skippoke
    xor a
    or h
    jp nz,.skippoke
    ld c, l
    ld hl, .address_input
    call atoi
    jr c, .skippoke

    ld (hl), c
    call .showcurrentvalue

.skippoke:
    jp .scan_keys

.handle_exit:
    ret

// L: the cheats in the .pok file of the game last loaded, ten to a page. 1-0 makes the
// pokes of that cheat, N and P turn the pages.
.handle_list:
    ld hl, 0
    ld (.liststart), hl
.listshow
    ld ix, 0
    ld hl, (.liststart)
    ld (ix + 2), hl
    ld hl, spare_space
    ld (ix + 4), hl
    ld (ix + 1), ACTION_POKE_LIST
    call loadscreen.waitpico
    or a
    jr z, .listgot
    ld hl, .nopoklist
    call loadscreen.message
    jp .showtopscreen
.listgot
    call clearscreen
    ld de, 0x0101
    ld hl, .listtitle
    call putstring
    ld a, (spare_space)
    ld b, a
    ld c, 0
.listdraw
    ld a, c
    cp b
    jr z, .listdrawn
    push bc
    ld a, c                 // the key and the name, as the lists do
    add a, LIST_TOP
    ld d, a
    ld e, 2
    ld a, KEY_ATTR
    ld (text_attr), a
    ld a, c
    add a, '1'
    cp '9' + 1
    jr c, 1F
    ld a, '0'
1:  ld c, a
    call putchar
    ld c, '-'
    call putchar
    ld a, OPTION_ATTR
    ld (text_attr), a
    pop bc
    inc c
    jr .listdraw
.listdrawn
    xor a                   // the names scroll to the left if they are too long
    ld (.lscrollpos), a
    ld hl, SCROLL_DELAY
    ld (.lscrolltimer), hl
    ld a, OPTION_ATTR
    ld (text_attr), a
    call .listnames
    xor a
    ld (text_attr), a
    ld de, BAR_ROW * 256 + 2
    ld hl, .listkeys
    call putstring
.listkey
    call .listwait
    cp 'x'
    jr z, .listexit
    cp 0x18
    jr z, .listexit
    cp 'n'
    jr z, .listnext
    cp 'p'
    jr z, .listprev
    cp '0'
    jr c, .listkey
    cp '9' + 1
    jr nc, .listkey
    sub '1'
    jr nc, 1F
    ld a, 9                 // 0 is the tenth
1:  ld c, a
    ld a, (spare_space)     // entries on this page
    cp c
    jr z, .listkey
    jr c, .listkey
    ld b, 0
    ld hl, (.liststart)
    add hl, bc
    push hl
    call waitforkeyrelease
    pop hl
    jr .listapply
.listexit
    call waitforkeyrelease
    jp .showtopscreen
.listnext
    ld a, (spare_space + 1)     // more to come?
    or a
    jr z, .listkey
    ld hl, (.liststart)
    ld de, 10
    add hl, de
    jr .listturn
.listprev
    ld hl, (.liststart)
    ld a, h
    or l
    jr z, .listkey
    ld de, 10
    or a
    sbc hl, de
.listturn
    ld (.liststart), hl
    call waitforkeyrelease
    jp .listshow

// hl is the cheat to make
.listapply
    ld ix, 0
    ld (ix + 2), hl
    ld hl, spare_space + MOVE_INFO
    ld (ix + 4), hl
    ld (ix + 1), ACTION_POKE_GET
    call loadscreen.waitpico
    or a
    jp nz, .listshow
    ld a, (spare_space + MOVE_INFO + 1)
    or a
    jr z, .listpoke
    // one of its values is for the user to choose
    ld hl, .askinput
    call loadscreen.textinput
    jp c, .listshow
    ld hl, loadscreen.editbuf
    call atoi
    jp c, .listbadvalue
    ld a, h
    or a
    jp nz, .listbadvalue
    ld a, l
    ld (.askvalue), a
.listpoke
    xor a
    ld (.pokedcount), a
    ld (.skipcount), a
    ld a, (spare_space + MOVE_INFO)
    or a
    jr z, .listresult
    ld b, a
    ld hl, spare_space + MOVE_INFO + 2
.pokeone
    ld e, (hl)
    inc hl
    ld d, (hl)              // the address
    inc hl
    ld c, (hl)              // the value
    inc hl
    ld a, (hl)              // 1 if it is the user's
    inc hl
    or a
    jr z, 1F
    ld a, (.askvalue)
    ld c, a
1:  ld a, (hl)              // the bank
    inc hl
    push hl
    push bc
    call .pokeallowed
    pop bc
    jr nz, .pokeskipped
    ld a, c
    ld (de), a
    ld a, (.pokedcount)
    inc a
    ld (.pokedcount), a
    jr .pokenext
.pokeskipped
    ld a, (.skipcount)
    inc a
    ld (.skipcount), a
.pokenext
    pop hl
    djnz .pokeone
.listresult
    call clearscreen
    ld de, 0x0301
    ld hl, .pokedtext
    call putstring
    ld a, (.pokedcount)
    call printbytevalue
    ld a, (.skipcount)
    or a
    jr z, .listresultkey
    ld de, 0x0501
    ld hl, .skippedtext
    call putstring
    ld a, (.skipcount)
    call printbytevalue
    ld de, 0x0601
    ld hl, .skippedwhy
    call putstring
.listresultkey
    ld de, 0x0901
    ld hl, loadscreen.anykey
    call putstring
    call waitforkeypress
    call waitforkeyrelease
    jp .listshow
.listbadvalue
    ld hl, .badvalue
    call loadscreen.message
    jp .listshow

// a = the bank of a poke and de its address: returns z if it can be made now. The ROM
// area cannot; 0x4000 to 0xBFFF only in their own banks (5 and 2); 0xC000 up only when
// the machine runs a 48K program, which has bank 0 there.
.pokeallowed
    ld c, a
    ld a, d
    cp 0x40
    jr c, .notallowed
    ld a, c
    cp 8
    jr nc, .allowed         // no bank: the address as the processor sees it
    ld a, d
    cp 0x80
    jr c, 1F
    cp 0xC0
    jr c, 2F
    ld a, (start_up_machine48)
    or a
    jr z, .notallowed
    ld a, c
    or a
    jr z, .allowed
    jr .notallowed
1:  ld a, c
    cp 5
    jr z, .allowed
    jr .notallowed
2:  ld a, c
    cp 2
    jr z, .allowed
.notallowed
    or 1                    // nz
    ret
.allowed
    xor a
    ret

// Waits for a key like waitforkeypress, and meanwhile scrolls the names that are too long
.listwait
    call getkeypress
    or a
    jr z, 1F
    cp 255
    ret nz
1:  call .listscroll
    jr .listwait

// Every SCROLL_DELAY calls moves the long names on one character, and back to the start
// at the end (as the Browse Games list does)
.listscroll
    ld hl, (.lscrolltimer)
    dec hl
    ld (.lscrolltimer), hl
    ld a, h
    or l
    ret nz
    ld hl, SCROLL_DELAY
    ld (.lscrolltimer), hl
    ld a, (.lscrollmax)
    or a
    ret z
    ld b, a
    ld a, (.lscrollpos)
    inc a
    ld c, a
    ld a, b
    add a, 2 * SCROLL_PAUSE
    cp c
    jr nc, 1F
    ld c, 0
1:  ld a, c
    ld (.lscrollpos), a
    ld a, OPTION_ATTR
    ld (text_attr), a
    call .listnames
    xor a
    ld (text_attr), a
    ret

// Draws the name of each cheat on the page: POKE_NAME_WINDOW characters from column 4,
// from the character .lscrollpos says (after a pause at each end). Sets .lscrollmax to how
// far the longest name can scroll.
.listnames
    xor a
    ld (.lscrollmax), a
    ld a, (spare_space)     // cheats on this page
    or a
    ret z
    ld b, a
    ld c, 0
1:  push bc
    call .listname
    pop bc
    inc c
    djnz 1B
    ret

// c = the cheat on the page
.listname
    ld b, 0
    ld hl, spare_space + 2
    add hl, bc
    add hl, bc
    ld a, (hl)
    inc hl
    ld h, (hl)
    ld l, a                 // the name
    ld a, c
    add a, LIST_TOP
    ld (.lnamerow), a
    push hl
    ld b, 0
1:  ld a, (hl)
    or a
    jr z, 2F
    inc hl
    inc b
    jr 1B
2:  pop hl
    ld a, b                 // how far this name can scroll
    sub POKE_NAME_WINDOW
    jr nc, 3F
    xor a
3:  ld c, a
    ld a, (.lscrollmax)
    cp c
    jr nc, 4F
    ld a, c
    ld (.lscrollmax), a
4:  ld a, (.lscrollpos)     // start at lscrollpos - SCROLL_PAUSE, between 0 and c
    sub SCROLL_PAUSE
    jr nc, 5F
    xor a
5:  cp c
    jr c, 6F
    ld a, c
6:  ld e, a
    ld d, 0
    add hl, de
    ld a, (.lnamerow)
    ld d, a
    ld e, 4
    ld b, POKE_NAME_WINDOW
7:  ld a, (hl)
    or a
    jr z, 8F
    ld c, a
    inc hl
    jr 9F
8:  ld c, ' '               // pad a short name
9:  call putchar
    djnz 7B
    ret

.lscrollpos BYTE 0
.lscrollmax BYTE 0
.lscrolltimer WORD 0
.lnamerow BYTE 0
.liststart WORD 0
.pokedcount BYTE 0
.skipcount BYTE 0
.askvalue BYTE 0
.listtitle DZ "Poke list"
.listkeys DZ "1-0 Poke N-Next P-Prev X-Exit"
.nopoklist DZ "No .pok for it in Poke List"
.askinput WORD .asktitle, .asklabel, .askhelp1, .askhelp2
    BYTE 3, 0
.asktitle DZ "Poke value"
.asklabel DZ "Value 0-255:"
.askhelp1 DZ "ENTER to poke, BREAK to cancel"
.askhelp2 DZ "This cheat needs a number"
.badvalue DZ "That is not 0 to 255"
.pokedtext DZ "Pokes made: "
.skippedtext DZ "Not made: "
.skippedwhy DZ "(they need another 128K bank)"

.active_edit: byte 0  ; 1 is address, 2 is value
.address_input: block 8
.value_input: block 5


.startmenu_def:
    MENU_ITEM 0, 1, 1, .title, 0
    MENU_ITEM 'a', 3, 2, .addressmsg, .handle_address
    MENU_ITEM 'v', 5, 2, .valuemsg, .handle_value
    MENU_ITEM 'p', 7, 2, .pokemsg, .handle_poke
    MENU_ITEM 'l', 9, 2, .listmsg, .handle_list
    MENU_ITEM 'x', 11, 2, .exitmsg,  .handle_exit
    MENU_END
.listmsg: DZ "Poke list (.pok file)"
.title DZ "Apply Poke"
.addressmsg: DZ "Address: "
.valuemsg: DZ "Value: "
.pokemsg: DZ "Poke"
.exitmsg: DZ "Exit"




; -------------------------------------------

     STRUCT SNAHEADER
I    BYTE
HLx  WORD
DEx  WORD
BCx  WORD
AFx  WORD
HL   WORD
DE   WORD
BC   WORD
IY   WORD
IX   WORD
IFF2 BYTE
R    BYTE
AF   WORD
SP   WORD
IM   BYTE
BORD BYTE
     ENDS


; -------------------------------------------

// The fields that follow the z80 header of a 128K snapshot (Z80X_7FFD is
// just after the 30 bytes of the header)
Z80X_7FFD equ 30            // the value last written to port 0x7FFD
Z80X_AYFLAG equ 31          // non zero if the sound chip registers are valid
Z80X_AYSEL equ 32           // the register of the sound chip that was selected
Z80X_AYREGS equ 33          // the 16 registers of the sound chip
Z80X_SIZE equ 49

     STRUCT Z80HEADER
A    BYTE
F    BYTE
BC   WORD
HL   WORD
PC   WORD
SP   WORD
I    BYTE
R    BYTE
FLAGS1 BYTE
DE   WORD
BCx WORD
DEx WORD
HLx WORD
Ax  BYTE
Fx  BYTE

IY  WORD
IX  WORD

EI BYTE
IFF2 BYTE
FLAGS2 BYTE
    ENDS


// Sends the snapshot to the Pico.  A machine that can page memory gets a 128K
// snapshot, others the 48K one.
//
// The 128K machine's port 7FFD can't be read, so what bank was paged in at 0xC000
// is guessed from BANK_M and checked by the hash, which the Pico compares with the
// banks it receives.  Paging can't be changed if the machine is in 48K mode (or has
// no paging): this is found by writing to 0xC000 with two banks paged in.
savesnapshot:
    ld a, (start_up_machine48)
    or a
    jp nz, sendsnapshottopico
    push ix
    ld ix, 0
    call getimmode
    ld (sna_on_entry + SNAHEADER.IM), a
    ld a, (BANK_M)
    ld (save128_extra.bankm), a

    // the PC that NMI pushed is at the top of the stack
    ld hl, (sna_on_entry + SNAHEADER.SP)
    ld e, (hl)
    inc hl
    ld d, (hl)
    ld (save128_extra.pc), de

    ld hl, save128_extra.ay
    ld bc, 0xfffd
    ld e, 0
1:  out (c), e
    in a, (c)
    ld (hl), a
    inc hl
    inc e
    ld a, e
    cp 16
    jr nz, 1B

    // s1 = sum of the bytes, s2 = sum of the s1s, in 16 bits
    ld hl, HASH_START
    ld bc, HASH_LEN
    ld de, 0
    ld ix, 0
1:  ld a, (hl)
    add a, e
    ld e, a
    adc a, d
    sub e
    ld d, a
    add ix, de
    inc hl
    dec bc
    ld a, b
    or c
    jr nz, 1B
    ld (save128_extra.s1), de
    ld (save128_extra.s2), ix
    ld ix, 0

    // can banks 0 and 1 be paged in?  Put back what was there either way
    ld bc, 0x7ffd
    ld hl, 0xC000
    xor a
    out (c), a
    ld d, (hl)
    ld (hl), 0x55
    ld a, 1
    out (c), a
    ld e, (hl)
    ld (hl), 0xAA
    xor a
    out (c), a
    ld a, (hl)
    cp 0x55
    jr nz, .nopaging
    ld (hl), d
    ld a, 1
    out (c), a
    ld (hl), e
    jr .paging

.nopaging
    ld (hl), d
    pop ix
    jp sendsnapshottopico

.paging
    ld hl, sna_on_entry
    ld (ix + 2), hl
    ld (ix + 1), ACTION_SNA_BEGIN_WRITE128
    call save128_wait

    // bank 5 is the screen (the pico's copy) and the memory after it, bank 2 is next
    ld (ix + 1), ACTION_SNA_WRITE_SCREEN
    call save128_wait
    ld hl, 0x4000 + 6912
    ld de, 0x4000 - 6912
    call save128_send
    ld hl, 0x8000
    ld de, 0x4000
    call save128_send

    // then the banks that are paged in at 0xC000, from page_order[2]
    ld hl, page_order + 2
    ld b, 6
.banks
    push bc
    push hl
    ld a, (hl)
    ld bc, 0x7ffd
    out (c), a
    ld hl, 0xC000
    ld de, 0x4000
    call save128_send
    pop hl
    inc hl
    pop bc
    djnz .banks

    ld (ix + 1), ACTION_SNA_END_WRITE128
    call save128_wait
    ld a, (ix + 2)
    ld bc, 0x7ffd
    out (c), a
    pop ix
    ret

// tell the Pico there is a request and wait for it to be dealt with
save128_wait:
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B
    ret

// send DE bytes from HL to the Pico, up to 1024 at a time through spare_space
save128_send:
    ld a, d
    or e
    ret z
    ld bc, 1024
    ld a, d
    cp 4
    jr nc, 1F
    ld b, d
    ld c, e
1:  push hl
    ex de, hl
    or a
    sbc hl, bc
    ex (sp), hl
    ld de, spare_space
    ld (ix + 2), de
    ld (ix + 4), bc
    ld (ix + 1), ACTION_SNA_NEXT_WRITE
    ldir
    call save128_wait
    pop de
    jr save128_send


// Shows the controls and instructions page by page. a = 0 for the file last previewed,
// 1 for the game last loaded, 2 for the SD card set up (the pico draws it). N goes to the next page and P to the previous, both going
// round; only X (or BREAK) ends it, and ENTER, which is returned in a for the caller (V
// loads the game with it). Other keys do nothing.
showgameinfo:
    ld (.infomode), a
    xor a
    ld (.infopage), a
    ld (.infopic), a
    ld (.infoop), a
.infoshow:
    ld ix, 0
    ld a, (.infomode)
    ld (ix + 2), a
    ld a, (.infopage)
    ld (ix + 3), a
    ld a, (.infopic)
    ld (ix + 4), a
    ld a, (.infoop)
    ld (ix + 5), a          // 0, 1 asking to delete the grab shown, 2 delete it, 3 start the notes editor, 4 a key for it
    ld a, (.infokeyval)
    ld (ix + 6), a
    ld (ix + 1), ACTION_GAME_INFO
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B
    ld b, a
    and 15
    ld (.infopages), a      // the low four bits
    ld a, b
    rrca
    rrca
    rrca
    rrca
    and 15
    ld (.infopics), a       // the high four bits
    ld a, (ix + 1)          // the pico sets this to 1 while it asks Y or N, 3 while the notes are edited
    ld (.infoop), a
    ld a, (ix + 3)          // the page, as the pico kept it
    ld (.infopage), a
    call previewfrompico
    ld hl, PICTURE_DELAY
    ld (.infotimer), hl
.infokey:
    call getkeypress
    or a
    jr z, .infoidle
    cp 255
    jr z, .infoidle
    push af
    call waitforkeyrelease
    pop af
    ld b, a
    ld hl, .infoop
    ld a, (hl)
    cp 3
    jr z, .infotyped        // the notes editor takes every key
    or a
    ld a, b
    jr z, 2F
    cp 'y'                  // asked to delete: Y does, any other key does not
    ld a, 0
    jr nz, 3F
    ld a, 2
3:  ld (hl), a
    jp .infoshow
2:  ld hl, .infopage
    cp 'd'                  // D: delete the grab shown (the pico ignores it for other pictures)
    jr nz, 4F
    ld a, 1
    ld (.infoop), a
    jp .infoshow
4:  cp 'm'                  // M: edit the notes
    jr nz, 5F
    ld a, 3
    ld (.infoop), a
    jp .infoshow
5:  cp 'n'
    jr z, .infonext
    cp 'p'
    jr z, .infoprev
    cp 'x'
    ret z
    cp 0x13                 // ENTER
    ret z
    cp 0x18                 // BREAK
    jr nz, 1F
    ld a, 'x'
    ret
1:  jr .infokey            // any other key does nothing
.infotyped:
    ld a, b
    ld (.infokeyval), a
    ld a, 4
    ld (hl), a
    jp .infoshow
// No key: with several pictures, the next one after PICTURE_DELAY of these, while the
// first page shows (it is the one with the picture), going round
.infoidle:
    ld a, (.infoop)         // no cycling while it asks
    or a
    jr nz, .infokey
    ld a, (.infopics)
    cp 2
    jr c, .infokey
    ld a, (.infopage)
    or a
    jr nz, .infokey
    ld hl, (.infotimer)
    dec hl
    ld (.infotimer), hl
    ld a, h
    or l
    jp nz, .infokey
    ld a, (.infopics)
    ld c, a
    ld a, (.infopic)
    inc a
    cp c
    jr c, 3F
    xor a
3:  ld (.infopic), a
    jp .infoshow
.infonext:
    ld a, (.infopages)
    dec a
    jp z, .infokey          // one page
    cp (hl)
    jr z, 1F
    inc (hl)
    jp .infoshow
1:  ld (hl), 0              // after the last page the first
    jp .infoshow
.infoprev:
    ld a, (.infopages)
    dec a
    jp z, .infokey
    ld a, (hl)
    or a
    jr z, 1F
    dec (hl)
    jp .infoshow
1:  ld a, (.infopages)
    dec a
    ld (hl), a              // before the first page the last
    jp .infoshow
.infokeyval: BYTE 0
.infomode: BYTE 0
.infoop: BYTE 0
.infopage: BYTE 0
.infopages: BYTE 0
.infopic: BYTE 0
.infotimer: WORD 0
.infopics: BYTE 0

// Sends the screen to the pico (screentopico) or brings it back
// (screenfrompico), SCREEN_PIECE bytes at a time through spare_space + MOVE_INFO,
// which is above the list, so a list survives a preview.
screentopico:
    ld c, ACTION_SCREEN_STASH
    jr 1F
screenfrompico:
    ld c, ACTION_SCREEN_RESTORE
    jr 1F
// shows the screen that ACTION_SNAP_PREVIEW read
previewfrompico:
    ld c, ACTION_PREVIEW_READ
1:  push ix
    ld ix, 0
    ld hl, SCREEN
    ld b, 6912 / SCREEN_PIECE
.piece
    push bc
    ld (ix + 1), c
    ld de, spare_space + MOVE_INFO
    ld (ix + 2), de
    ld (ix + 4), hl
    ld de, SCREEN_PIECE
    ld (ix + 6), de
    ld a, c
    ld de, spare_space + MOVE_INFO
    ld bc, SCREEN_PIECE
    cp ACTION_SCREEN_STASH
    jr nz, .fetch
    ldir                    // hl moves on to the next piece
    call save128_wait
    jr .next
.fetch
    call save128_wait
    ex de, hl
    ldir
    ex de, hl               // hl is the next piece
.next
    pop bc
    djnz .piece
    pop ix
    ret

sendsnapshottopico:
    push ix
    ld ix, 0
    call getimmode
    ld (sna_on_entry + SNAHEADER.IM), a
    ld hl, sna_on_entry
    ld (ix + 2), hl
    ld (ix + 1), ACTION_SNA_BEGIN_WRITE
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B


    // the screen comes from the pico's copy
    ld (ix + 1), ACTION_SNA_WRITE_SCREEN
    call save128_wait

    // the 42240 bytes after the screen go through spare_space, a piece at a time
    ld b, SNAP_SEND_BLOCKS
    ld hl, 16384 + 6912

.transfer
    push bc
    ld de, spare_space
    ld bc, SNAP_SEND_BLOCK
    ld (ix + 2), de
    ld (ix + 4), bc
    ld (ix + 1), ACTION_SNA_NEXT_WRITE
    ldir
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B
    pop bc
    djnz .transfer
    pop ix

    ret


// enter with hl pointing to the snapshot name string
//            a  holding the drive byte
loadsnapshot:
    ld ix, 0
    ld (ix + 2), hl
    ld hl, sna_header
    ld (ix + 4), a
    ld (ix + 5), hl
    ld (ix + 1), ACTION_BEGIN_SNAP_READ
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B

    ld a, (ix + 2)
    cp 2
    jp z, load_paged_snapshot

    // A 48K snapshot: the memory above 0xC000 is bank 0 and the ROM is the 48K one, as
    // the pico says (it is the port 0x7FFD value in the second byte of param1).  On a
    // 128K machine whatever was paged in for the NMI menu would be left otherwise, and
    // might be a slow (contended) bank.
    ld bc, 0x7ffd
    ld a, (ix + 3)
    out (c), a
    ld a, (ix + 2)
    cp 1
    jp z, load_z80_snapshot



    ld a, (sna_header + SNAHEADER.BORD)
    and 7
    out (0xfe), a    

    ld b, 49152 / LOAD_BLOCK
    ld de, 16384

.transfer
    push bc
    ld hl, spare_space
    ld bc, LOAD_BLOCK
    ld (ix + 2), hl
    ld (ix + 4), bc
    ld (ix + 1), ACTION_SNAP_READ_NEXT
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B

    ldir

    pop bc
    djnz .transfer


    ld a, (sna_header + SNAHEADER.I)
    ld i, a
    ld a, (sna_header + SNAHEADER.IM)
    cp 2
    jr z, .im2mode
    im 1
    jr 1F
.im2mode
    im 2
1:
    exx
    ex af, af'
    ld hl, (sna_header + SNAHEADER.AFx)
    push hl
    pop af
    ld hl, (sna_header + SNAHEADER.HLx)
    ld de, (sna_header + SNAHEADER.DEx)
    ld bc, (sna_header + SNAHEADER.BCx)
    exx
    ex af, af'
    ld hl, (sna_header + SNAHEADER.AF)
    push hl
    ld hl, (sna_header + SNAHEADER.HL)
    ld de, (sna_header + SNAHEADER.DE)
    ld bc, (sna_header + SNAHEADER.BC)
    ld iy, (sna_header + SNAHEADER.IY)
    ld ix, (sna_header + SNAHEADER.IX)
    ld a, (sna_header + SNAHEADER.IFF2)

    bit 2, a

    jr z, .di

    pop af
    ld sp, (sna_header + SNAHEADER.SP)
    jp ei_exitnmi


.di:
    pop af
    ld sp, (sna_header + SNAHEADER.SP)
    di
    jp exitnmi


load_z80_snapshot:

    ld a, (sna_header + Z80HEADER.FLAGS1)
    srl a
    and 7
    out (0xfe), a    

    ld b, 49152 / LOAD_BLOCK
    ld de, 16384

.transfer
    push bc
    ld hl, spare_space
    ld bc, LOAD_BLOCK
    ld (ix + 2), hl
    ld (ix + 4), bc
    ld (ix + 1), ACTION_SNAP_READ_NEXT
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B

    ldir

    pop bc
    djnz .transfer

z80_registers:
    // Prepare some self modifying code to jump to address where the z80
    // snapshot starts.
    // When the instruction has been fully read the pico knows
    // so disable the nmi rom.
    ld a, 0xC3 // JMP instruction
    ld (exitnmi - 2), a
    ld hl, (sna_header + Z80HEADER.PC)
    ld (exitnmi - 1), hl

    ld a, (sna_header + Z80HEADER.EI)
    cp 0
    jr z, .di
    ld a, 0xFB // opcode EI
    jr 1F
.di
    ld a, 0xF3 // opcode DI
1:
    ld (exitnmi - 3), a


    ld a, (sna_header + Z80HEADER.I)
    ld i, a
    ld a, (sna_header + Z80HEADER.FLAGS2)
    and 0x3
    cp 2
    jr z, .im2mode
    im 1
    jr 1F
.im2mode
    im 2
1:
    exx
    ex af, af'
    ld a, (sna_header + Z80HEADER.Ax)
    ld h, a
    ld a, (sna_header + Z80HEADER.Fx)
    ld l, a
    push hl
    pop af
    ld hl, (sna_header + Z80HEADER.HLx)
    ld de, (sna_header + Z80HEADER.DEx)
    ld bc, (sna_header + Z80HEADER.BCx)
    exx
    ex af, af'
    ld a, (sna_header + Z80HEADER.A)
    ld h, a
    ld a, (sna_header + Z80HEADER.F)
    ld l, a
    push hl
    ld hl, (sna_header + Z80HEADER.HL)
    ld de, (sna_header + Z80HEADER.DE)
    ld bc, (sna_header + Z80HEADER.BC)
    ld iy, (sna_header + Z80HEADER.IY)
    ld ix, (sna_header + Z80HEADER.IX)
    pop af
    ld sp, (sna_header + Z80HEADER.SP)
    jp exitnmi - 3


// A 128K snapshot.  The eight banks are read 4096 bytes at a time in the order
// of page_order and each is paged in at 0xC000 for the copy, apart from the
// first two which are the banks that are always at 0x4000 and 0x8000.  Then the
// sound chip registers (if the snapshot has them) and the paging port are set
// and the rest is as for a z80 snapshot.
load_paged_snapshot:
    ld a, (sna_header + Z80HEADER.FLAGS1)
    srl a
    and 7
    out (0xfe), a

    ld hl, page_order
    ld de, 0x4000
    ld b, 8
.page:
    push bc
    push hl
    ld a, (hl)
    ld bc, 0x7ffd
    out (c), a
    ld b, 16384 / LOAD_BLOCK
.chunk:
    push bc
    ld hl, spare_space
    ld bc, LOAD_BLOCK
    ld (ix + 2), hl
    ld (ix + 4), bc
    ld (ix + 1), ACTION_SNAP_READ_NEXT
    ld (ix + 0), 255
1:  ld a, (ix)
    cp 255
    jr z, 1B
    ldir
    pop bc
    djnz .chunk
    ld a, d             // de wraps to 0 after the bank that ends at 0xFFFF
    or a
    jr nz, 1F
    ld d, 0xc0
1:  pop hl
    inc hl
    pop bc
    djnz .page

    ld a, (sna_header + Z80X_AYFLAG)
    or a
    jr z, .noay
    ld hl, sna_header + Z80X_AYREGS
    ld e, 0
.ay:
    ld bc, 0xfffd
    out (c), e
    ld b, 0xbf
    ld a, (hl)
    out (c), a
    inc hl
    inc e
    ld a, e
    cp 16
    jr nz, .ay
    ld b, 0xff
    ld a, (sna_header + Z80X_AYSEL)
    out (c), a
.noay:
    ld a, (sna_header + Z80X_7FFD)
    ld bc, 0x7ffd
    out (c), a
    jp z80_registers

// The banks (the values for port 0x7FFD) in the order the pico sends them,
// which must be the same as PAGE_ORDER in snapload.h
page_order: defb 5, 2, 0, 1, 3, 4, 6, 7

   
getimmode:
    ld de, 0x3E3E
    ld hl, im2handler
    ld bc, IM2HANDLER_SIZE
    ldir

    ld hl, 0x3D00
    ld a, 0x3E
    ld (hl), a
    ld de, 0x3D01
    ld bc, 256
    ldir

    ld a, i
    ld c, a

    ld a, 0x3D
    ld i, a

    ld b, 0
    ei
    halt
    di
    ld a, c
    ld i, a
    ld a, b    
    ret


; Pauses for a while.
; de: wait time, ca. de*0.1ms
pause:
	push af
	push bc

pause_loop_l2:
	ld b,26
pause_loop_l1:
	djnz pause_loop_l1 ; 1 cycle should be roughly 100us=0.1ms

	dec de
	ld a,d
	or e
	jr nz,pause_loop_l2

	pop bc
	pop af
	ret


// de location (y, x) and hl points to string
putstring:
   
1   ld a, (hl)
    or a
    ret z
    ld c, a
    call putchar
    inc hl
    jr 1B


putchar:
    push af, de, bc, hl

    ld h, 0
    ld l, c
    add hl, hl
    add hl, hl
    add hl, hl
    ld bc, charbank - 32 * 8
    add hl, bc

    // de holds y/x character position
    // 
    ld a, d
    and 0b111
    rrca
    rrca
    rrca
    or e
    ld e, a

    ld a, d
    and 0b00011000
    or  0b01000000
    ld d, a

    ld b, 8

1   ld a, (hl)
    ld (de), a
    inc hl
    inc d
    djnz 1B

    // and the colour, if text_attr says (d has gone 8 lines past the character)
    ld a, (text_attr)
    or a
    jr z, 2F
    ld c, a
    ld a, d
    sub 8
    and 0b00011000
    rrca
    rrca
    rrca
    or 0x58
    ld d, a
    ld a, c
    ld (de), a

2:  pop hl, bc, de, af
    inc e
    ret

// the attribute putchar gives each character, 0 to leave the colour alone
text_attr BYTE 0


// de holds the y, x position to reverse
// b holds the count
// a the value to set

putattributes:
    ld hl, SCREEN_ATTRIB
    ld l, e
    ld e, 0
    srl d
    rr e
    srl d
    rr e
    srl d
    rr e
    add hl, de

1:  ld (hl), a
    inc hl
    djnz 1B
    ret    


// de holds the y, x position to reverse
reverseattr:
    ld hl, SCREEN_ATTRIB
    ld l, e
    ld e, 0
    srl d
    rr e
    srl d
    rr e
    srl d
    rr e
    add hl, de

    ld a, (hl)
    and a, 0b111
    sla a
    sla a
    sla a
    ld e, a
    ld a, (hl)
    and a, 0b111000
    srl a
    srl a
    srl a
    or e
    ld e, a
    ld a, (hl)
    and 0b11000000
    or e

    ld (hl), a
    ret




clearscreen:
    // Clear MENU_ROWS lines of attributes
    ld hl, SCREEN_ATTRIB
    ld a, 0b000111  // black paper, white ink
    ld (hl), a
    ld de, SCREEN_ATTRIB + 1
    ld bc, 32 * MENU_ROWS - 1
    ldir
    ASSERT MENU_ROWS > 16 && MENU_ROWS <= 24

    xor a
    // Clear 16 lines of pixel data, this is contiguous
    ld hl, SCREEN
    ld (hl),a
    ld de, SCREEN + 1
    ld bc, 32 * 16 * 8 - 1   // clear 16 rows of 32 characters each that is 8 pixels high
    ldir

    // Then the clear the lines from the 17th to MENU_ROWS a row of pixels at a time

    ld hl, SCREEN + 32 * 16 * 8
    xor a
    ld b, 8
.nextrow
    push hl
    push bc
    ld de, hl
    inc de
    ld (hl), a
    ld bc, 32 * (MENU_ROWS - 16) - 1
    ldir
    pop bc
    pop hl
    inc h
    djnz .nextrow

    // the PICOFACE title across row 0, on every menu screen
    ld de, 0
    ld b, 32
    ld a, TITLE_ATTR
    call putattributes
    ld de, 0x000C
    ld hl, .banner
    call putstring
    ret

.banner DZ "PICOMENU"

// The big PICOFACE title of the main menu: rows 0 to 2 blue, with the name in
// letters twice as wide and high as the menu's, from the same font.
bigtitle:
    ld de, 0
    ld b, 96
    ld a, TITLE_ATTR
    call putattributes
    ld hl, SCREEN               // clear the pixels of the three rows
    ld a, 8
1:  push af
    push hl
    ld d, h
    ld e, l
    inc de
    ld (hl), 0
    ld bc, 95
    ldir
    pop hl
    inc h
    pop af
    dec a
    jr nz, 1B
    ld hl, clearscreen.banner
    ld e, 8                     // the name takes columns 8 to 23
2:  ld c, (hl)
    ld a, c
    or a
    ret z
    push hl
    push de
    call .bigchar
    pop de
    push de
    call .colour
    pop de
    pop hl
    inc hl
    inc e
    inc e
    jr 2B

// e the column of a letter: its two cells on the three rows get its colour
.colour:
    ld a, e
    rrca
    and 3
    ld c, a
    ld b, 0
    ld hl, .inks
    add hl, bc
    ld a, (hl)
    ld hl, SCREEN_ATTRIB
    ld d, 0
    add hl, de
    ld bc, 31
    ld (hl), a
    inc hl
    ld (hl), a
    add hl, bc
    ld (hl), a
    inc hl
    ld (hl), a
    add hl, bc
    ld (hl), a
    inc hl
    ld (hl), a
    ret
// red, yellow, green and cyan letters, bright, on blue
.inks:
    defb 0b01001010, 0b01001110, 0b01001100, 0b01001101

// c the character, e the column; draws it 16 by 16 pixels from pixel line 3
.bigchar:
    ld h, 0
    ld l, c
    add hl, hl
    add hl, hl
    add hl, hl
    ld bc, charbank - 32 * 8
    add hl, bc
    ld d, 0x43
    ld b, 8
1:  push bc
    ld a, (hl)
    inc hl
    push hl
    push af
    rrca
    rrca
    rrca
    rrca
    and 15
    call .widen
    ex af, af'
    pop af
    and 15
    call .widen
    ld c, a                     // the right half
    ex af, af'
    ld b, a                     // the left half
    call .twolines
    call .twolines
    pop hl
    pop bc
    djnz 1B
    ret

.twolines:
    ld a, b
    ld (de), a
    inc e
    ld a, c
    ld (de), a
    dec e
    // down a pixel line, into the next character row at the bottom of a cell
    inc d
    ld a, d
    and 7
    ret nz
    ld a, d
    sub 8
    ld d, a
    ld a, e
    add a, 32
    ld e, a
    ret

// a 4-bit number to the byte with each of its bits twice
.widen:
    ld hl, .nibbles
    ld c, a
    ld b, 0
    add hl, bc
    ld a, (hl)
    ret
.nibbles:
    defb 0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F
    defb 0xC0, 0xC3, 0xCC, 0xCF, 0xF0, 0xF3, 0xFC, 0xFF


waitforkeypress:
    call getkeypress
    cp 0
    jr z, waitforkeypress
    cp 255
    jr z, waitforkeypress
    ret

waitforkeyrelease:
    ld de, 1000
    call pause
    call getkeypress
    cp 0
    jr nz, waitforkeyrelease
    ld de, 100
    call pause
    ret
    

// FEFF Shift, Z, X, C, V
// FDFF A, S, D, F, G
// FBFF Q W E R T
// F7FF 1 2 3 4 5
// EFFF 0 9 8 7 6
// DFFF P O I U Y
// BFFF /n L K J H
// 7FFF spc ss M N B

getkeypress:
    ld h, 0 // shift code
    ld l, 0 // key index press
    ld e, 0 // current key index

    ld b, 0xFE


.scanloop
    ld c, 0xFE

    in a, (c)

    bit 0, b
    jr nz, .checksymshift
    rrca
    jr c, .countbits
    ld h, 1
    jr .countbits

.checksymshift
    bit 7, b
    jr nz, .countbits
    rra
    rra
    jr c, .symshiftnotpressed
.symshiftpressed
    set 1, h
.symshiftnotpressed
    rlca

.countbits:
    push bc
    ld b, 5
1:
    inc e
    rra
    jr c, .nokey

    ld c, a

    xor a
    cp l
    jr z, .firstkeypress

.secondkeypress
    pop bc
    ld a, 0xFF
    ret


.firstkeypress
    ld a, c
    ld l, e
.nokey
    djnz 1B

    pop bc
    scf
    rl b
    jr c, .scanloop


    xor a
    cp l
    ret z


    bit 0, h
    jr z, 1F
    ld de, keytable1 - 1
    jr 3F
1:
    bit 1, h
    jr z, 1F
    ld de, keytable2 - 1
    jr 3F
1:
    ld de, keytable0 - 1
3:
    ld h, 0
    add hl, de
    ld a, (hl)
    ret

keytable0:
    defb 'z', 'x', 'c', 'v', '~'
    defb 'a', 's', 'd', 'f','g'
    defb 'q', 'w', 'e', 'r','t'
    defb '1', '2', '3', '4','5'
    defb '0', '9', '8', '7','6'
    defb 'p', 'o', 'i', 'u','y'
    defb 0x13, 'l', 'k', 'j','h'
    defb ' ', 'm', 'n', 'b', '~'
; shifted keys
keytable1:
    defb 'Z', 'X', 'C', 'V', '~'
    defb 'A', 'S', 'D', 'F','G'
    defb 'Q', 'W', 'E', 'R','T'
    defb 0x1, 0x2, 0x3, 0x4, 0x5
    defb 0xA, 0x9, 0x8, 0x7, 0x6
    defb 'P', 'O', 'I', 'U','Y'
    defb 0x13, 'L', 'K', 'J','H'
    defb 0x18, 'M', 'N', 'B', '~'

; symshifted keys
keytable2:
    defb ':', 0x60, '?', '/', '~'
    defb '~', '|', '\', '{','}'
    defb '~', '~', '~', '<','>'
    defb '!', '@', '#', '$','%'
    defb '_', ')', '(', "\'", '&'
    defb '"', ';', '~', ']','['
    defb 0x13, '=', '+', '-','^'
    defb ' ', '.', ',', '*', '~'    

charbank:
	; Carton Bold font from https://damieng.com/zx-origins
	defb 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 ;  
	defb 0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00 ; !
	defb 0x6c,0x6c,0x00,0x00,0x00,0x00,0x00,0x00 ; "
	defb 0x00,0x6c,0xfe,0x6c,0x6c,0xfe,0x6c,0x00 ; #
	defb 0x38,0x6c,0x68,0x3c,0x16,0x56,0x3c,0x10 ; $
	defb 0x40,0xec,0x58,0x30,0x64,0xce,0x04,0x00 ; %
	defb 0x70,0xd8,0xd8,0x76,0xdc,0xdc,0x66,0x00 ; 0x
	defb 0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00 ; '
	defb 0x0c,0x18,0x30,0x30,0x30,0x18,0x0c,0x00 ; (
	defb 0x30,0x18,0x0c,0x0c,0x0c,0x18,0x30,0x00 ; )
	defb 0x18,0x7e,0x3c,0x3c,0x7e,0x18,0x00,0x00 ; *
	defb 0x00,0x18,0x18,0x7e,0x18,0x18,0x00,0x00 ; +
	defb 0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x30 ; ,
	defb 0x00,0x00,0x00,0xfe,0x00,0x00,0x00,0x00 ; -
	defb 0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x00 ; .
	defb 0x02,0x06,0x0c,0x18,0x30,0x60,0x40,0x00 ; /
	defb 0x38,0x6c,0xc6,0xc6,0xc6,0x6c,0x38,0x00 ; 0
	defb 0x08,0x18,0x18,0x18,0x18,0x18,0x18,0x00 ; 1
	defb 0x3c,0x46,0x06,0x0c,0x18,0x30,0x7e,0x00 ; 2
	defb 0x7e,0x0c,0x18,0x0c,0x06,0x46,0x3c,0x00 ; 3
	defb 0x0c,0x1c,0x3c,0x6c,0xcc,0xfe,0x0c,0x00 ; 4
	defb 0x7e,0x60,0x7c,0x46,0x06,0x46,0x3c,0x00 ; 5
	defb 0x0c,0x18,0x30,0x7c,0x66,0x66,0x3c,0x00 ; 6
	defb 0x7e,0x06,0x06,0x0c,0x18,0x30,0x30,0x00 ; 7
	defb 0x3c,0x66,0x66,0x3c,0x66,0x66,0x3c,0x00 ; 8
	defb 0x3c,0x66,0x66,0x3e,0x0c,0x18,0x30,0x00 ; 9
	defb 0x00,0x00,0x18,0x00,0x00,0x00,0x18,0x00 ; :
	defb 0x00,0x00,0x18,0x00,0x00,0x00,0x18,0x30 ; ;
	defb 0x0c,0x18,0x30,0x60,0x30,0x18,0x0c,0x00 ; <
	defb 0x00,0x00,0x7e,0x00,0x7e,0x00,0x00,0x00 ; =
	defb 0x30,0x18,0x0c,0x06,0x0c,0x18,0x30,0x00 ; >
	defb 0x1c,0x26,0x06,0x0c,0x18,0x00,0x18,0x00 ; ?
	defb 0x3c,0x42,0x9d,0xb5,0xb5,0x9e,0x40,0x3e ; @
	defb 0x38,0x38,0x6c,0x6c,0x7c,0xc6,0xc6,0x00 ; A
	defb 0xf8,0xcc,0xcc,0xfc,0xc6,0xc6,0xfc,0x00 ; B
	defb 0x3c,0x62,0xc0,0xc0,0xc0,0x62,0x3c,0x00 ; C
	defb 0xf8,0xcc,0xc6,0xc6,0xc6,0xcc,0xf8,0x00 ; D
	defb 0x7e,0x60,0x60,0x7c,0x60,0x60,0x7e,0x00 ; E
	defb 0x7e,0x60,0x60,0x7c,0x60,0x60,0x60,0x00 ; F
	defb 0x3c,0x62,0xc0,0xce,0xc6,0x66,0x3c,0x00 ; G
	defb 0xc6,0xc6,0xc6,0xfe,0xc6,0xc6,0xc6,0x00 ; H
	defb 0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00 ; I
	defb 0x0c,0x0c,0x0c,0x0c,0x0c,0x4c,0x38,0x00 ; J
	defb 0xc6,0xcc,0xd8,0xf0,0xd8,0xcc,0xc6,0x00 ; K
	defb 0x60,0x60,0x60,0x60,0x60,0x60,0x7e,0x00 ; L
	defb 0xc6,0xee,0xfe,0xd6,0xc6,0xc6,0xc6,0x00 ; M
	defb 0xc6,0xe6,0xf6,0xde,0xce,0xc6,0xc6,0x00 ; N
	defb 0x38,0x6c,0xc6,0xc6,0xc6,0x6c,0x38,0x00 ; O
	defb 0x7c,0x66,0x66,0x7c,0x60,0x60,0x60,0x00 ; P
	defb 0x38,0x6c,0xc6,0xc6,0xd6,0x6c,0x36,0x00 ; Q
	defb 0x7c,0x66,0x66,0x7c,0x78,0x6c,0x66,0x00 ; R
	defb 0x38,0x64,0x60,0x3c,0x06,0x46,0x3c,0x00 ; S
	defb 0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x00 ; T
	defb 0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0x7c,0x00 ; U
	defb 0xc6,0xc6,0x6c,0x6c,0x6c,0x38,0x38,0x00 ; V
	defb 0xc6,0xc6,0xd6,0xd6,0xfe,0xee,0x44,0x00 ; W
	defb 0xc6,0xc6,0x6c,0x38,0x6c,0xc6,0xc6,0x00 ; X
	defb 0xc3,0x66,0x3c,0x18,0x18,0x18,0x18,0x00 ; Y
	defb 0x7e,0x06,0x0c,0x18,0x30,0x60,0x7e,0x00 ; Z
	defb 0x3c,0x30,0x30,0x30,0x30,0x30,0x3c,0x00 ; [
	defb 0x40,0x60,0x30,0x18,0x0c,0x06,0x02,0x00 ; \
	defb 0x3c,0x0c,0x0c,0x0c,0x0c,0x0c,0x3c,0x00 ; ]
	defb 0x10,0x38,0x6c,0xc6,0x00,0x00,0x00,0x00 ; ^
	defb 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff ; _
	defb 0x1c,0x32,0x30,0xf8,0x60,0x60,0xfe,0x00 ; £
	defb 0x00,0x00,0x3c,0x06,0x3e,0x66,0x3e,0x00 ; a
	defb 0x60,0x60,0x7c,0x66,0x66,0x66,0x7c,0x00 ; b
	defb 0x00,0x00,0x3c,0x62,0x60,0x62,0x3c,0x00 ; c
	defb 0x06,0x06,0x3e,0x66,0x66,0x66,0x3e,0x00 ; d
	defb 0x00,0x00,0x3c,0x66,0x7e,0x60,0x3e,0x00 ; e
	defb 0x1c,0x32,0x30,0x7c,0x30,0x30,0x30,0x00 ; f
	defb 0x00,0x3e,0x66,0x3c,0x60,0x3c,0x46,0x7c ; g
	defb 0x60,0x60,0x7c,0x66,0x66,0x66,0x66,0x00 ; h
	defb 0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00 ; i
	defb 0x0c,0x00,0x0c,0x0c,0x0c,0x0c,0x0c,0x38 ; j
	defb 0x60,0x60,0x66,0x6c,0x78,0x6c,0x66,0x00 ; k
	defb 0x30,0x30,0x30,0x30,0x30,0x30,0x1c,0x00 ; l
	defb 0x00,0x00,0xac,0xfe,0xd6,0xd6,0xd6,0x00 ; m
	defb 0x00,0x00,0x7c,0x66,0x66,0x66,0x66,0x00 ; n
	defb 0x00,0x00,0x3c,0x66,0x66,0x66,0x3c,0x00 ; o
	defb 0x00,0x00,0x7c,0x66,0x66,0x7c,0x60,0x60 ; p
	defb 0x00,0x00,0x3e,0x66,0x66,0x3e,0x06,0x06 ; q
	defb 0x00,0x00,0x6c,0x72,0x60,0x60,0x60,0x00 ; r
	defb 0x00,0x00,0x3e,0x60,0x3c,0x06,0x7c,0x00 ; s
	defb 0x18,0x18,0x7e,0x18,0x18,0x18,0x0e,0x00 ; t
	defb 0x00,0x00,0x66,0x66,0x66,0x66,0x3e,0x00 ; u
	defb 0x00,0x00,0x66,0x66,0x66,0x3c,0x18,0x00 ; v
	defb 0x00,0x00,0xc6,0xd6,0xd6,0x7c,0x28,0x00 ; w
	defb 0x00,0x00,0x66,0x3c,0x18,0x3c,0x66,0x00 ; x
	defb 0x00,0x00,0x66,0x66,0x66,0x3c,0x18,0x70 ; y
	defb 0x00,0x00,0x7e,0x0c,0x18,0x30,0x7e,0x00 ; z
	defb 0x0e,0x18,0x18,0x70,0x18,0x18,0x0e,0x00 ; {
	defb 0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00 ; |
	defb 0x70,0x18,0x18,0x0e,0x18,0x18,0x70,0x00 ; }
	defb 0x32,0x7e,0x4c,0x00,0x00,0x00,0x00,0x00 ; ~
	defb 0x3c,0x42,0x9d,0xb1,0xb1,0x9d,0x42,0x3c ; ©



    SAVEBIN "nmirom.bin", 0x0, $
    SAVEBIN "fuserom.bin", 0x0, 0x4000



; Stack: this area is reserved for the stack
STACK_SIZE: equ 150    ; in words


; Reserve stack space
    defw 0  ; WPMEM, 2
stack_bottom:
    defs    STACK_SIZE*2, 0
stack_top:
    ;defw 0
    defw 0  ; WPMEM, 2    


// The z80 header of the snapshot being loaded, and for a 128K snapshot some
// more fields
sna_header: BLOCK Z80X_SIZE

spare_size: equ 0x4000 - $

// The screen goes to and from the pico in pieces of SCREEN_PIECE bytes, and
// snapshots are loaded LOAD_BLOCK bytes at a time, through spare_space.
SCREEN_PIECE equ 1152          // 6 pieces (4 of 1728 needed 2752 bytes of spare_space with MOVE_INFO)
LOAD_BLOCK equ 2048
    ASSERT spare_size >= MOVE_INFO + SCREEN_PIECE
    ASSERT SCREEN_PIECE * (6912 / SCREEN_PIECE) == 6912
    ASSERT spare_size >= SCREEN_PIECE
    ASSERT LOAD_BLOCK * (16384 / LOAD_BLOCK) == 16384
    ASSERT spare_size >= LOAD_BLOCK
    // getimmode builds an interrupt table at 0x3D00 to 0x3E3E + IM2HANDLER_SIZE
    ASSERT spare_space <= 0x3D00

// sendsnapshottopico sends the memory after the screen in SNAP_SEND_BLOCKS
// pieces of SNAP_SEND_BLOCK bytes, each copied through spare_space.
SNAP_SEND_BLOCK equ 2112
SNAP_SEND_BLOCKS equ (49152 - 6912) / SNAP_SEND_BLOCK
    ASSERT SNAP_SEND_BLOCK * SNAP_SEND_BLOCKS == 49152 - 6912
    ASSERT spare_size >= SNAP_SEND_BLOCK
    // save128_send copies up to 1024 bytes at a time through spare_space
    ASSERT spare_size >= 1024
    // the memory load and save start up actions move spare_size_hi * 256 bytes at a time
    ASSERT spare_size_hi >= 1 && spare_size_hi * 256 <= spare_size
    // the move, rename and erase descriptions, after the longest list
    ASSERT spare_size >= MOVE_INFO + 3 * SNAPLIST_PATH_SIZE
    ASSERT MOVE_INFO >= SNAPLIST_STRINGS + 10 * (SNAPLIST_NAME_MAX + 1 + 4 + 1 + 9)

spare_space: BLOCK spare_space
   

