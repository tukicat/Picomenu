#!/bin/sh
# This file is part of picoFace (GNU GPL v3 or later; see LICENSE).
# Builds and runs the PC tests of the ZXDB downloads' pieces (nettest.cpp). A 200 KB game made
# up here (big.tzx, zipped as big.zip) checks the inflater past its 32 KB window.
set -e
D=$(cd "$(dirname "$0")" && pwd)
O=${1:-/tmp/nettest}
mkdir -p "$O"
cp "$D"/data/* "$O"/
python3 - "$O" <<'PY'
import sys, zipfile, random
o = sys.argv[1]
r = random.Random(7)
words = [bytes(r.randrange(256) for _ in range(r.randrange(3, 40))) for _ in range(300)]
data = bytearray()
while len(data) < 200000:
    data += r.choice(words) if r.random() < 0.8 else bytes([r.randrange(256)])
open(o + "/big.tzx", "wb").write(data)
z = zipfile.ZipFile(o + "/big.zip", "w", compression=zipfile.ZIP_DEFLATED)
z.write(o + "/big.tzx", "Big Game.tzx")
z.close()
PY
g++ -std=c++17 -O1 -Wall -DMINIZ_NO_MALLOC -DMINIZ_NO_STDIO -I"$D/../../pico" "$D/nettest.cpp" -x c "$D/../../pico/miniz_tinfl.c" -o "$O/nettest"
"$O/nettest" "$O"
