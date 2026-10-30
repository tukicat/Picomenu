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

"""Checks after a build that enough RAM is left for the heap, and that the program fits before the
flash file system: python check_heap.py nm picoface.elf

The RAM the program's variables don't use (from the end of .bss to __StackLimit) is the heap,
and littlefs takes about 2 KB of it when the flash is mounted and 1 KB more for each open file
(FatFs 512 bytes for a long name). A test build left only 2 KB and the Picoface
stopped answering the NMI button, so the build now fails below MIN_HEAP.

It also checks that the memory lent to FatFs (list_pool) is 8-aligned: FatFs keeps its long name
buffer there, read and written 16 bits at a time, and the Cortex-M0+ stops (a HardFault) on a
16-bit access at an odd address. In a test build list_pool was at an odd address and copying
a folder (or making one) stopped the Pico.
"""
import subprocess, sys

MIN_HEAP = 5 * 1024

nm, elf = sys.argv[1], sys.argv[2]
syms = {}
for line in subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout.splitlines():
    parts = line.split()
    if len(parts) == 3:
        syms[parts[2]] = int(parts[0], 16)
        if parts[2].endswith('list_pool'):          # a static: the name may be decorated
            syms['list_pool'] = int(parts[0], 16)
heap = syms['__StackLimit'] - syms['__end__']
print(f"RAM left for the heap: {heap} bytes (at least {MIN_HEAP} needed)")
if heap < MIN_HEAP:
    sys.exit(f"error: only {heap} bytes of RAM are left for the heap; littlefs and FatFs need {MIN_HEAP}")
pool = syms.get('list_pool')
if pool is None:
    sys.exit("error: list_pool not found")
print(f"list_pool at 0x{pool:08x}")
if pool & 7:
    sys.exit(f"error: list_pool (lent to FatFs) is at 0x{pool:08x}, not 8-aligned")

# The program must end before the file system of the internal flash, which starts 248 KB into
# it (fwupdate.h APP_MAX): a bigger one would be refused by Update PicoMenu ("Too big") and,
# put on with BOOTSEL, would write over the start of the file system. A test build was
# 252 KB.
fs_kb = int(sys.argv[3]) if len(sys.argv) > 3 else 1800     # (the Pico W build: 1400)
reserve_kb = int(sys.argv[4]) if len(sys.argv) > 4 else 0  # (the Pico W build: 8 KB for the Bluetooth pairing)
APP_MAX = 2 * 1024 * 1024 - fs_kb * 1024 - reserve_kb * 1024
end = syms.get('__flash_binary_end')
if end is None:
    sys.exit("error: __flash_binary_end not found")
size = end - 0x10000000
print(f"Program: {size} bytes ({APP_MAX - size} left before the file system)")
if size > APP_MAX:
    sys.exit(f"error: the program is {size} bytes, more than the {APP_MAX} before the flash's file system")
