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


// The SD card as a USB drive for a PC: who has the card, and what to tell the PC.
//
// This part does not use TinyUSB, so it can be tested on a PC (usb_msc.cxx connects it to
// the SCSI callbacks of TinyUSB and picoface.cxx supplies the Hooks).
//
// The card is used by the Pico (the NMI menu, the tape player, the serial commands) or by
// the PC, never both:
//   * the PC gets the card the first time it asks for it (a Test Unit Ready or a read)
//     while the Pico is not using it; the Pico then lets go of the card (give);
//   * while the PC has it the Pico sees no SD card (so the NMI menu says it is in use);
//   * the PC loses it when it ejects the drive, or the cable is pulled or the PC sleeps
//     (suspend); the Pico then mounts the card again (take_back);
//   * when the Pico is using the card, the PC is told there is no disk in the drive;
//   * every time the PC gets the card back it is told the disk may have changed, so it
//     reads the file system again.

#ifndef INCLUDED_USB_MSC_CORE_H
#define INCLUDED_USB_MSC_CORE_H

#include <stdint.h>
#include <stddef.h>

namespace usbmsc {

static const uint16_t BLOCK_SIZE = 512;

// What the Pico provides. All are called from the main loop only.
struct Hooks {
    bool (*pico_idle)();                                    // the Pico is not using the card
    bool (*card_ready)();                                   // a card is there (starts it if needed)
    uint32_t (*sectors)();                                  // its size in sectors, 0 if none
    int (*read)(uint8_t* buf, uint32_t lba, uint32_t count);          // 0 if it worked
    int (*write)(const uint8_t* buf, uint32_t lba, uint32_t count);   // 0 if it worked
    void (*give)();                                         // the Pico lets go of the card
    void (*take_back)();                                    // the Pico takes the card again
    uint32_t (*now_ms)();
};

// SCSI sense: key, additional sense code and qualifier
struct Sense {
    uint8_t key = 0, asc = 0, ascq = 0;
};

static const uint8_t SENSE_NOT_READY = 0x02;
static const uint8_t SENSE_MEDIUM_ERROR = 0x03;
static const uint8_t SENSE_ILLEGAL_REQUEST = 0x05;
static const uint8_t SENSE_UNIT_ATTENTION = 0x06;

class Core {
public:
    explicit Core(const Hooks& h) : h_(h) {}

    bool holds_card() const { return holding_; }

    // True while the PC has been reading or writing very recently: the main loop then
    // keeps TinyUSB going without pausing
    bool busy() const {
        return holding_ && (uint32_t) (h_.now_ms() - last_activity_) < 30;
    }

    // Test Unit Ready: true if the PC may use the disk, otherwise sense says why
    bool test_unit_ready(Sense* sense) {
        if (ejected_) {
            return no_medium(sense);
        }
        if (!holding_ && !acquire()) {
            return no_medium(sense);
        }
        if (attention_) {
            attention_ = false;
            sense->key = SENSE_UNIT_ATTENTION;
            sense->asc = 0x28;          // not ready to ready change, medium may have changed
            sense->ascq = 0x00;
            return false;
        }
        return true;
    }

    // Read Capacity: false if there is no disk
    bool capacity(uint32_t* blocks, uint16_t* block_size) {
        *block_size = BLOCK_SIZE;
        *blocks = 0;
        if (ejected_ || (!holding_ && !acquire())) {
            return false;
        }
        *blocks = h_.sectors();
        return *blocks != 0;
    }

    // Read10 / Write10 for part of a transfer: bytes from the start of block lba plus offset.
    // Returns the number of bytes done or -1 (and sense says why).
    int32_t read10(uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t bytes, Sense* sense) {
        return transfer(false, lba, offset, buf, nullptr, bytes, sense);
    }
    int32_t write10(uint32_t lba, uint32_t offset, const uint8_t* buf, uint32_t bytes, Sense* sense) {
        return transfer(true, lba, offset, nullptr, buf, bytes, sense);
    }

    // Start Stop Unit: the PC ejects the disk (start = false, load_eject = true): the card is
    // given back to the Pico and the drive stays empty until the cable is plugged in again
    bool start_stop(bool start, bool load_eject) {
        if (load_eject) {
            if (!start) {
                ejected_ = true;
                release();
            }
            // loading is not supported: plug the cable in again
        }
        return true;
    }

    // The PC is gone (cable pulled, PC asleep): the Pico has its card back. If the PC returns
    // it is told the disk may have changed.
    void on_suspend() { release(); }

    // The PC has configured the device: a new connection, so an ejected drive is back
    void on_mount() {
        ejected_ = false;
        release();
    }

    // Bus reset, or the configuration lost
    void on_umount() { release(); }

private:
    Hooks h_;
    bool holding_ = false;
    bool ejected_ = false;
    bool attention_ = false;
    uint32_t last_activity_ = 0;

    static bool no_medium(Sense* s) {
        s->key = SENSE_NOT_READY;
        s->asc = 0x3A;                  // medium not present
        s->ascq = 0x00;
        return false;
    }

    // Takes the card for the PC, if the Pico is not using it and there is one
    bool acquire() {
        if (!h_.pico_idle() || !h_.card_ready()) {
            return false;
        }
        h_.give();
        holding_ = true;
        attention_ = true;
        last_activity_ = h_.now_ms();
        return true;
    }

    void release() {
        if (holding_) {
            holding_ = false;
            h_.take_back();
        }
    }

    int32_t transfer(bool write, uint32_t lba, uint32_t offset, uint8_t* rbuf, const uint8_t* wbuf,
                     uint32_t bytes, Sense* sense) {
        if (ejected_ || (!holding_ && !acquire())) {
            no_medium(sense);
            return -1;
        }
        if (attention_) {
            // The PC has not seen that the disk changed: tell it before anything is read or
            // written from what it knew of the old one
            attention_ = false;
            sense->key = SENSE_UNIT_ATTENTION;
            sense->asc = 0x28;
            sense->ascq = 0x00;
            return -1;
        }
        // whole blocks only (TinyUSB's buffer is a multiple of the block size)
        if (offset % BLOCK_SIZE != 0 || bytes % BLOCK_SIZE != 0 || bytes == 0) {
            sense->key = SENSE_ILLEGAL_REQUEST;
            sense->asc = 0x24;          // invalid field in CDB
            sense->ascq = 0x00;
            return -1;
        }
        uint32_t first = lba + offset / BLOCK_SIZE;
        uint32_t count = bytes / BLOCK_SIZE;
        uint32_t total = h_.sectors();
        if (first >= total || count > total - first) {
            sense->key = SENSE_ILLEGAL_REQUEST;
            sense->asc = 0x21;          // logical block address out of range
            sense->ascq = 0x00;
            return -1;
        }
        int rc = write ? h_.write(wbuf, first, count) : h_.read(rbuf, first, count);
        last_activity_ = h_.now_ms();
        if (rc != 0) {
            sense->key = SENSE_MEDIUM_ERROR;
            sense->asc = write ? 0x0C : 0x11;   // write error / unrecovered read error
            sense->ascq = 0x00;
            return -1;
        }
        return (int32_t) bytes;
    }
};

}   // namespace usbmsc

#endif
