#!/bin/sh
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

# build.sh tape.tzx 48.rom [z80 opt] [main opt] -> bench.elf, bench.bin (a 32K 128.rom: a 128K)
# BENCHFLAGS="-DBENCH_NOACCEL -DBENCH_NOFLASH": without the loader acceleration / flash loading
set -e
H=$(cd "$(dirname "$0")" && pwd); P=$H/../../../pico
python3 -c "import sys; print(','.join(map(str, open(sys.argv[1],'rb').read())))" "$2" > "$H/rom.inc"
python3 -c "import sys; print(','.join(map(str, open(sys.argv[1],'rb').read())))" "$1" > "$H/tape.inc"
F="-mcpu=cortex-m0plus -mthumb -ffunction-sections -fdata-sections -DZ80_STATIC -I$P -I$P/z80/API -I$P/z80/zeta"
arm-none-eabi-gcc $F ${3:--Os} -c "$P/z80/Z80.c" -o "$H/z80.o"
[ $(stat -c%s "$2") = 32768 ] && F="$F -DBENCH128"
arm-none-eabi-g++ $F ${4:--Os} $BENCHFLAGS -std=c++17 -fno-exceptions -fno-rtti -c "$H/bench.cpp" -o "$H/bench.o"
arm-none-eabi-g++ -mcpu=cortex-m0plus -mthumb -nostartfiles --specs=nano.specs -T "$H/bench.ld" -Wl,--gc-sections \
    "$H/bench.o" "$H/z80.o" -o "$H/bench.elf"
arm-none-eabi-objcopy -O binary -j .text -j .data "$H/bench.elf" "$H/bench.bin"
arm-none-eabi-size "$H/bench.elf"
arm-none-eabi-nm -S --size-sort -C "$H/bench.elf" | grep -i " t " | tail -4
