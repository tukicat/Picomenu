#!/bin/sh
# This file is part of picoFace (GNU GPL v3 or later; see the other build.sh files).
# build.sh tape.tzx|tape.tap [mono|stereo] -> bench_sbc.elf, bench_sbc.bin: run as tools/tzxconv/bench's
# bench-run.ts (in an rp2040js checkout); each "frame" it reports is a second of sound
set -e
H=$(cd "$(dirname "$0")" && pwd); P=$H/../../../pico
B=${PICO_SDK_PATH:-$H/../../../../../pico-sdk}/lib/btstack
python3 -c "import sys; print(','.join(map(str, open(sys.argv[1],'rb').read()[:200000])))" "$1" > "$H/tape.inc"
case "$1" in *.tzx|*.TZX) TZX=true ;; *) TZX=false ;; esac
[ "$2" = stereo ] && ST=-DBENCH_STEREO || ST=
F="-mcpu=cortex-m0plus -mthumb -ffunction-sections -fdata-sections -Os -I$H/.. -I$P -I$P/z80/API -I$P/z80/zeta -DZ80_STATIC -I$B/src -I$B/src/classic -I$B/3rd-party/bluedroid/encoder/include"
O=$H/obj; mkdir -p $O; rm -f $O/*.o
for c in $B/src/classic/btstack_sbc_encoder_bluedroid.c $B/3rd-party/bluedroid/encoder/srce/*.c; do
    arm-none-eabi-gcc $F -w -c $c -o $O/$(basename $c .c).o
done
arm-none-eabi-g++ $F -std=c++17 -fno-exceptions -fno-rtti -DBENCH_TZX=$TZX $ST -c "$H/bench_sbc.cpp" -o $O/bench.o
arm-none-eabi-g++ -mcpu=cortex-m0plus -mthumb -nostartfiles --specs=nano.specs -T "$H/../../tzxconv/bench/bench.ld" -Wl,--gc-sections $O/*.o -o "$H/bench_sbc.elf"
arm-none-eabi-objcopy -O binary -j .text -j .data "$H/bench_sbc.elf" "$H/bench_sbc.bin"
arm-none-eabi-size "$H/bench_sbc.elf"
