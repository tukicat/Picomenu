# .z80 to .tap (T on the Batch convert screen)

The converter is `pico/z80tap.h`; the loader it puts on each tape is `loader.asm` here.

    python3 build.py              # assembles loader.asm (sjasmplus) and writes pico/z80tap_loader.h
    g++ -std=c++17 -O1 -I../../pico z80tap_host.cpp -o z80tap_host
    ./z80tap_host game.z80 game.tap [name]

## The tape
1. `10 CLEAR VAL "R-1": LOAD ""CODE : RANDOMIZE USR VAL "R"`
2. The loader: 1 KB at R (0xBC00, or 0x9C00, 0x7C00, 0x6C00 if the game's stack is there).
3. The game's memory. The screen comes first, so it shows while the rest loads. A 128K game's banks follow, each paged in at 0xC000. Runs of 512 or more of the same byte are left off the tape and filled in by the loader.
4. The game's own 1 KB at R.

The loader keeps interrupts off throughout. It calls the ROM's LD-BYTES at 0x0562, the point after it pushes SA/LD-RET, so the ROM returns straight to the loader, never turns interrupts on, and its interrupt routine never writes over the game's system variables.

At the end the loader:
- sets the sound chip's registers (128K);
- copies a 90-byte stub to just below the game's stack pointer, memory the game isn't using;
- runs the stub, which loads the last block, sets every register (I, R, IM, IFF, the alternate set, IX, IY, the border, port 0x7FFD) and jumps to the game's PC.

## Proof
    python3 roundtrip.py 48.rom 128.rom [work folder]

It makes test snapshots with random memory, long runs, random registers and a `DI`/`JR $` loop, so nothing changes once loaded. It covers several stack positions: at 0, low, where the loader would go, and in a paged 128K bank. One case has interrupts on.

Each snapshot becomes a .tap, which `../tzxconv/tzxconv_host` then loads with `TZXNOFLASH=1`, so every edge goes through the ROM's own loader. The snapshot that comes back must have the same memory, registers, paging and sound chip.

Real games converted the same way (Mikie, Green Beret 48K and 128K, Athena 128K) load and carry on where they were.
