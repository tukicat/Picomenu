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


// The SD card as a USB drive for a PC (see usb_msc_core.h for how the card is shared)

#ifndef INCLUDED_USB_MSC_H
#define INCLUDED_USB_MSC_H

#include <stdint.h>

// Call once, after tusb_init()
void usb_msc_init();

// Call from the main loop as often as possible: runs TinyUSB
void usb_msc_task();

// True while the PC has been using the card in the last few milliseconds (the main
// loop should not sleep then)
bool usb_msc_busy();

// True while the card is given to the PC: the Pico sees no SD card
bool usb_msc_pc_holds_card();

// Set by the serial command handler while it is working, so the PC is not given the card
// in the middle of a command
void usb_msc_set_command_active(bool active);

// Supplied by picoface.cxx:
//   whether the Pico is using the card (the NMI menu is open or about to open, a tape is
//   being played, ...), and what to do when the card goes to the PC and comes back
bool pico_sd_in_use();
void pico_sd_give_to_pc();
void pico_sd_take_back_from_pc();

#endif
