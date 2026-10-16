/*
This file is part of picoFace.

picoFace is free software: you can redistribute it and/or modify it under the terms of
the GNU General Public License as published by the Free Software Foundation, either
version 3 of the License, or (at your option) any later version.

picoFace is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with picoFace. If
not, see <https://www.gnu.org/licenses/>.
*/

// The SD card for tools/picosim: a RAM disk under FatFs (drive 0), and sd_get_by_num
#include <string.h>
#include <stdlib.h>
#include "ff.h"
#include "diskio.h"
#include "hw_config.h"

#define SECTORS (64 * 1024 * 2)            // 64 MB of 512-byte sectors
static uint8_t* disk;

DSTATUS disk_initialize(BYTE pdrv) { if (!disk) disk = (uint8_t*) calloc(SECTORS, 512); return pdrv == 0 ? 0 : STA_NOINIT; }
DSTATUS disk_status(BYTE pdrv) { return pdrv == 0 && disk ? 0 : STA_NOINIT; }
DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv || sector + count > SECTORS) return RES_PARERR;
    memcpy(buff, disk + sector * 512, count * 512); return RES_OK;
}
DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv || sector + count > SECTORS) return RES_PARERR;
    memcpy(disk + sector * 512, buff, count * 512); return RES_OK;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv) return RES_PARERR;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t*) buff = SECTORS; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD*) buff = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD*) buff = 1; return RES_OK;
    }
    return RES_PARERR;
}
DWORD get_fattime(void) { return ((DWORD) (2026 - 1980) << 25) | (10 << 21) | (15 << 16); }

static sd_card_t card = { "0:" };
sd_card_t* sd_get_by_num(size_t num) { return num == 0 ? &card : 0; }
