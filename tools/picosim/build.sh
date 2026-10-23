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

# Builds picosim (the firmware and its NMI ROM on a PC). Needs gcc, g++, sjasmplus, python3.
set -e
H=$(cd "$(dirname "$0")" && pwd); P=$H/../../pico; O=${1:-$H/obj}
mkdir -p "$O"
# SIM_SANITIZE=1: stop at any misaligned access (a PC allows them; the Pico's Cortex-M0+ stops with
# a HardFault on a 16 or 32-bit access that is not aligned), with the memory lent to FatFs put at
# an odd address (as list_pool was in a test build, when copying a folder stopped the Pico)
if [ -n "$SIM_SANITIZE" ]; then SAN="-fsanitize=alignment -fno-sanitize-recover=all"; SIM_FFFLAGS="$SIM_FFFLAGS -DFF_HOST_ODD_POOL"; else SAN=""; fi
CC="gcc $SAN"; CXX="g++ $SAN"
# the NMI ROM as nmi.c / nmi.h, the same way the firmware build makes it
(cd "$O" && sjasmplus --nologo --sym=nmirom.sym "$P/../nmirom/nmirom.asm" >/dev/null && python3 "$P/asm_to_c_file.py" >/dev/null)
INC="-I$H/stubs -I$O -I$P -I$P/FatFs_SPI/ff15/source -I$P/FatFs_SPI/include -I$P/littlefs-lib -I$P/z80/API -I$P/z80/zeta"
$CXX -std=c++17 -O1 -g -c -DPICOFACE_HOST -Dmain=picoface_main -DZ80_STATIC $INC $P/picoface.cxx -o $O/picoface.o
$CXX -std=c++17 -O1 -g -c $INC $P/file_wrapper.cxx -o $O/file_wrapper.o
$CXX -std=c++17 -O2 -g -c -DZ80_STATIC $INC $P/tzxconv_pico.cxx -o $O/tzxconv_pico.o
$CC -O2 -c -DZ80_STATIC $INC $P/z80/Z80.c -o $O/z80.o
$CC -O1 -c $INC $P/lz4.c -o $O/lz4.o
for f in ff ffunicode; do $CC -O1 -c $INC $P/FatFs_SPI/ff15/source/$f.c -o $O/$f.o; done
$CC -O1 -c -DFF_HOST_HEAP_LIMIT $SIM_FFFLAGS $INC $P/FatFs_SPI/ff15/source/ffsystem.c -o $O/ffsystem.o   # malloc as on the Pico
$CC -O1 -g -c $INC $P/littlefs-lib/lfs.c -o $O/lfs.o
$CC -O1 -g -c $INC $H/pico_hal_host.c -o $O/pico_hal_host.o
$CC -O1 -c $INC $H/ramdisk.c -o $O/ramdisk.o
$CXX -std=c++17 -O1 -c $INC $H/host_stubs.cpp -o $O/host_stubs.o
$CC -O1 -c $INC $O/nmi.c -o $O/nmi.o
$CXX -std=c++17 -O1 -g -DZ80_STATIC $INC $H/sim.cpp $O/picoface.o $O/file_wrapper.o $O/tzxconv_pico.o $O/z80.o \
    $O/lz4.o $O/ff.o $O/ffunicode.o $O/ffsystem.o $O/lfs.o $O/pico_hal_host.o $O/ramdisk.o $O/host_stubs.o $O/nmi.o -o $H/sim
echo built $H/sim
