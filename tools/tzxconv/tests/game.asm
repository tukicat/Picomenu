; This file is part of picoFace.
;
; picoFace is free software: you can redistribute it and/or modify it under the terms of
; the GNU General Public License as published by the Free Software Foundation, either
; version 3 of the License, or (at your option) any later version.
;
; picoFace is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
; See the GNU General Public License for more details.
;
; You should have received a copy of the GNU General Public License along with picoFace. If
; not, see <https://www.gnu.org/licenses/>.

; The "game" the test tapes load: at 0x8000. Puts a marker at 0x9000, a border, and runs
; with interrupts on, counting frames at 0x9010, so the snapshot can be checked.
    device zxspectrum48
    org 0x8000
start:
    di
    ld sp, 0xFF00
    ld hl, marker
    ld de, 0x9000
    ld bc, 6
    ldir
    ld a, 5
    out (0xFE), a
    im 1
    ei
loop:
    halt
    ld hl, (0x9010)
    inc hl
    ld (0x9010), hl
    jr loop
marker:
    db "GAMEOK"
    ds 0x8800 - $, 0x77        ; padding so the block is 2 KB
    savebin "game.bin", start, 0x800
