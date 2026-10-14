# PicoMenu

PicoMenu is a fork of [picoFace](https://github.com/) by Brian Apps: the hardware and the original firmware are his, and the menu and file features here are added on top. It keeps the GPL v3 licence (see `LICENSE` and `THIRD-PARTY-NOTICES.txt`). The picoFace is an inexpensive device for the ZX Spectrum that uses a Raspberry Pi Pico to provide Multiface style functionality.

Picomenu can be used to:
* Replace the internal ROM with another image, optionally making it writable.
* Load and save snapshots, 48K and 128K, as `.sna`, `.z80` and `.szx`; load `.tap` and `.tzx` tapes.
* Controls and instructions for each game from a text file `Instructions.txt` in the folder `Scraper` on the SD card (or in the root, if there is none there): shown beside a small copy of the game's screen (V in Browse Games, where D deletes a screen grab) and from the menu with I (Game Instructions) after the game has loaded (see `Docs/Instructions-example.txt`).
* Quick save keeps earlier saves: Q asks to overwrite, or N saves as "<game> Save Game 2", 3 ...
* Each game's information can also show its software house and year, and your own notes on the game (M while the information shows, E to edit them; kept in a `Game Notes` folder that you make in the root of the SD card).
* A list of the last 10 games played (E on the menu), kept after power off; 1 to 9 and 0 load one, V then a number shows its picture and instructions.
* G on the menu saves the running game's screen (`.scr`) in a `Screen Grabs` folder on the SD card; V in Browse Games cycles through a game's own screen and its grabs.
* The SD card shows up as a USB drive on a PC when the Pico is plugged in with the Spectrum off (copy files without taking the card out; eject before unplugging).
* Browse the SD card and the internal flash (both with folders, search, move files and folders, copy files and folders (a folder copy stays on one drive; the SD card and the flash are not mixed), copy files between the drives, rename files and folders, erase files and folders): preview a game's screen, show file sizes and which machine (48 or 128) each snapshot needs.
* Quick save (`<game> Save Game`) and reload the last game with one key.
* Apply pokes to loaded games, typed in or from a `.pok` file.
* Reset into 48K or 128K from the menu.
* Update PicoMenu's own firmware from a `.uf2` in the root of the SD card (U on the menu; if it fails, BOOTSEL still works).
* on-screen help


### The NMI menu

| Key | Does |
|---|---|
| S | Save snapshot (type auto, .sna, .z80, .szx, same) |
| Q | Quick save as `<game> Save Game` in the `Save Games` folder of the SD card |
| B | Browse Games (O for info: Type, Size and Mode columns) |
| R | Reload last game (shows its name, scrolling a long one; remembered after power off) |
| C | Change ROM |
| P | Poke (L for the game's `.pok` list) |
| 4 / 1 | Reset into 48K / 128K |
| D | Set up SD card: makes `Scraper` (with an example `Instructions.txt`), `Game Notes`, `Save Games`, `Screen Grabs`, `Favourites`, `Poke List` on a new card; replaces nothing |
| U | Update PicoMenu from a `.uf2` file in the root of the SD card (listed nowhere else) |
| T | Test ROM: choose zx-diagnostics (`testrom.rom`) or DiagROM (`DiagROM.rom`), kept on the flash, and reset into it |
| H | Help (D for diagnostics) |
| X | Exit |

The NMI button: a short press opens the menu; a **double press** saves the screen as a screen grab (green border: saved, red: not); holding it for one second resets into the Spectrum's own ROM. See `CHANGELOG.md` for what changed and `Docs/Instructions.txt` for the details and test notes. Features have been tested in simulation; check anything important on your own hardware.

For a demonstration of the device in action see this video:


## Related repositories

* The KiCad circuit design is held in https://github.com/brianapps/picoface_kicad
* The Android Studio project for the phone loading application is held in https://github.com/brianapps/picoface_phone


## Building the pico firmware

The firmware build relies on the following being installed and on the path:

- [sjasmplus](https://github.com/z00m128/sjasmplus)
- Python 3.x
- Raspberry Pi Pico SDK 
- VSCode and VSCode Pico plugin.


## License

picoFace is free software: you can redistribute it and/or modify it under the terms of
the GNU General Public License as published by the Free Software Foundation, either
version 3 of the License, or (at your option) any later version.

picoFace is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; 
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details.

A copy of the license is provided in the [LICENSE](./LICENSE) file.

Third-party parts (FatFs, littlefs, LZ4, the Apache 2.0 SD driver, the Pico SDK, TinyUSB and the C runtime) keep their own, GPL-compatible licences. They are listed with their full texts in [THIRD-PARTY-NOTICES.txt](./THIRD-PARTY-NOTICES.txt); the Apache 2.0 text is in [pico/FatFs_SPI/LICENSE-Apache-2.0.txt](./pico/FatFs_SPI/LICENSE-Apache-2.0.txt).


## Acknowledgments

The picoface firmware includes the following libraries:

* [pico-littlefs](https://github.com/lurk101/pico-littlefs) by lurk101
* [no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico) by Carl J Kugler III
* [lz4](https://github.com/lz4/lz4) by Yann Collet.
* [Carton](https://damieng.com/typography/zx-origins/carton/) font by Damien Guard

