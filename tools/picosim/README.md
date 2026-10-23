# picosim: the firmware and its NMI ROM on a PC

`picoface.cxx` is built for a PC (with `PICOFACE_HOST` and small stand-ins for the Pico SDK in `stubs/`). The real NMI ROM (`nmirom.asm`, assembled as for the firmware) runs on an emulated Spectrum, a 48K or a 128K. Each time the ROM puts a request in its mailbox, the real `processNmiRequest()` answers it, as the Pico does.
- **SD card:** a RAM disk under the firmware's own FatFs.
- **Flash:** a RAM littlefs. `pico_hal_host.c` is a copy of `pico_hal.c` with RAM in place of the flash, and handles as small numbers.

It tests the menus as they really run, without a Picoface: keys in, screens out, files on the card.

    ./build.sh [objdir]                 # needs gcc, g++, sjasmplus, python3
    ./sim [-128 128-0.rom 128-1.rom] 48.rom script.txt
    tests/run.sh ROMDIR TAPEDIR [out]   # the converter's scripts
    SIM_SANITIZE=1 ./build.sh           # stop at a misaligned access, as the Pico does (see below)
    SIM_CARD=exfat32k ./sim ...         # the card as exFAT with 32 KB clusters (else FAT32)

The script commands are listed at the top of `sim.cpp`: sd, flash, boot, nmi, key, wait, text, shot, expect, expectnot, hide, slice, clock, ls, get. `text` and `expect` read the screen with the menu font.

## What it doesn't do
- The bus timing, `piohandler.S`, USB and the real flash.
- Speed: time on a PC means nothing for the Pico.
- Memory: the PC has plenty of RAM and stack, so a stack overflow on the Pico wouldn't show here. FatFs's heap is limited to `SIM_HEAP` bytes (default 4096), as the Pico's malloc stops when it runs out.
- Alignment: a PC allows a 16 or 32-bit access at any address, and the Pico's Cortex-M0+ stops with a HardFault. Build with `SIM_SANITIZE=1` to stop at such an access (it also puts the memory lent to FatFs at an odd address, as it was in a test build when copying a folder stopped the Pico). Run the scripts that way after changing memory that is lent or cast.
