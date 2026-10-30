#!/bin/sh
# This file is part of picoFace (GNU GPL v3 or later; see the other build.sh files).
# Builds the PC check of the Bluetooth tape's sound (needs gcc, g++ and the Pico SDK's BTstack:
# PICO_SDK_PATH, or ../../../pico-sdk next to this project)
set -e
D=$(dirname "$0")
P=$D/../../pico
B=${PICO_SDK_PATH:-$D/../../../../pico-sdk}/lib/btstack
O=${2:-/tmp/btaudio_obj}
mkdir -p $O
I="-I$D -I$B/src -I$B/src/classic -I$B/3rd-party/bluedroid/encoder/include -I$B/3rd-party/bluedroid/decoder/include -I$B/platform/posix"
for c in $B/src/classic/btstack_sbc_encoder_bluedroid.c $B/src/classic/btstack_sbc_decoder_bluedroid.c $B/src/classic/btstack_sbc_plc.c \
         $B/3rd-party/bluedroid/encoder/srce/*.c $B/3rd-party/bluedroid/decoder/srce/*.c $B/src/btstack_util.c; do
    gcc -O2 -w $I -c $c -o $O/$(basename $c .c).o
done
gcc -O2 -w $I -c $D/stubs.c -o $O/stubs.o
gcc -O2 -c -DZ80_STATIC -I$P/z80/API -I$P/z80/zeta $P/z80/Z80.c -o $O/z80.o
g++ -O2 -std=c++17 -Wall -DZ80_STATIC $I -I$P -I$P/z80/API -I$P/z80/zeta $D/btaudio_host.cpp $O/*.o -o "${1:-btaudio_host}"
