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


// The SD card as a USB drive for a PC: TinyUSB's mass storage callbacks.
// The sharing rules are in usb_msc_core.h.

#include "usb_msc.h"
#include "usb_msc_core.h"

#include <string.h>
#include "tusb.h"
#include "pico/time.h"
#include "ff.h"
#include "diskio.h"

static bool command_active = false;

void usb_msc_set_command_active(bool active) {
    command_active = active;
}

static bool hook_pico_idle() {
    return !command_active && !pico_sd_in_use();
}

static bool hook_card_ready() {
    // starts the card if it is not (a card put in since power on is found this way too)
    if ((disk_status(0) & STA_NOINIT) != 0) {
        if ((disk_initialize(0) & STA_NOINIT) != 0) {
            return false;
        }
    }
    return (disk_status(0) & (STA_NOINIT | STA_NODISK)) == 0;
}

static uint32_t hook_sectors() {
    LBA_t n = 0;
    if (disk_ioctl(0, GET_SECTOR_COUNT, &n) != RES_OK) {
        return 0;
    }
    return (uint32_t) n;
}

static int hook_read(uint8_t* buf, uint32_t lba, uint32_t count) {
    return disk_read(0, buf, lba, count) == RES_OK ? 0 : -1;
}

static int hook_write(const uint8_t* buf, uint32_t lba, uint32_t count) {
    return disk_write(0, buf, lba, count) == RES_OK ? 0 : -1;
}

static uint32_t hook_now_ms() {
    return to_ms_since_boot(get_absolute_time());
}

static const usbmsc::Hooks hooks = {
    hook_pico_idle, hook_card_ready, hook_sectors, hook_read, hook_write,
    pico_sd_give_to_pc, pico_sd_take_back_from_pc, hook_now_ms,
};
static usbmsc::Core core(hooks);

void usb_msc_init() {
}

void usb_msc_task() {
    tud_task();
}

bool usb_msc_busy() {
    return core.busy();
}

bool usb_msc_pc_holds_card() {
    return core.holds_card();
}

// ---- TinyUSB callbacks

static void set_sense(uint8_t lun, const usbmsc::Sense& s) {
    tud_msc_set_sense(lun, s.key, s.asc, s.ascq);
}

extern "C" {

void tud_mount_cb(void) {
    core.on_mount();
}

void tud_umount_cb(void) {
    core.on_umount();
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void) remote_wakeup_en;
    core.on_suspend();
}

void tud_resume_cb(void) {
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void) lun;
    memcpy(vendor_id, "PicoMenu", 8);
    memcpy(product_id, "SD card         ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    usbmsc::Sense s;
    if (core.test_unit_ready(&s)) {
        return true;
    }
    set_sense(lun, s);
    return false;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size) {
    (void) lun;
    core.capacity(block_count, block_size);
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void) lun;
    (void) power_condition;
    return core.start_stop(start, load_eject);
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    usbmsc::Sense s;
    int32_t n = core.read10(lba, offset, (uint8_t*) buffer, bufsize, &s);
    if (n < 0) {
        set_sense(lun, s);
    }
    return n;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    usbmsc::Sense s;
    int32_t n = core.write10(lba, offset, buffer, bufsize, &s);
    if (n < 0) {
        set_sense(lun, s);
    }
    return n;
}

// Commands TinyUSB does not know: the ones that need no more than "done"
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize) {
    (void) buffer;
    (void) bufsize;
    switch (scsi_cmd[0]) {
    case 0x35:      // SYNCHRONIZE CACHE (10): the writes are made at once
    case 0x1E:      // PREVENT ALLOW MEDIUM REMOVAL
        return 0;
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);     // invalid command
        return -1;
    }
}

}   // extern "C"
