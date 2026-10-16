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


// File operations that work the same on the SD card (FatFs) and the internal flash
// (littlefs): used by the NMI menu's file keys.  A path is in the form of sd_cwd,
// "/dir/name.ext", relative to the root of its drive.  Nothing here overwrites a file
// that is already there.  Tested on a PC against both file systems.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "file_wrapper.h"

namespace fsops {

// The same values as FILEOP_OK, FILEOP_NAME_IN_USE and FILEOP_FAILED in picoface.cxx
static const uint8_t OK = 0;
static const uint8_t NAME_IN_USE = 1;
static const uint8_t FAILED = 2;
static const uint8_t CANCELLED = 12;       // the same as FILEOP_CANCELLED: the user stopped a copy

// Called while a file is copied with the number of bytes just written (0 before a file starts):
// returning false stops the copy.
typedef bool (*CopyStep)(void* context, size_t bytes);

enum Kind { KIND_NONE = 0, KIND_FILE = 1, KIND_DIR = 2 };

inline Drive drive_of(uint8_t drive) {
    return drive == 1 ? Drive::EXTERNAL_SD_CARD : Drive::INTERNAL_FLASH;
}

inline bool is_root(const char* path) {
    return path[0] == '\0' || (path[0] == '/' && path[1] == '\0');
}

// What is at path: nothing, a file or a directory
inline Kind kind(Drive drive, const char* path) {
    if (is_root(path)) {
        return KIND_DIR;
    }
    if (drive == Drive::EXTERNAL_SD_CARD) {
        // static: a FILINFO is over 500 bytes, too much for the Pico's small stack
        static FILINFO info;
        if (f_stat(path, &info) != FR_OK) {
            return KIND_NONE;
        }
        return (info.fattrib & AM_DIR) != 0 ? KIND_DIR : KIND_FILE;
    }
    static lfs_info info;       // static: its name alone is 256 bytes
    if (pico_stat(path, &info) < 0) {
        return KIND_NONE;
    }
    return info.type == LFS_TYPE_DIR ? KIND_DIR : KIND_FILE;
}

// Renames or moves a file or directory within a drive.  Never replaces anything
// (littlefs would, FatFs would refuse).
inline uint8_t rename_path(Drive drive, const char* from, const char* to) {
    if (kind(drive, to) != KIND_NONE) {
        return NAME_IN_USE;
    }
    if (drive == Drive::EXTERNAL_SD_CARD) {
        return f_rename(from, to) == FR_OK ? OK : FAILED;
    }
    return pico_rename(from, to) >= 0 ? OK : FAILED;
}

// Removes a file, or a directory that is empty
inline bool remove_path(Drive drive, const char* path) {
    if (is_root(path)) {
        return false;
    }
    if (drive == Drive::EXTERNAL_SD_CARD) {
        return f_unlink(path) == FR_OK;
    }
    return pico_remove(path) >= 0;
}

inline uint8_t make_dir(Drive drive, const char* path) {
    // no look first: it would add to the stack, which the SD card's f_mkdir needs a lot of
    if (drive == Drive::EXTERNAL_SD_CARD) {
        FRESULT fr = f_mkdir(path);
        return fr == FR_OK ? OK : fr == FR_EXIST ? NAME_IN_USE : FAILED;
    }
    int r = pico_mkdir(path);
    return r >= 0 ? OK : r == LFS_ERR_EXIST ? NAME_IN_USE : FAILED;
}

// Copies a file, between drives if need be.  The copy must be a new file; one that
// fails part way (a full card or flash) is removed.  buffer is the memory to copy through.
inline uint8_t copy_file(Drive from_drive, const char* from, Drive to_drive, const char* to,
        uint8_t* buffer, size_t buffer_size, CopyStep step = nullptr, void* context = nullptr) {
    if (kind(to_drive, to) != KIND_NONE) {
        return NAME_IN_USE;
    }
    static FileHandle src, dst;
    if (!src.open(from_drive, from, true)) {
        return FAILED;
    }
    if (!dst.open(to_drive, to, false)) {
        src.close();
        return FAILED;
    }
    bool ok = true;
    bool cancelled = false;
    for (;;) {
        int got = src.read(buffer, buffer_size);
        if (got < 0) {
            ok = false;
            break;
        }
        if (got == 0) {
            break;
        }
        if (dst.write(buffer, got) != got) {
            ok = false;
            break;
        }
        if (step != nullptr && !step(context, (size_t) got)) {
            ok = false;
            cancelled = true;
            break;
        }
    }
    src.close();
    if (!dst.close()) {
        ok = false;
    }
    if (!ok) {
        remove_path(to_drive, to);
    }
    return ok ? OK : cancelled ? CANCELLED : FAILED;
}

// Moves a file: a rename within a drive, a copy and then removing the original between
// drives.  If the original cannot be removed after the copy is made, FAILED is returned
// and both are left.
inline uint8_t move_file(Drive from_drive, const char* from, Drive to_drive, const char* to,
        uint8_t* buffer, size_t buffer_size) {
    if (from_drive == to_drive) {
        return rename_path(from_drive, from, to);
    }
    uint8_t status = copy_file(from_drive, from, to_drive, to, buffer, buffer_size);
    if (status == OK && !remove_path(from_drive, from)) {
        status = FAILED;
    }
    return status;
}

// The path to give DirHandle::open for a directory of a drive
inline void dir_open_path(Drive drive, const char* path, char* out, size_t size) {
    if (drive == Drive::EXTERNAL_SD_CARD) {
        snprintf(out, size, "/sd%s", path);
    }
    else {
        snprintf(out, size, "%s", is_root(path) ? "/" : path);
    }
}

// Removes a directory and everything in it.  path is the directory (not the root) in a
// buffer of path_size bytes that is used and put back.  dirs is max_depth DirHandles to
// read the levels with; directories nested deeper than that are not removed.  scratch is
// memory to build the paths for opening in (it should be as big as path), so that the
// recursion keeps almost nothing on the stack.
// Returns false if anything could not be removed (what was removed stays removed).
inline bool remove_tree(Drive drive, char* path, size_t path_size, DirHandle* dirs, int depth, int max_depth,
        char* scratch, size_t scratch_size) {
    if (is_root(path) || depth >= max_depth) {
        return false;
    }
    size_t base = strlen(path);
    for (;;) {
        dir_open_path(drive, path, scratch, scratch_size);
        DirHandle& dir = dirs[depth];
        if (!dir.open(scratch)) {
            return false;
        }
        // the first entry that is not . or ..
        bool found = false;
        while (!dir.atEnd()) {
            const char* name = dir.fileName();
            if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
                found = true;
                break;
            }
            if (!dir.next()) {
                break;
            }
        }
        if (!found) {
            dir.close();
            break;
        }
        size_t len = strlen(dir.fileName());
        bool is_dir = dir.isDirectory();
        if (base + 1 + len >= path_size) {
            dir.close();
            return false;
        }
        path[base] = '/';
        memcpy(path + base + 1, dir.fileName(), len + 1);
        dir.close();                    // nothing may be open while it is changed
        bool ok = is_dir ? remove_tree(drive, path, path_size, dirs, depth + 1, max_depth, scratch, scratch_size) :
            remove_path(drive, path);
        path[base] = '\0';
        if (!ok) {
            return false;
        }
    }
    return remove_path(drive, path);
}

// ---- Copying a folder and everything in it (on one drive)
//
// Walked without recursion, so the Pico's small stack is not used for the depth: the open
// directory of each level is in dirs, and from / to are one path each that grows and shrinks
// as the walk goes down and up.  The tree is walked twice: once to count the bytes (for the
// progress bar), once to copy.  The source tree is not changed, so the folder being copied
// into must not be inside it (the caller checks that).
struct TreeCopy {
    Drive drive;
    DirHandle* dirs;                // max_depth DirHandles
    int max_depth;                  // at most 16
    char* from;                     // the folder to copy, in a buffer of from_size bytes
    size_t from_size;
    char* to;                       // where the copy goes: must not exist yet
    size_t to_size;
    char* scratch;                  // for opening directories (as big as from)
    size_t scratch_size;
    uint8_t* buffer;                // memory to copy files through
    size_t buffer_size;
    // makes a folder (the SD card's needs memory lent to FatFs): default fsops::make_dir
    uint8_t (*make_folder)(Drive drive, const char* path);
    // called as the copy goes on; false cancels it
    bool (*report)(void* context, uint64_t done, uint64_t total);
    void* context;
    uint64_t total;                 // set by the counting walk
    uint64_t done;
    uint32_t files;
    uint32_t folders;
};

inline bool tree_step(void* context, size_t bytes) {
    TreeCopy* t = static_cast<TreeCopy*>(context);
    t->done += bytes;
    return t->report == nullptr || t->report(t->context, t->done, t->total);
}

inline uint8_t walk_tree(TreeCopy& t, bool counting) {
    size_t from_len[16], to_len[16];
    int depth = 0;
    int opened = 0;             // dirs[0 .. opened-1] are open
    uint8_t status = OK;
    if (t.max_depth > 16 || t.max_depth < 1) {
        return FAILED;
    }
    if (!counting) {
        status = (t.make_folder != nullptr ? t.make_folder : make_dir)(t.drive, t.to);
        if (status != OK) {
            return status == NAME_IN_USE ? FAILED : status;
        }
    }
    dir_open_path(t.drive, t.from, t.scratch, t.scratch_size);
    if (!t.dirs[0].open(t.scratch)) {
        return FAILED;
    }
    opened = 1;
    from_len[0] = strlen(t.from);
    to_len[0] = strlen(t.to);
    for (;;) {
        DirHandle& dir = t.dirs[depth];
        bool failed = false;
        while (!dir.atEnd() && (strcmp(dir.fileName(), ".") == 0 || strcmp(dir.fileName(), "..") == 0)) {
            if (!dir.next()) {
                failed = true;
                break;
            }
        }
        if (failed) {
            status = FAILED;
            break;
        }
        if (dir.atEnd()) {
            dir.close();
            opened--;
            if (depth == 0) {
                break;
            }
            depth--;
            t.from[from_len[depth]] = '\0';
            t.to[to_len[depth]] = '\0';
            if (!t.dirs[depth].next()) {
                status = FAILED;
                break;
            }
            continue;
        }
        size_t len = strlen(dir.fileName());
        if (from_len[depth] + 1 + len >= t.from_size || to_len[depth] + 1 + len >= t.to_size) {
            status = FAILED;            // a path too long
            break;
        }
        bool is_dir = dir.isDirectory();
        uint32_t size = dir.fileSize();
        t.from[from_len[depth]] = '/';
        memcpy(t.from + from_len[depth] + 1, dir.fileName(), len + 1);
        t.to[to_len[depth]] = '/';
        memcpy(t.to + to_len[depth] + 1, dir.fileName(), len + 1);
        if (is_dir) {
            if (depth + 1 >= t.max_depth) {
                status = FAILED;        // too deep
                break;
            }
            if (!counting) {
                status = (t.make_folder != nullptr ? t.make_folder : make_dir)(t.drive, t.to);
                if (status != OK) {
                    status = status == NAME_IN_USE ? FAILED : status;
                    break;
                }
            }
            bool entered;
            if (t.drive == Drive::EXTERNAL_SD_CARD) {
                entered = t.dirs[depth + 1].openSubdir(dir);
            }
            else {
                dir_open_path(t.drive, t.from, t.scratch, t.scratch_size);
                entered = t.dirs[depth + 1].open(t.scratch);
            }
            if (!entered) {
                status = FAILED;
                break;
            }
            opened++;
            depth++;
            from_len[depth] = strlen(t.from);
            to_len[depth] = strlen(t.to);
            if (counting) {
                t.folders++;
            }
            continue;
        }
        if (counting) {
            t.total += size;
            t.files++;
        }
        else {
            if (!tree_step(&t, 0)) {                // a chance to cancel between files
                status = CANCELLED;
                break;
            }
            status = copy_file(t.drive, t.from, t.drive, t.to, t.buffer, t.buffer_size, tree_step, &t);
            if (status != OK) {
                break;
            }
        }
        t.from[from_len[depth]] = '\0';
        t.to[to_len[depth]] = '\0';
        if (!t.dirs[depth].next()) {
            status = FAILED;
            break;
        }
    }
    while (opened > 0) {
        t.dirs[--opened].close();
    }
    return status;
}

// Copies the folder t.from to t.to (which must not exist).  Returns OK, NAME_IN_USE (it exists),
// CANCELLED (report said stop) or FAILED.  After a failure or a cancel what was copied is removed.
// t.from and t.to are put back as they were.
inline uint8_t copy_tree(TreeCopy& t) {
    if (kind(t.drive, t.to) != KIND_NONE) {
        return NAME_IN_USE;
    }
    size_t from_base = strlen(t.from);
    size_t to_base = strlen(t.to);
    t.total = 0;
    t.done = 0;
    t.files = 0;
    t.folders = 0;
    uint8_t status = walk_tree(t, true);
    t.from[from_base] = '\0';
    t.to[to_base] = '\0';
    if (status == OK) {
        status = walk_tree(t, false);
        t.from[from_base] = '\0';
        t.to[to_base] = '\0';
    }
    if (status != OK) {
        // what was made (if anything) goes again
        if (kind(t.drive, t.to) == KIND_DIR) {
            remove_tree(t.drive, t.to, t.to_size, t.dirs, 0, t.max_depth, t.scratch, t.scratch_size);
        }
        t.to[to_base] = '\0';
    }
    return status;
}


}   // namespace fsops
