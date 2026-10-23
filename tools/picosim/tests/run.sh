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

# Runs the simulator scripts: run.sh ROMDIR TAPEDIR [OUT]
# ROMDIR has 48.rom, 128-0.rom and 128-1.rom; TAPEDIR has the Speedlock tapes the scripts name
# (Mikie, Green Beret, Renegade 128K). Each script passes when every "expect" does.
H=$(cd "$(dirname "$0")" && pwd); R=$1; T=$2; O=${3:-/tmp/picosim-out}
mkdir -p "$O"; cat "$R/128-0.rom" "$R/128-1.rom" > "$O/128.rom"
head -c 6912 /dev/zero | tr "\\000" "U" > "$O/dummy.scr"    # a scraper screen already there (convertscr48)
{ head -c 6144 /dev/zero; head -c 768 /dev/zero | tr "\\000" "8"; } > "$O/blank.scr"    # an empty picture (viewscroll)
python3 "$H/make_levelloader.py" "$O" > /dev/null     # levelloader48's made-up game and tape
printf 'Infinite lives\r\nM 1000 0 0\r\nZ 1000 0 0\r\n' > "$O/test.pok"    # listall
printf 'Green\r\nSoftware house: Imagine Software (Ocean Software Ltd)\r\nYear: 1986\r\nControls\r\nQ up\r\nInstructions\r\nRescue the prisoners.\r\n' > "$O/Instructions.txt"    # viewscroll
fails=0
for s in "$H"/*.txt; do
    n=$(basename "$s" .txt)
    sed -e "s#{ROM48}#$R/48.rom#g; s#{ROM128_0}#$R/128-0.rom#g; s#{ROM128_1}#$R/128-1.rom#g; s#{ROM128}#$O/128.rom#g; s#{TAPES}#$T#g; s#{OUT}#$O#g; s#{HERE}#$H#g" "$s" > "$O/$n.script"
    case $n in *128*) args="-128 $R/128-0.rom $R/128-1.rom" ;; *) args="" ;; esac
    if "$H/../sim" $args "$R/48.rom" "$O/$n.script" > "$O/$n.log" 2>&1; then r=pass; else r=FAIL; fails=$((fails+1)); fi
    printf '%-16s %s  %s\n' "$n" "$r" "$(grep -c '^PASS' "$O/$n.log") expects passed"
done
[ $fails -eq 0 ]
