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

#include "hardware/regs/addressmap.h"
#include "hardware/regs/pio.h"
#include "nmi.h"

.global piohandler

.extern rom_data
.extern rom_state

.section .time_critical.piohandler

.type piohandler,%function

// use a bunch of aliases for registers to back it
// a bit easier to remember what registers are being
// used for what.
r_rom_addr .req r4
r_pio_base .req r6
r_rom_state .req r5
r_nmi_rom_entry .req r7
r_nmi_exit .req r8
r_nmi_rom_exit .req r9
r_writable_from .req r10
r_rom_data .req r11
r_nmi_data .req r12
// lr holds the shifted address of the tape-in fetch (TAPE_IN_ENTRY as an M1 read, as
// r_nmi_rom_entry is), or 1 (never matches) when no tape is in. It is loaded from
// rom_state.tape_entry at the start and each time the NMI ROM exits. piohandler never
// returns and calls nothing, so lr is free.

// Tape in (the Spectrum's own ROM loads, the Pico gives it the blocks): while the internal ROM
// is serving and a tape is in, an M1 fetch at 0x0562 (in LD-BYTES, after its first 12 bytes)
// makes the Pico serve rom_data, which holds the hook there (it starts with a NOP, served
// as the NMI's first byte is, from a constant). It hands back to the internal ROM when the
// hook's RET at TAPE_IN_EXIT is read. In the 128K editor ROM 0x0556-0x05A0 is text, so it is
// never fetched as an instruction there. pico/picoface.cxx has the hook (TAPE_IN_HOOK).
#define TAPE_IN_ENTRY 0x0562
#define TAPE_IN_EXIT 0x059A
#define TAPE_MBOX 0x3FF8

// rom_state: +0 flags, +4 nmi_active, +8 writableStartAddress, +12 flags_on_nmi_exit,
// +16 exit_after_nmi (r_nmi_exit after the NMI ROM exits: the hook's exit while it runs,
// else 1), +20 tape_entry (lr), +24 tape_exit (the watched hook's last byte, shifted)
// Two hooks can be watched, one at a time: loading (playing a tape put in with T: LD-BYTES at
// 0x0562, which RETs at 0x059A) or saving (recording, W on the tape position screen: SA-BYTES
// at 0x04C2, whose last byte 0x04E2 ends its JP to SA/LD-RET at 0x053F in the Spectrum's ROM).
// In the 128K editor ROM 0x04B0-0x05A0 is all text, never fetched as an instruction.


piohandler:

    ldr r1, =((0 << 14 | 0x66) << 17)
    mov r_nmi_rom_entry, r1               // r_nmi_rom_entry is shifted nmi entry address, this is M1 read at 0x66

    ldr r1, =((0 << 14 | EXITNMI) << 18)
    mov r_nmi_rom_exit, r1
                             
    
    ldr r_rom_state, =rom_state
    ldr r_pio_base, =PIO0_BASE
    mov r0, #1 
    mov r_nmi_exit, r0              // r_nmi_exit is the NMI exit address shift, 1 is an invalid value
    lsl r0, r0, #15
    mov r_writable_from, r0             // r_writable_from is writable address start and is initialised to 
                            // something outside the address range we handle.
    ldr r_rom_addr, =rom_data
    mov r_rom_data, r_rom_addr
    ldr r0, [r_rom_state, #20]
    mov lr, r0                      // the tape-in fetch, or 1

    ldr r0, =(rom_data + 16384)
    mov r_nmi_data, r0

main_loop:
    ldr r1, =0x3FFF
check_fifo_empty:
    // The critical is the time is takes from when the pio sm puts an address
    // on the RX FIFO to time in which we put something on the TX FIFO of the
    // send data sm. We basically need to do this in about 18 cycles.
    // The code tries to avoid loads and stores to main memory because
    // these take two cycles. Accessing the pio state machines is via the SIO
    // so these ldr and str instructions take one cycle.

    // The next 4 instructions are basically a pio_sm_get_blocking(pio0, 0)
    // And this pulls 32 bits from the address/data fetcher pio state machine
    // This ends up with r0 holding
    //
    //  bits 0-13 : lower 14 bits of the spectrum address
    //  bit 15: Read flag = 1 for read this is effectively the /WR line
    //  bit 16: /M1 flag = 0 for a M1 fetch 1 otherwise
    //  bits 17-24: the value on the data lines D0-D7 (useful only for writes)
    //  bits 25-31: random (this is stuff on the other gpio pins that we don't care about)

    ldr r0, [r_pio_base, #PIO_FSTAT_OFFSET]
    lsr r0, r0, #(PIO_FSTAT_RXEMPTY_LSB + 1)
    bcs check_fifo_empty
    ldr r0, [r_pio_base, #PIO_RXF0_OFFSET]   // r0 now holds data and address

    // Point A: Cycle count starts here

    and r1, r0, r1                   // r1 now holds just address
    lsl r2, r0, #17                  // carry flag is set for a read, and clear for a write
    bcc write_op
    ldr r3, [ r_rom_state, #0]       // r3 holds rom_state.flags
    lsr r3, r3, #1                   // CC is set if rom enable, zero flag is NMI check is enabled
    bcc read_rom_disabled
    beq send_data
    cmp r2, r_nmi_rom_entry
    beq activate_nmi
send_data:
    ldrb r0, [r_rom_addr, r1]
    // This is effectively a pio_sm_put(pio, 1, ) call
    str r0, [r_pio_base, #(PIO_TXF1_OFFSET) ]       
    // 12/13 cycles to complete this from Point A depending on which branch followed.

    // Check if have reached the exit point of the nmi routine
    // allow nmi to exit with either a M1 read or normal read of the exit address, hence the
    // shift
    lsl r2, r2, #1
    cmp r2, r_nmi_exit 
    beq exit_nmi

    
    // Just send anything non-zero to the putdata pio to indicate that we wish to hold
    // the /CSROM line and continue serving ROM data from the pico
    str r_rom_addr, [r_pio_base, #(PIO_TXF1_OFFSET) ]
    b main_loop

exit_nmi:
    cmp r_nmi_exit, r_nmi_rom_exit // the hook's RET (tape in), not the NMI ROM's exit?
    bne exit_tape
    // leaving nmi rom now
    
    ldr r0, [r_rom_state, #12]     // read rom_state.flags_on_nmi_exit
    // rom_state is zero to return to internal rom or 1 to serve up a custom ROM
    // Can just pass this to the putdata pio because 0 means set the /CSROM low to 
    // enable the internal rom, anything else means hold the /CSROM line high.
    str r0, [r_pio_base, #(PIO_TXF1_OFFSET) ] 
    str r0, [r_rom_state, #0]      // restore the rom_state flags
    ldr r0, [r_rom_state, #8]      // read writableStartAddress in case we are now going to serve up a writable rom
    mov r_writable_from, r0        // and put into r_writable_from for later checks 
    ldr r0, [r_rom_state, #16]     // r_nmi_exit: the tape-in hook's exit if the NMI came while it ran (to load
                                   // a block), else 1, an invalid shifted address: (addr << 18) never equals 1
    mov r_nmi_exit, r0
    ldr r0, [r_rom_state, #20]     // a tape may have been put in or taken out
    mov lr, r0
    mov r_rom_addr, r_rom_data     // Restore the rom address to the normal rom, regardless of whether the rom is enabled or not
                                   
    mov r0, #0                     // And set a variable to say we aren't serving the nmi rom anymore
    str r0, [r_rom_state, #4]    
    b main_loop

read_rom_disabled:
    beq tape_check         // zero flag is set if nmi check is not required
    cmp r2, r_nmi_rom_entry
    beq activate_nmi
tape_check:
    cmp r2, lr             // the tape-in fetch at 0x0562? (lr is 1 when no tape is in)
    bne main_loop
    // 14 cycles from point A to the NOP sent (activate_nmi: 14, one more than before)

activate_tape:
    mov r0, #0                                  // the hook starts with a NOP, served from a constant
    str r0, [r_pio_base, #(PIO_TXF1_OFFSET) ]   // as the NMI's first byte is
    mov r0, #1
    str r0, [r_pio_base, #(PIO_TXF1_OFFSET) ]   // hold /CSROM: the Pico serves rom_data from now
    ldr r3, [r_rom_state, #0]
    orr r3, r0
    str r3, [r_rom_state, #0]            // rom enabled (an NMI asked for meanwhile stays asked for)
    str r0, [r_rom_state, #12]           // an NMI that loads a block comes back to the hook
    ldr r0, =TAPE_MBOX
    mov r_writable_from, r0              // the hook writes its mailbox
    str r0, [r_rom_state, #8]
    ldr r0, [r_rom_state, #24]           // the hook's last byte (shifted): its RET, or its JP to the ROM
    mov r_nmi_exit, r0                   // hand back when it is read
    str r0, [r_rom_state, #16]
    mov r_rom_addr, r_rom_data
    b main_loop

exit_tape:
    mov r0, #0
    str r0, [r_pio_base, #(PIO_TXF1_OFFSET) ]   // release /CSROM: the internal ROM again
    ldr r3, [r_rom_state, #0]
    mov r0, #1
    bic r3, r0
    str r3, [r_rom_state, #0]            // rom disabled
    mov r_nmi_exit, r0                   // no exit to look for
    str r0, [r_rom_state, #16]
    mov r0, #0
    str r0, [r_rom_state, #12]           // an NMI from now returns to the internal ROM
    mov r0, #1
    lsl r0, r0, #15
    mov r_writable_from, r0              // nothing writable
    str r0, [r_rom_state, #8]
    b main_loop

activate_nmi:
    mov r0, #0                                  // always serve a NOP as first NMI instruction, this saves a read instruction
                                                // but we need to ensure that nmi.asm also has a nop at location 0x66.
    str r0, [r_pio_base, #(PIO_TXF1_OFFSET) ]   // Send data to the PIO like an pio_sm_put() and this sends it off to the speccy.
                                                // 13 cycles to complete from point A.
    mov r_writable_from, r0                     // NMI always allows writes, so set allowable writes from 0 onwards
    mov r0, #1
    str r0, [r_pio_base, #(PIO_TXF1_OFFSET) ] 
    str r0, [r_rom_state, #4]            // nmi routine is now active
    str r0, [r_rom_state, #0]            // rom is enabled, because it is the nmi rom
    mov r_nmi_exit, r_nmi_rom_exit           // and set r_nmi_exit to actual exit address from the nmi rom
    mov r_rom_addr, r_nmi_data
    b main_loop
write_op:
    // r0 is data and address, r1 is just address
    // writes are allowed from the address in r_writable_from onwards
    cmp r1, r_writable_from
    blo main_loop
    lsr r0, r0, #16
    strb r0, [r_rom_addr, r1]
    b main_loop