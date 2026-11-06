#!/usr/bin/env python3
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
#
# make_netroots.py <cacert.pem> <out.h>: the root certificates the Pico W trusts for ZXDB
# downloads (pico/net.h), as DER arrays, from a Mozilla root store file (cacert.pem, as the
# certifi project publishes it: https://github.com/certifi/python-certifi). Only the roots
# below are kept: they sign the certificates of most of the web's servers (Let's Encrypt,
# Google, DigiCert, Sectigo, Amazon, GlobalSign), and each costs flash and, while connected,
# about 600 bytes of RAM.
import base64, re, sys

WANT = [
    "ISRG Root X1", "ISRG Root X2", "GTS Root R1", "GTS Root R4", "DigiCert Global Root G2",
    "USERTrust RSA Certification Authority", "USERTrust ECC Certification Authority",
    "Amazon Root CA 1", "GlobalSign Root CA - R3",
]
pem, out = sys.argv[1], sys.argv[2]
text = open(pem, encoding="utf-8").read()
found = {}
for m in re.finditer(r'# Label: "([^"]+)".*?-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----', text, re.S):
    if m.group(1) in WANT and m.group(1) not in found:
        found[m.group(1)] = base64.b64decode("".join(m.group(2).split()))
missing = [w for w in WANT if w not in found]
if missing:
    sys.exit("not in %s: %s" % (pem, ", ".join(missing)))
with open(out, "w") as o:
    o.write("// Made by tools/make_netroots.py from Mozilla's root store (certifi's cacert.pem):\n")
    o.write("// the roots the Pico W trusts for ZXDB downloads (net.h). %d roots.\n" % len(WANT))
    o.write("#pragma once\n#include <stdint.h>\n#include <stddef.h>\nnamespace netroots {\n")
    for i, w in enumerate(WANT):
        d = found[w]
        o.write("// %s\nstatic const uint8_t R%d[] = {\n" % (w, i))
        for k in range(0, len(d), 16):
            o.write("    " + ", ".join("0x%02X" % b for b in d[k:k + 16]) + ",\n")
        o.write("};\n")
    o.write("struct Root { const uint8_t* der; size_t len; };\nstatic const Root ROOTS[] = {\n")
    for i in range(len(WANT)):
        o.write("    { R%d, sizeof(R%d) },\n" % (i, i))
    o.write("};\nstatic const int COUNT = %d;\n}   // namespace netroots\n" % len(WANT))
print("%s: %d roots, %d bytes" % (out, len(WANT), sum(len(found[w]) for w in WANT)))
