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

# Makes the test tapes, converts each with the PC build and checks the result.
# usage: runtests.sh 48.rom [workdir]
set -e
H=$(cd "$(dirname "$0")" && pwd)
ROM=$1; W=${2:-/tmp/tzxconv-tests}
mkdir -p "$W/out"
cd "$H" && sjasmplus --nologo game.asm >/dev/null && sjasmplus --nologo turbo.asm >/dev/null
python3 "$H/maketapes.py" "$W/tapes" >/dev/null
"$H/../build.sh" "$W/tzxconv_host"
pass=0; fail=0
# expected: ok = loads and the snapshot checks out; error = the converter says it did not
# load (no file); warn = saved with a warning; wrong = the converter thinks it loaded but
# the snapshot is not the game
expect() {
    case $1 in
        bad_rom_checksum*|unknown_block*|bad_turbo_checksum*) echo error ;;
        missing_turbo*) echo warn ;;
        *) echo ok ;;
    esac
}
for t in "$W"/tapes/*; do
    n=$(basename "$t"); e=$(expect "$n")
    rc=0; "$W/tzxconv_host" "$ROM" "$t" "$W/out/$n.z80" > "$W/out/$n.log" 2>&1 || rc=$?
    if [ $rc -eq 0 ]; then
        kind=turbo; case $n in standard*) kind=plain ;; esac
        if python3 "$H/checkz80.py" "$W/out/$n.z80" $kind > "$W/out/$n.check"; then got=ok; else got=wrong; fi
    elif [ $rc -eq 3 ]; then
        got=warn
    else
        got=error
    fi
    if [ "$got" = "$e" ]; then pass=$((pass+1)); r=pass; else fail=$((fail+1)); r=FAIL; fi
    printf '%-26s expected %-6s got %-6s %s  (%s)\n' "$n" "$e" "$got" "$r" "$(grep '^result\|^emulated' "$W/out/$n.log" | tr '\n' ' ' | cut -c1-90)"
done
echo "passed $pass, failed $fail"
[ $fail -eq 0 ]
