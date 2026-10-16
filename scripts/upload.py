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


import usb
import argparse
import pathlib
import re
import lz4
import convertZ80


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description = 'Upload snap shot'
    )

    parser.add_argument('-p', '--port', default='/dev/ttyACM0')
    parser.add_argument('filename')

    args = parser.parse_args()


    file = pathlib.Path(args.filename)

    print(file.name)

    s = file.stem


    
    s = re.sub("\([^)]+\)", "", s)
    s = re.sub("\[[^]]+\]", "", s).strip()
    print(s)


    if file.suffix.lower() == ".z80":
        print("convert z80 to v1 uncompressed")
        snapshot = convertZ80.Z80Snapshot()
        snapshot.process_file(args.filename)
        input_data = snapshot.to_bytes()
    else:
        input_data = file.read_bytes()

    input_data = lz4.block.compress(input_data, mode='high_compression', store_size=False, compression=12)    

    command = usb.join_params( 
        [ "upload", s + file.suffix + "z"]
    )

    usb.send_command(args.port, command, input_data, None)
