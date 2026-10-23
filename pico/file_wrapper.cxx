/*
Copyright (C) 2024 Brian Apps

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

#include "file_wrapper.h"

bool FileHandle::open(Drive drive, const char* filename, bool readOnly) {
    this->drive = drive;
    if (drive == Drive::EXTERNAL_SD_CARD) {;
        memset(&sdFile, 0, sizeof(sdFile));
        FRESULT res = f_open(&sdFile, filename, 
            readOnly ? FA_READ : (FA_WRITE | FA_CREATE_ALWAYS));
        return res == FR_OK;
    }
    else {
        intFile = pico_open(
            filename, readOnly ? (LFS_O_RDONLY) :
            (LFS_O_CREAT | LFS_O_TRUNC | LFS_O_WRONLY)
        );
        return intFile >= 0;
    }
}

bool FileHandle::openAppend(Drive drive, const char* filename) {
    this->drive = drive;
    if (drive == Drive::EXTERNAL_SD_CARD) {
        memset(&sdFile, 0, sizeof(sdFile));
        return f_open(&sdFile, filename, FA_WRITE | FA_OPEN_APPEND) == FR_OK;
    }
    intFile = pico_open(filename, LFS_O_CREAT | LFS_O_APPEND | LFS_O_WRONLY);
    return intFile >= 0;
}

bool FileHandle::open(const char* filename, bool readOnly) {
    if (strStartsWith(filename, "/sd")) {
        return open(Drive::EXTERNAL_SD_CARD, filename + 3, readOnly);
    }
    else {
        return open(Drive::INTERNAL_FLASH, filename, readOnly);
    }
}


bool DirHandle::open(const char* path) {
    if (strStartsWith(path, "/sd")) {
        drive = Drive::EXTERNAL_SD_CARD;
        memset(&sdDir, 0, sizeof(sdDir));
        memset(&sdFileInfo, 0, sizeof(sdFileInfo));
        if (f_findfirst(&sdDir, &sdFileInfo, path + 3, "*") == FR_OK) {
            return true;
        }
        // f_findfirst opens the folder, then reads its first entry: a read that fails (a card
        // error) leaves it open, taking one of FatFs's 16 places for open files and folders, so
        // it is closed here (a folder that never opened is not: f_closedir refuses it)
        f_closedir(&sdDir);
        return false;
    }
    else {
        drive = Drive::INTERNAL_FLASH;
        // "/fl" and a path is a directory of the flash, in the form the SD card paths are
        // in (the root is empty); anything else is a path as it is
        if (strStartsWith(path, "/fl") && (path[3] == '\0' || path[3] == '/')) {
            path += 3;
            if (path[0] == '\0') {
                path = "/";
            }
        }
        intDir = pico_dir_open(path);
        if (intDir < 0) {
            return false;
        }
        int ret = pico_dir_read(intDir, &intInfo);
        if (ret == 0) {
            intInfo.name[0] = '\0';
        }
        if (ret < 0) {
            pico_dir_close(intDir);     // (as above)
            return false;
        }
        return true;
    }
}


bool DirHandle::openSubdir(DirHandle& parent) {
    drive = Drive::EXTERNAL_SD_CARD;
    memset(&sdDir, 0, sizeof(sdDir));
    memset(&sdFileInfo, 0, sizeof(sdFileInfo));
    if (f_opendir_last(&sdDir, &parent.sdDir) != FR_OK) {
        return false;
    }
    sdDir.pat = "*";
    if (f_findnext(&sdDir, &sdFileInfo) != FR_OK) {
        f_closedir(&sdDir);             // (as in open)
        return false;
    }
    return true;
}


bool DirHandle::next() {
    if (drive == Drive::EXTERNAL_SD_CARD) {
        return f_findnext(&sdDir, &sdFileInfo) == FR_OK;
    }
    else {
        int ret = pico_dir_read(intDir, &intInfo);
        if (ret == 0) {
            intInfo.name[0] = '\0';
        }
        return ret >= 0;
    }
}


bool DirHandle::getStats(uint32_t& blockSize, uint32_t& blockCount, uint32_t& blocksUsed) {
    if (drive == Drive::EXTERNAL_SD_CARD) {
        return false;
    }
    else {
        pico_fsstat_t stats;
        if (pico_fsstat(&stats) < 0) {
            return false;
        }
        blockSize = stats.block_size;
        blocksUsed = stats.blocks_used;
        blockCount = stats.block_count;
        return true;
    }
}


