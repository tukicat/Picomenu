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
; byte and the carry (load or verify) in both cases. The Pico answers 2 (loaded), 3 (failed)
; or 4: load it from the EAR socket (the Bluetooth tape), as the ROM would.
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
    ld a,1
    ld (MBOX),a             ; post the request
1:  ld a,(MBOX)
    cp 2
    jr c,1B                 ; until the Pico says 2 (done), 3 (failed) or 4 (from EAR)
    cp 4
    jr z,ear
    add ix,de               ; as the ROM leaves them
    cp 3                    ; carry: done
    push af
    ld hl,glyph             ; the mailbox was the (c) of the font: put it back
    ld de,MBOX
    ld bc,8
    ldir
    ld de,0
    pop af
exit:
    ret                     ; (the bus handler hands back to the internal ROM as this is read)
glyph:
    db 0x3C,0x42,0x99,0xA1,0xA1,0x99,0x42,0x3C
; 4: the block is to come from the EAR socket (B - Bluetooth tape plays it): the ROM's own
; LD-BYTES loads it. A', F', IX and DE are put back as they were at 0x0562, and what
; 0x0562-0x056A do (IN A,(FE) / RRA / AND 20 / OR 2 / LD C,A / CP A) is done here, as 0x0562
; is the hook's fetch: the RET then goes on at 0x056B (LD-BREAK) in the internal ROM.
; Only for a tape put in (T): with the patched 48K ROM, 0x056B is the hook's.
ear:
    ld a,(MBOX+6)
    rra                     ; carry: load
    inc d                   ; F' as the ROM's own INC D / EX AF,AF' / DEC D at 0x0556 leaves it:
    ld a,(MBOX+1)           ; the carry, and Z from INC D (NZ says the first byte is the flag)
    ex af,af'
    dec d
    push de
    ld hl,glyph
    ld de,MBOX
    ld bc,8
    ldir
    pop de
    ld hl,0x056B
    push hl
    in a,(0xfe)
    rra
    and 0x20
    or 2
    ld c,a
    cp a                    ; Z, as LD-BREAK wants
    jr exit
hook_end:
    ASSERT hook_end <= 0x05E3   ; LD-EDGE-2 and LD-EDGE-1 stay
    SAVEBIN "hook.bin", hook, hook_end - hook
    DISPLAY "hook ", /D, hook_end - hook, " bytes, exit at ", /H, exit, ", glyph at ", /H, glyph, ", ear at ", /H, ear
