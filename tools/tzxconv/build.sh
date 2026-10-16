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

# Builds the PC test version of the tape converter (needs gcc and g++)
P=$(dirname "$0")/../../pico
set -e
gcc -O2 -c -DZ80_STATIC -I$P/z80/API -I$P/z80/zeta $P/z80/Z80.c -o /tmp/tzxconv_z80.o
g++ -O2 -std=c++17 -Wall -DZ80_STATIC -I$P -I$P/z80/API -I$P/z80/zeta "$(dirname "$0")/tzxconv_host.cpp" /tmp/tzxconv_z80.o -o "${1:-tzxconv_host}"
