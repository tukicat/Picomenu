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

; A home-made turbo loader at 0x6000 (loaded with LOAD "" CODE). It reads its own blocks:
; a pilot of 1200 T pulses, a 400+400 T sync, bits of 400+400 T (0) or 800+800 T (1),
; a flag byte, the data, an XOR checksum. Block 1 is the screen at 0x4000, block 2 the game
; at 0x8000, which is then decrypted with the R register (as protected loaders do) and run.
; If a checksum is wrong: border red and HALT with interrupts off (a "failed" load).
    device zxspectrum48
    org 0x6000
start:
    di
    ld sp, 0x5FF0
    ld ix, 0x4000
    ld de, 6912
    ld a, 0x01                  ; flag of block 1
    call load_block
    jr nc, failed
    ld ix, 0x8000
    ld de, 0x800
    ld a, 0x02
    call load_block
    jr nc, failed
    ; decrypt the game: key = R, read in a loop whose R steps are fixed
    ld a, 0x80                  ; R7 set, to check the emulator keeps bit 7
    ld r, a
    ld hl, 0x8000
    ld bc, 0x800
dec_loop:
    ld a, r
    xor (hl)
    ld (hl), a
    inc hl
    dec bc
    ld a, b
    or c
    jr nz, dec_loop
    jp 0x8000
failed:
    ld a, 2
    out (0xFE), a
    halt

; Loads DE bytes to IX, the flag being A. Carry set if all went well.
load_block:
    ld (want_flag), a
    xor a
    ld c, a                     ; C: the last level (bit 6)
.pilot_start:
    ld h, 0                     ; pilot pulses seen in a row
.pilot:
    ld b, 0
    call edge
    jr nc, .pilot_start         ; nothing: keep waiting (no time limit here)
    ld a, b
    cp 14
    jr c, .short                ; shorter than a pilot pulse: sync?
    cp 34
    jr nc, .pilot_start         ; too long
    inc h
    jr nz, .pilot
    dec h                       ; stays at 255
    jr .pilot
.short:
    ld a, h
    cp 64                       ; enough pilot before it?
    jr c, .pilot_start
    ld b, 0                     ; the second half of the sync
    call edge
    jr nc, .pilot_start
    ; bytes: the flag, then DE bytes, then the checksum
    call read_byte
    ret nc
    ld a, l
    ld hl, want_flag
    cp (hl)
    jr nz, .bad
    xor a
    ld (sum), a
.bytes:
    call read_byte
    ret nc
    ld (ix+0), l
    inc ix
    ld a, (sum)
    xor l
    ld (sum), a
    dec de
    ld a, d
    or e
    jr nz, .bytes
    call read_byte              ; the checksum
    ret nc
    ld a, (sum)
    cp l
    jr nz, .bad
    scf
    ret
.bad:
    and a
    ret

; Reads a byte into L (8 bits, most significant first). Carry set if all went well.
read_byte:
    ld l, 1                     ; a marker bit: the byte is done when it falls out
.bit:
    ld b, 0
    call edge
    ret nc
    call edge                   ; B counts both halves of the bit
    ret nc
    ld a, b
    cp 21                       ; 0: about 13 counts, 1: about 29
    ccf                         ; carry = 1 for a long (one) bit
    rl l
    jr nc, .bit
    scf
    ret

; Waits for the EAR level to change, counting in B (52 T a count). Carry clear if B ran out.
edge:
    ld a, 0x7F
    in a, (0xFE)
    and 0x40
    cp c
    jr nz, .got
    inc b
    jr nz, edge
    and a
    ret
.got:
    ld c, a
    scf
    ret

want_flag: db 0
sum: db 0
end:
    savebin "turbo.bin", start, end - start
