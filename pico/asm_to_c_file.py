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

# PicoMenu: the ROM is kept compressed in the firmware (LZSS, about 4 KB smaller), and
# nmi_rom_unpack() puts it in the ROM space each time the menu opens.
import sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from lzss_pack import compress, decompress

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
    fp.write("static const uint8_t NMI_ROM_LZ[] = {")
    for i in range(0, len(packed)):
        if i % 16 == 0:
            fp.write("\n    ")
        fp.write(f"0x{packed[i]:02x}, ")
    fp.write("\n};\n\n")
    fp.write(f"const uint32_t NMI_ROM_SIZE = {len(romdata)};\n\n")
    fp.write("""// Unpacks LZSS (lzss_pack.py): a flag byte for each 8 items; a set bit is a byte as it is, a
// clear one a match of 2 bytes, offset 1-4096 in the top 12 bits, length 3-18 in the low 4.
// Used for the NMI ROM and the Help text.
void lzss_unpack(const uint8_t* c, uint8_t* to, uint32_t n) {
    uint32_t o = 0;
    while (o < n) {
        uint8_t flags = *c++;
        for (int bit = 0; bit < 8 && o < n; bit++) {
            if (flags & (1u << bit)) {
                to[o++] = *c++;
            } else {
                uint32_t v = c[0] | ((uint32_t) c[1] << 8);
                c += 2;
                uint32_t off = (v >> 4) + 1;
                uint32_t len = (v & 15) + 3;
                while (len-- > 0 && o < n) {
                    to[o] = to[o - off];
                    o++;
                }
            }
        }
    }
}

void nmi_rom_unpack(uint8_t* to) {
    lzss_unpack(NMI_ROM_LZ, to, NMI_ROM_SIZE);
}
""")
    print(f"NMI ROM {len(romdata)} bytes, packed {len(packed)}")

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
    fp.write("#ifdef __cplusplus\nextern \"C\"\n#endif\nvoid lzss_unpack(const uint8_t* src, uint8_t* to, uint32_t n);\n")
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
    fp.write(f"#define NMI_CHARBANK 0x{symbols['charbank']:x}\n")
    # the game's sound chip volumes, kept while the menu is up, and where they go in a 128K save
    fp.write(f"#define NMI_AYVOLS 0x{symbols['ayvols']:x}\n")
    fp.write(f"#define SAVE128_AY_AT {symbols['save128_extra.ay'] - symbols['sna_on_entry']}\n")     # the menus' font (the pico draws with it too)
    fp.write(f"#define INFO_HOUSE 0x{symbols['INFO_HOUSE']:x}\n")
    fp.write(f"#define INFO_HOUSE_SIZE {symbols['INFO_HOUSE_SIZE']}\n")
    fp.write(f"#define INFO_HOUSE_WIDTH {symbols['INFO_HOUSE_WIDTH']}\n")

    for k, v in symbols.items():
        if k.startswith("STARTUP_ACTION_"):
            fp.write(f"#define {k} 0x{v:x}\n")




