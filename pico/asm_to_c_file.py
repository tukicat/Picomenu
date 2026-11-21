# Copyright (C) 2024 Brian Apps
#
# This file is part of picoFace.
#
# picoFace is free software: you can redistribute it and/or modify it under the terms of
# the GNU General Public License as published by the Free Software Foundation, either
# version 3 of the License, or (at your option) any later version.
#
# picoFace is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; 
# without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with picoFace. If 
# not, see <https://www.gnu.org/licenses/>.


import pathlib

# PicoMenu: the ROM is kept compressed in the firmware (blob_pack.py, about 5 KB smaller), and
# nmi_rom_unpack() puts it in the ROM space each time the menu opens.
import sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from blob_pack import compress, decompress

romdata = pathlib.Path('nmirom.bin').read_bytes()

symbols = {}

with pathlib.Path('nmirom.sym').open("r") as fp:
    for l in fp.readlines():

        toks = l.split(': EQU')
        if len(toks) == 2:
            symbols[toks[0].strip()] = int(toks[1].strip(), 16)


exitnmi = symbols["exitnmi"]
startup_param2 = symbols["start_up_param2"]
startup_param1 = symbols["start_up_param1"]
startup_command = symbols["start_up_action"]


with open("nmi.c", "w") as fp:
    fp.write("#include \"pico/stdlib.h\"\n\n")

    packed = compress(romdata)
    assert decompress(packed, len(romdata)) == romdata
    fp.write("static const uint8_t NMI_ROM_Z[] = {")
    for i in range(0, len(packed)):
        if i % 16 == 0:
            fp.write("\n    ")
        fp.write(f"0x{packed[i]:02x}, ")
    fp.write("\n};\n\n")
    fp.write(f"const uint32_t NMI_ROM_SIZE = {len(romdata)};\n\n")
    fp.write("""// Unpacks blob_pack.py's packing: LZ77 with Elias gamma codes. Bits come from bytes of their
// own (the top bit first) as they are needed, among the literal and offset bytes. Until n bytes
// are out: bit 0, a literal byte; bit 1, a match: gamma(length - 1), gamma(((offset - 1) >> 8)
// + 1), then the low byte of offset - 1. Used for the NMI ROM, its routines, the Help text and
// the set up texts.
void blob_unpack(const uint8_t* c, uint8_t* to, uint32_t n) {
    uint32_t o = 0, bits = 0, left = 0;
#define BLOB_BIT() (left == 0 ? (bits = *c++, left = 7, (bits >> 7) & 1) : (--left, (bits >> left) & 1))
    while (o < n) {
        if (!BLOB_BIT()) {
            to[o++] = *c++;
            continue;
        }
        uint32_t v[2];
        for (int g = 0; g < 2; g++) {
            uint32_t k = 0;
            while (!BLOB_BIT()) k++;
            uint32_t x = 1;
            while (k-- > 0) x = (x << 1) | BLOB_BIT();
            v[g] = x;
        }
        uint32_t len = v[0] + 1;
        uint32_t off = (((v[1] - 1) << 8) | *c++) + 1;
        while (len-- > 0 && o < n) {
            to[o] = to[o - off];
            o++;
        }
    }
#undef BLOB_BIT
}

void nmi_rom_unpack(uint8_t* to) {
    blob_unpack(NMI_ROM_Z, to, NMI_ROM_SIZE);
}
""")

    print(f"NMI ROM {len(romdata)} bytes, packed {len(packed)}")

    # the autosave's copy (cpsave in nmirom.asm), which the pico puts in the ROM for an autosave
    cpsave = pathlib.Path('cpsave.bin').read_bytes()
    assert len(cpsave) == symbols["CP_SIZE"]
    cplz = compress(cpsave)
    assert decompress(cplz, len(cpsave)) == cpsave
    fp.write("const uint8_t CP_SAVE_Z[] = {")
    fp.write(", ".join(f"0x{b:02x}" for b in cplz))
    fp.write("};\n\n")
    print(f"Autosave copy {len(cpsave)} bytes")

    # Texts the firmware writes to the SD card when it is set up (Picomenu.ini, the example
    # Instructions.txt), packed too: taken from the C strings in settings.h and sdsetup.h
    import re
    def c_string(path, name):
        src = pathlib.Path(__file__).resolve().parent.joinpath(path).read_text()
        m = re.search(r'static const char ' + name + r'\[\] =\s*((?:\s*"(?:[^"\\]|\\.)*"\s*)+);', src)
        body = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))
        return body.encode('latin1').decode('unicode_escape').encode('latin1')
    texts = [c_string('settings.h', 'DEFAULT_SETTINGS') + c_string('settings.h', 'ORDER_HEADING'),
             c_string('sdsetup.h', 'EXAMPLE')]
    allt = b''.join(texts)
    packed = compress(allt)
    assert decompress(packed, len(allt)) == allt
    fp.write("const uint8_t SETUP_TEXTS_Z[] = {")
    fp.write(", ".join(f"0x{b:02x}" for b in packed))
    fp.write("};\n\n")
    setup_sizes = [len(t) for t in texts]
    print(f"Set up texts {len(allt)} bytes, packed {len(packed)}")
    hk = pathlib.Path('hotkeys.bin').read_bytes()
    assert len(hk) == symbols["HOTKEYS_SIZE"]
    hklz = compress(hk)
    assert decompress(hklz, len(hk)) == hk
    fp.write("const uint8_t HOTKEYS_Z[] = {")
    fp.write(", ".join(f"0x{b:02x}" for b in hklz))
    fp.write("};\n\n")
    cpold = pathlib.Path('cpold.bin').read_bytes()
    assert len(cpold) == symbols["CP_OLD_SIZE"]
    fp.write("#if PICOMENU_CP_OLD\nconst uint8_t CP_OLD_BIN[] = {")
    fp.write(", ".join(f"0x{b:02x}" for b in cpold))
    fp.write("};\n#endif\n\n")

    print(f"exit op code: {romdata[exitnmi]}\n")
    if romdata[exitnmi] != 201:
        print("exit is not simple ret")
        exitnmi += 1
    

    # fp.write(f"const uint16_t STARTUP_COMMAND_OFFSET = 0x{startup_command:x};\n")
    # fp.write(f"const uint16_t STARTUP_PARAM1_OFFSET = 0x{startup_param1:x};\n")
    # fp.write(f"const uint16_t STARTUP_PARAM2_OFFSET = 0x{startup_param2:x};\n")


with open("nmi.h", "w") as fp:

    fp.write("#ifndef __ASSEMBLER__\n")
    fp.write("extern const uint32_t NMI_ROM_SIZE;\n")
    fp.write("#ifdef __cplusplus\nextern \"C\"\n#endif\nvoid nmi_rom_unpack(uint8_t* to);\n")
    fp.write("#ifdef __cplusplus\nextern \"C\"\n#endif\nvoid blob_unpack(const uint8_t* src, uint8_t* to, uint32_t n);\n")
    fp.write("#endif\n")


    for k, v in symbols.items():
        # ACTION_ are the request codes, SNAPLIST_ the layout of the
        # ACTION_SNAP_LIST results
        # Z80X_ the extra fields of a 128K snapshot's header
        if k.startswith("ACTION_") or k.startswith("SNAPLIST_") or k.startswith("Z80X_") or k.startswith("SAVE128_"):
            fp.write(f"#define {k} {v}\n")

    # the order the NMI rom asks for the banks of a 128K snapshot in
    order = ", ".join(str(romdata[symbols["page_order"] + i]) for i in range(8))
    fp.write(f"#define NMI_PAGE_ORDER {order}\n")



    fp.write(f"#define EXITNMI 0x{exitnmi:x}\n")
    # the autosave's copy (cpsave): where it goes, where the pico writes the copy of each block and
    # where that puts it
    fp.write("#ifndef __ASSEMBLER__\nextern const uint8_t CP_SAVE_Z[];\nextern const uint8_t CP_OLD_BIN[];\n#endif\n")
    # the set up texts (SETUP_TEXTS_Z): Picomenu.ini's defaults and order heading, then the example
    fp.write("#ifndef __ASSEMBLER__\nextern const uint8_t SETUP_TEXTS_Z[];\n#endif\n")
    fp.write(f"#define SETUP_INI_SIZE {setup_sizes[0]}\n#define SETUP_EXAMPLE_SIZE {setup_sizes[1]}\n")
    # the pico's own code (picoscreen's PICO_RUN, and the high score table at the menu's NMI)
    fp.write(f"#define PICO_CODE_AT 0x{symbols['PICO_CODE']:x}\n#define WAITPICO 0x{symbols['waitpico']:x}\n")
    fp.write(f"#define PICO_RUN 0x{symbols['PICO_RUN']:x}\n")
    fp.write(f"#define EXIT_NO_SCREEN 0x{symbols['nmientry.exitnoscreenrestore']:x}\n")
    fp.write(f"#define SPARE_AT 0x{symbols['SPARE_SPACE']:x}\n")
    fp.write(f"#define TROPHY_ITEM 0x{symbols['startmenu.trophyitem']:x}\n")
    fp.write(f"#define TROPHY_MAKER_ITEM 0x{symbols['startmenu.trophymakeritem']:x}\n")
    # hotkeys (nmirom.asm): the code the pico puts in for the button, and what the hotkeys open
    fp.write("#ifndef __ASSEMBLER__\nextern const uint8_t HOTKEYS_Z[];\n#endif\n")
    for k, sym in (("HOTKEYS_AT", "HOTKEYS_AT"), ("HOTKEYS_SIZE", "HOTKEYS_SIZE"), ("HK_HELD", "hotkey_held"),
                   ("HK_CALLMENU", "nmientry.callmenu"), ("HK_BROWSE", "startmenu.handle_load"),
                   ("HK_ZXDB", "startmenu.handle_zxdb"), ("HK_INFO", "startmenu.handle_info"),
                   ("HK_SAVE", "startmenu.handle_save"), ("HK_GRAB", "startmenu.handle_grab"),
                   ("HK_RESET128", "startmenu.handle_reset128"), ("HK_RESET48", "startmenu.handle_reset48"),
                   ("HK_TAPE", "startmenu.handle_tape"), ("HK_AUTOSAVE", "startmenu.handle_checkpoints"),
                   ("HK_POKE", "startmenu.handle_poke"), ("HK_TROPHIES", "startmenu.handle_trophies"),
                   ("TOOLS_FLAG", "startmenu.intools")):
        fp.write(f"#define {k} 0x{symbols[sym]:x}\n")
    for k in ("CP_AT", "CP_SIZE", "CP_OLD_SIZE", "CP_CODE", "CP_SP", "CP_DATA", "CP_DATA_SIZE", "CP_LIMIT", "CP_GROUP_CODE"):
        fp.write(f"#define {k} 0x{symbols[k]:x}\n")
    fp.write(f"#define STARTUP_COMMAND_OFFSET 0x{startup_command:x}\n")
    fp.write(f"#define STARTUP_PARAM1_OFFSET 0x{startup_param1:x}\n")
    fp.write(f"#define STARTUP_PARAM2_OFFSET 0x{startup_param2:x}\n") 
    fp.write(f"#define STARTUP_SDCARD_PRESENT 0x{symbols['start_up_sdcard_present']:x}\n") 
    fp.write(f"#define STARTUP_RELOAD_LABEL 0x{symbols['start_up_reload_label']:x}\n")
    # what Picomenu.ini sets as the menu opens
    fp.write(f"#define START_SHOWALL 0x{symbols['loadscreen.showall']:x}\n")
    fp.write(f"#define START_SHOWINFO 0x{symbols['loadscreen.showinfo']:x}\n")
    fp.write(f"#define START_DRIVE 0x{symbols['loadscreen.drive']:x}\n")
    fp.write(f"#define START_SAVETYPE 0x{symbols['savesnapshotscreen.savedefault']:x}\n")
    fp.write(f"#define START_THEME 0x{symbols['theme_light']:x}\n")
    fp.write(f"#define START_MENUSHOT 0x{symbols['menu_shot']:x}\n")
    fp.write(f"#define START_KEYREPEAT 0x{symbols['key_repeat']:x}\n")    # the pico's screen is one for typing: a held key repeats
    fp.write(f"#define NMI_CHARBANK 0x{symbols['charbank']:x}\n")
    # the game's sound chip volumes, kept while the menu is up, and where they go in a 128K save
    fp.write(f"#define NMI_AYVOLS 0x{symbols['ayvols']:x}\n")
    # the NMI's saved registers and the top of its stack (a test build checks the two copies agree)
    fp.write(f"#define NMI_SNA_ON_ENTRY 0x{symbols['sna_on_entry']:x}\n")
    fp.write(f"#define NMI_STACK_TOP 0x{symbols['stack_top']:x}\n")
    fp.write(f"#define SAVE128_AY_AT {symbols['save128_extra.ay'] - symbols['sna_on_entry']}\n")     # the menus' font (the pico draws with it too)
    fp.write(f"#define INFO_HOUSE 0x{symbols['INFO_HOUSE']:x}\n")
    fp.write(f"#define INFO_HOUSE_SIZE {symbols['INFO_HOUSE_SIZE']}\n")
    fp.write(f"#define INFO_HOUSE_WIDTH {symbols['INFO_HOUSE_WIDTH']}\n")
    # the game information's scrolling (the software house, the genre): the tables the pico fills
    fp.write(f"#define INFO_SCROLL1 0x{symbols['startmenu.infoscroll']:x}\n")
    fp.write(f"#define INFO_SCROLL2 0x{symbols['startmenu.infoscroll2']:x}\n")
    fp.write(f"#define INFO_SCROLL_CUR 0x{symbols['startmenu.scrollcur']:x}\n")
    fp.write(f"#define INFO_SCROLL_POS 0x{symbols['startmenu.scrollpos']:x}\n")
    fp.write(f"#define INFO_SCROLL_TIMER 0x{symbols['startmenu.scrolltimer']:x}\n")
    fp.write(f"#define INFO_SCROLL_HOLD {symbols['SCROLL_HOLD']}\n")
    fp.write(f"#define INFO_HOUSE_ROW {symbols['INFO_HOUSE_ROW']}\n")
    fp.write(f"#define INFO_HOUSE_COL {symbols['INFO_HOUSE_COL']}\n")

    for k, v in symbols.items():
        if k.startswith("STARTUP_ACTION_"):
            fp.write(f"#define {k} 0x{v:x}\n")




