; This file is part of PicoMenu (picoFace), GNU GPL v3 or later.
;
; The loader of the tapes Batch convert makes from .z80 snapshots (T on its folder screen),
; for a real Spectrum loading at the normal speed. pico/z80tap.h puts it on the tape after a
; BASIC line (10 CLEAR ...: LOAD ""CODE: RANDOMIZE USR ...) and fills in its table and values.
;
; It runs with interrupts off throughout. Each block is loaded by the ROM's LD-BYTES entered
; at 0x0562, after the place where it pushes SA/LD-RET, so it comes straight back here with
; the carry flag and never turns interrupts on (the ROM's interrupt routine would write over
; the system variables, which by then are the game's own memory). It uses no system variable.
;
; The loader (LC) is LC_SIZE bytes at R, which the Pico chooses so that nothing else needs it
; until the end. The table says what to do: load a block, fill a run of one byte (cheaper than
; loading it), or page a 128K bank in at 0xC000. Then the sound chip's registers (128K), the
; paging, and the stub is copied below the game's stack pointer (memory the game is not using:
; the stack grows down) and run there: it loads the last block, R's own bytes, then sets every
; register and jumps into the game.
;
; Assembled twice (by tools/z80tap/build.sh) to find the addresses to relocate: LC at ORG 0 or
; 0x1000, the stub at DISP 0 or 0x2000. pico/z80tap_loader.h has the result.

    DEVICE NOSLOT64K

LC_SIZE equ 1024
LD_BYTES_AFTER_PUSH equ 0x0562

T_END equ 0
T_LOAD equ 1        ; WORD address, WORD length
T_FILL equ 2        ; WORD address, WORD length, BYTE value
T_PAGE equ 3        ; BYTE value for port 0x7FFD

    IFNDEF LC_ORG
LC_ORG equ 0
    ENDIF
    IFNDEF STUB_DISP
STUB_DISP equ 0
    ENDIF

    ORG LC_ORG
lc_start:
    di
    ld sp, lc_start + LC_SIZE
    ld hl, table
next:
    ld a, (hl)
    inc hl
    or a
    jr z, done
    dec a
    jr z, doload
    dec a
    jr z, dofill
    ld a, (hl)              ; T_PAGE
    inc hl
    ld bc, 0x7FFD
    out (c), a
    jr next
doload:
    ld e, (hl)
    inc hl
    ld d, (hl)
    inc hl
    push de
    pop ix
    ld e, (hl)
    inc hl
    ld d, (hl)
    inc hl
    push hl
    call ldb
    pop hl
    jr c, next
fail:
    ld a, 2                 ; red: a loading error (rewind the tape and start again)
    out (0xFE), a
    jr fail
dofill:
    ld e, (hl)
    inc hl
    ld d, (hl)
    inc hl
    ld c, (hl)
    inc hl
    ld b, (hl)
    inc hl
    ld a, (hl)
    inc hl
    push hl
    ld h, d
    ld l, e
    ld (hl), a
    dec bc
    ld a, b
    or c
    jr z, 1F
    inc de
    ldir
1:  pop hl
    jr next

; Loads DE bytes at IX (a block with the flag byte 0xFF): carry set if it loaded
ldb:
    ld a, 0xFF
    scf
    inc d
    ex af, af'
    dec d
    ld a, 0x0F
    out (0xFE), a
    jp LD_BYTES_AFTER_PUSH  ; (its RET comes back to our caller)

done:
    ld a, (ayflag)
    or a
    jr z, noay
    ld hl, ayregs
    ld e, 0
ayloop:
    ld bc, 0xFFFD
    out (c), e
    ld a, (hl)
    ld b, 0xBF
    out (c), a
    inc hl
    inc e
    ld a, e
    cp 16
    jr nz, ayloop
    ld a, (aysel)
    ld b, 0xFF
    out (c), a
noay:
    ld a, (page_stub)       ; the game's bank at 0xC000, but the 48K ROM and not locked yet
    ld bc, 0x7FFD
    out (c), a
    ld hl, stub_image
    ld de, (stub_at)
    ld bc, STUB_SIZE
    ldir
    ld hl, (stub_at)
    jp (hl)

ayflag: db 0
aysel: db 0
ayregs: ds 16
page_stub: db 0x10
stub_at: dw 0

; ---- the stub: copied to stub_at (just below the game's SP) and run there ----
stub_image:
    DISP STUB_DISP
stub:
    ld sp, stub             ; its stack is below it (also the game's free stack)
    ld ix, lc_start         ; the loader's own place: the last block
    ld de, LC_SIZE
    call ldb
    jr nc, sfail
s_page equ $ + 1
    ld a, 0x30              ; port 0x7FFD as the game had it (a 48K one: locked to 48K)
    ld bc, 0x7FFD
    out (c), a
s_i equ $ + 1
    ld a, 0
    ld i, a
s_im equ $ + 1
    im 1                    ; (ED 46 / 56 / 5E)
    ld sp, s_af2
    pop af
    ex af, af'
    exx
s_bc2 equ $ + 1
    ld bc, 0
s_de2 equ $ + 1
    ld de, 0
s_hl2 equ $ + 1
    ld hl, 0
    exx
s_ix equ $ + 2
    ld ix, 0
s_iy equ $ + 2
    ld iy, 0
s_border equ $ + 1
    ld a, 0
    out (0xFE), a
s_r equ $ + 1
    ld a, 0
    ld r, a
s_bc equ $ + 1
    ld bc, 0
s_de equ $ + 1
    ld de, 0
s_hl equ $ + 1
    ld hl, 0
    ld sp, s_af
    pop af
s_sp equ $ + 1
    ld sp, 0
s_ei:
    nop                     ; (EI if interrupts were on)
s_pc equ $ + 1
    jp 0
sfail:
    ld a, 2
    out (0xFE), a
    jr sfail
s_af2: dw 0
s_af: dw 0
stub_end:
    ENT
STUB_SIZE equ stub_end - stub

table:                      ; T_LOAD / T_FILL / T_PAGE entries, then T_END
lc_end:

    SAVEBIN "loader.bin", lc_start, lc_end - lc_start
