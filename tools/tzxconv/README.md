# Tape converter: PC tools

The converter itself is `pico/tzxconv.h` (with the Z80 emulator in `pico/z80/`). The same code runs here on a PC, so it can be tested before it goes into the firmware.

## Convert a tape on a PC
    ./build.sh                          # makes ./tzxconv_host (needs gcc, g++)
    ./tzxconv_host 48.rom game.tzx game.z80
    TZXTRACE=1 ./tzxconv_host 48.rom game.tzx game.z80     # a line every emulated second

The tool prints:
- the result: loaded / R Tape loading error / stopped / BASIC error / damaged;
- how long the tape played in emulated time, and how long the PC took;
- any warning.

The exit code is 0 when the tape loaded, 3 when it was saved with a warning, and 1 when it did not load.

It also prints the instructions run, the loop passes skipped and the blocks flash-loaded. To compare runs:

    TZXNOACCEL=1 ./tzxconv_host ...     # no loader acceleration (every pass of every loop is run)
    TZXNOFLASH=1 ./tzxconv_host ...     # no flash loading (the ROM's blocks are played edge by edge)

`TZXGRAB=file.scr` saves the loading screen when the converter sees it loaded (it prints "loading screen at ... s").

The acceleration is exact: with TZXNOFLASH, a snapshot made with and without TZXNOACCEL must be the same, byte for byte. Flash loading is not: it leaves out the time the ROM's blocks take, so the snapshot is taken at another moment of the same game.

**A real tape to try:** convert it here, load the .z80 in Fuse (or on the Picoface) and see whether the game runs. Also load the .tzx in Fuse to compare.

## Tests
    tests/runtests.sh 48.rom [work folder]

This needs `sjasmplus` and `python3`. It assembles `tests/game.asm` and `tests/turbo.asm`, makes 15 tapes with `tests/maketapes.py`, converts each one, and checks each snapshot with `tests/checkz80.py`.

## Speed on the Pico's processor
`bench/` builds the converter for a Cortex-M0+ with one tape and ROM linked in (`bench/build.sh tape.tzx 48.rom [z80 -O] [main -O]`; `BENCHFLAGS="-DBENCH_NOACCEL -DBENCH_NOFLASH"` turns the speed-ups off). `bench-run.ts` runs it in the CPU core of rp2040js (github.com/wokwi/rp2040js) and counts clock cycles. It also models the RP2040's 16 KB XIP flash cache. Copy it into an rp2040js checkout and run it from there:

    node --experimental-transform-types --import ./reg.mjs bench-run.ts bench.bin 50 0x20000000
    PROF=1 MAXCYC=3000000000 node ... bench.bin 50 0x20000000 syms.txt   # where the time goes, by function
                                                                       # (syms.txt: arm-none-eabi-nm -S -C bench.elf)

(`reg.mjs` and `hook.mjs`, also in `bench/`, let Node load rp2040js's TypeScript: they map `./x.js` imports to `./x.ts`. Copy them too.) Results are in `Docs/Tape-converter-notes.md`.

## ../stackdepth.py
This gives the worst-case stack from a build made with `-fcallgraph-info=su`:

    python3 stackdepth.py builddir main
    CHILDREN=2 python3 stackdepth.py builddir main      # also each branch from main
