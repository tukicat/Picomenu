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
; The source of TAPE_HOOK in picoface.cxx: sjasmplus tapehook.asm makes hook.bin
; The ROM trap for tapes: LD-BYTES from 0x0562. 0x0556-0x0561 stay as in the ROM
; (INC D, EX AF,AF', DEC D, DI, border, PUSH SA/LD-RET), so a game that does that itself
; and jumps to 0x0562 (Out Run, Gauntlet) finds the hook too. A' and F' hold the flag
; byte and the carry (load or verify) in both cases.
    DEVICE NOSLOT64K
MBOX equ 0x3FF8
    org 0x0562
hook:
    ex af,af'
    di
    ld (MBOX+1),a           ; flag
    ld (MBOX+2),ix          ; destination
    ld (MBOX+4),de          ; length
    sbc a,a                 ; carry: load (1) or verify (0)
    and 1
    ld (MBOX+6),a
    add ix,de               ; as the ROM leaves them
    ld a,1
    ld (MBOX),a             ; post the request
1:  ld a,(MBOX)
    cp 2
    jr c,1B                 ; until the Pico says 2 (done) or 3 (failed)
    cp 3                    ; carry: done
    push af
    ld hl,glyph             ; the mailbox was the (c) of the font: put it back
    ld de,MBOX
    ld bc,8
    ldir
    ld de,0
    pop af
    ret
glyph:
    db 0x3C,0x42,0x99,0xA1,0xA1,0x99,0x42,0x3C
hook_end:
    ASSERT hook_end <= 0x05E3   ; LD-EDGE-2 and LD-EDGE-1 stay
    SAVEBIN "hook.bin", hook, hook_end - hook
    DISPLAY "hook ", /D, hook_end - hook, " bytes, glyph at ", /H, glyph
