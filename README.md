# PicoMenu

PicoMenu is a fork of [picoFace](https://github.com/) by Brian Apps: the hardware and the original firmware are his, and the menu and file features here are added on top. It keeps the GPL v3 licence (see `LICENSE` and `THIRD-PARTY-NOTICES.txt`). The picoFace is an inexpensive device for the ZX Spectrum that uses a Raspberry Pi Pico to provide Multiface style functionality.

Picomenu has been built using Brian Apps original code, an AI (Claude) has then developed on this with my input. My main input to this project was to decide on functionality, layout, feature set and testing.

Picomenu has been developed for and tested on a Spectrum +2 (Grey). It should work fine on a standard 48k or 128k model, although I have not been able to test. It will not work on +2A/+3, +2B, and +3B.

The ethos of the Picomenu is to make as much functionality as possible available on the Spectrum without a PC. It may be quicker or more convenient to use a PC for some things (creating .z80 snapshots from .tzx or .tap files for example), but convenience is not the aim. 

### Setting up the SD Card

On first start of the Picomenu you can choose D - Set up the SD card (this needs to be in exFAT or FAT32 format), this will populate the card with the folders it requires for some of the functionality to work. For instance it will create a /Favourites folder which you can use to add favourite games to quickly. It also creates a sample "Instructions.txt" which you can use to add the game information showed on the Game View Screen. 

You can also create the folders yourself, they need to be on the root of the SD card and named: -
/Favourites 
/Save Games
/Screen Grabs
/Scraper
/Game Notes
/Poke List

"instruction-example.txt" in the /docs section of the repo shows the headings you should include in the "instructions.txt" file. The "instructions.txt" file should be placed in the root of the SD card or the /Scraper folder. 

If you want to create the "instruction.txt" file yourself, here are the headings you need to include: 

Game Name (delete Game Name and replace with you game name)
Software house: (delete this bracked text and write software house here)
Year: (delete this bracked text and write year here)
Controls
(delete this bracked text and write controls here)
Instructions
(delete this bracked text and write instructions here)

In order to use .tap functionality and the .tap/.tzx conversion to .z80 feature you need to place 48.rom and 128.rom (or 128-0.rom and 128-0.rom) on the root of the flash or SD card. I can't provide these files so you will need to provide your own. 

For the T-Test ROM functionality you need to provide testrom.rom and/or DiagROM.rom and place them on the root of the flash. 

### Updating to a Newer Firmware

You can update Picomenu by placeing the .uf2 file from the releases section on the root of the Pico flash, Then run U-Update from the NMI menu. it will run through copying and writing and then ask you to power the spectrum off and on. You can verify the update has worked by using the U-Update screen (which shows the current version), or the H-Help section. Press D in the help section to take you to a diagnositic page that will show the version number.

Only put one .uf2 in the root of SD card or internal flash. I have not yet tried what will happen if two are present. 

If for any reason the update bricks the Picoface, switch off the spectrum and unplug the Picoface from it. Plug a USB cable into a PC, and before you plug the other end into the Pico hold down the BOOTSEL button. Once you have plugged the cable into the Pico release the BOOTSEL button. The pico will appear on your PC as drive named: RPI-RP2. Copy the .uf2 file onto that drive. The Pico will reset and you should now be able to plug the Picoface back into the Sspectrum, switch it on and the Picoface should work.

### User Guide

The /docs folder contains a help.txt which is also in the help section of the NMI Picomenu. 


![The NMI menu](images/1-nmi-menu.png)

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

# The NMI button

The NMI button (top right of the Picoface) has three functions. 

1.) a short press will take you into the NMI menu
2.) a three second press will reset the spectrum
3.) a double press will take a quick screen grab and place it in the /Screen Grabs folderardware.

![Browse Games](images/2-browse-games.png)

![View Screem](images/3-view-screen.png)

## Related repositories

* The KiCad circuit design is held in https://github.com/brianapps/picoface_kicad


## Building the pico firmware

The firmware build relies on the following being installed and on the path:

- [sjasmplus](https://github.com/z00m128/sjasmplus)
- Python 3.x
- Raspberry Pi Pico SDK 
- VSCode and VSCode Pico plugin.


## License

Picomenu is free software: you can redistribute it and/or modify it under the terms of
the GNU General Public License as published by the Free Software Foundation, either
version 3 of the License, or (at your option) any later version.

Picomenu is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; 
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

