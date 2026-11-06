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

# The answers the zxdb scripts' pretend network gives (tools/picosim/net_host.cpp): whole HTTP
# responses, as ZXInfo's API (output=flat), Spectrum Computing and archive.org would send them.
# make_zxdb.py OUT [TAPE]: the game is TAPE (tests/tapein.tap), zipped as GreenBeret.tap.
import io, os, sys, zipfile

out = sys.argv[1]
tape = open(sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "tapein.tap"), "rb").read()

def answer(name, body, status="200 OK", headers=(), chunked=False):
    h = "HTTP/1.1 %s\r\nContent-Type: text/plain\r\n" % status
    for k in headers:
        h += k + "\r\n"
    if chunked:
        h += "Transfer-Encoding: chunked\r\n\r\n"
        data = b""
        for i in range(0, len(body), 300):
            piece = body[i:i + 300]
            data += b"%x\r\n" % len(piece) + piece + b"\r\n"
        data += b"0\r\n\r\n"
    else:
        h += "Content-Length: %d\r\n\r\n" % len(body)
        data = body
    open(os.path.join(out, name), "wb").write(h.encode() + data)

search = """total.relation=eq
total.value=12
hits.0._id=0002259
hits.0.title=Green Beret
hits.0.originalYearOfRelease=1986
hits.0.genre=Arcade Game: Action
hits.0.availability=Available
hits.0.publishers.0.name=Imagine Software Ltd
hits.0.screens.0.type=Running screen
hits.0.screens.0.scrUrl=/zxdb/sinclair/entries/0002259/GreenBeret-run.scr
hits.0.screens.1.scrUrl=/zxdb/sinclair/entries/0002259/GreenBeret.scr
hits.0.screens.1.type=Loading screen
hits.0.additionalDownloads.0.path=/pub/sinclair/games-info/g/GreenBeret.txt
hits.0.additionalDownloads.0.size=1598
hits.0.additionalDownloads.0.type=Instructions
hits.0.additionalDownloads.1.path=/pub/sinclair/games-maps/g/GreenBeret.png
hits.0.additionalDownloads.1.type=Game map
hits.0.additionalDownloads.2.type=POK pokes file
hits.0.additionalDownloads.2.path=/zxdb/sinclair/pokes/g/Green Beret (1986)(Imagine Software).pok
hits.1._id=0030088
hits.1.title=Green Beret 2
hits.1.originalYearOfRelease=1991
hits.1.availability=Never released
hits.1.publishers.0.name=Somebody Else
"""
for i in range(2, 10):
    search += "hits.%d._id=00900%02d\nhits.%d.title=Greenery %d\nhits.%d.availability=Available\n" % (i, i, i, i, i)
answer("search.http", search.encode(), chunked=True)
page2 = "total.value=12\nhits.0._id=0009010\nhits.0.title=Greener Pastures\nhits.0.availability=Available\nhits.1._id=0009011\nhits.1.title=Greenhouse\n"
answer("search2.http", page2.encode())

entry = """title=Green Beret
originalYearOfRelease=1986
availability=Available
genre=Arcade Game: Action
publishers.0.name=Imagine Software Ltd
releases.0.releaseSeq=0
releases.0.files.0.path=/pub/sinclair/games/g/GreenBeret.tap.zip
releases.0.files.0.type=Tape image
releases.0.files.1.type=Instructions
releases.0.files.1.path=/pub/sinclair/games-info/g/GreenBeret.txt
releases.1.files.0.type=Snapshot image
releases.1.files.0.path=/zxdb/sinclair/entries/0002259/GreenBeret.z80.zip
"""
answer("entry.http", entry.encode())
answer("entry2.http", b"title=Green Beret 2\navailability=Never released\nreleases.0.files.0.path=/zxdb/sinclair/entries/0030088/GB2.tap.zip\nreleases.0.files.0.type=Tape image\n")

# archive.org sends the file from another of its servers
answer("redirect.http", b"", "302 Found", ["Location: https://ia800100.us.archive.org/view_archive.php?archive=x&file=GreenBeret.tap.zip"])
z = io.BytesIO()
with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
    zf.writestr("readme.txt", "Not a game")
    zf.writestr("GreenBeret.tap", tape)
answer("file.http", z.getvalue())

scr = bytes([0x55]) * 6144 + bytes([0x38]) * 768
answer("scr.http", scr)
# a games-info text as World of Spectrum has them: CR LF, tabs, long lines, blank lines, a line
# that would read as a heading, one as a key, one as a comment, a letter not in ASCII
instr = (b"GREEN BERET\r\n\r\n\r\n\r\nRescue the prisoners from the enemy camp before they are taken away, "
    b"running, climbing ladders and\tusing your knife.\r\n\r\nCONTROLS\r\nQ up, A down\r\nYear: 1985 in Europe\r\n"
    b";not a comment\r\nCaf\xe9\r\n")
answer("instr.http", instr)
# a long one (Juggernaut's is): 7 KB, more than the 2 KB kept up to 2.5.0 test 11
long_instr = b"".join(b"Part %03d of the long instructions, kept whole.\r\n" % i for i in range(1, 141)) + b"THE END\r\n"
answer("instr-long.http", long_instr)
# and a game's details with 8 KB of instructions: more than 15 pages on the V screen
details = "Software house: Long Games\r\nYear: 1987\r\nInstructions\r\n" + "".join("Page filler line %03d.\r\n" % i for i in range(1, 361))
open(os.path.join(out, "long.info.txt"), "w", newline="").write(details[:8000] + "\r\nLAST LINE\r\n")

# a title's pokes (the .pok as ZXDB has them)
answer("pok.http", b"NInfinite lives\r\nZ  8 35899   0   0\r\nNStart on level 3\r\nZ  8 36000   3   0\r\nY\r\n")

answer("missing.http", b"Not here", "404 Not Found")
