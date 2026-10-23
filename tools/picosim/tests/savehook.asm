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
;
; The source of TAPE_SAVE_HOOK in picoface.cxx (recording, W on the tape position screen):
; sjasmplus savehook.asm makes savehook.bin. It is served at 0x04C2 (SA-BYTES) while the Pico
; records; it hands back to the Spectrum's ROM when the last byte of its JP (0x04E2) is read.
    DEVICE NONE
    OUTPUT "savehook.bin"
    org 0x04C2
start:
    nop                     ; served from a constant, as the load hook's first byte is
    di
    ld (0x3FF9), a          ; flag
    ld (0x3FFA), ix         ; from
    ld (0x3FFC), de         ; length
    ld a, 2                 ; mode 2: save
    ld (0x3FFE), a
    ld a, 1                 ; request posted
    ld (0x3FF8), a
wait:
    ld a, (0x3FF8)
    cp 2
    jr c, wait
    jp 0x053F               ; SA/LD-RET in the Spectrum's own ROM: border, BREAK, EI
endjp:
