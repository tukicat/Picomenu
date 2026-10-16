/*
 * Based on tusb_config.h of the Raspberry Pi Pico SDK (pico_stdio_usb).
 * Changed for the Picoface (a serial port and a drive).
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2020 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// TinyUSB configuration: a serial port (CDC, for the PC scripts and the log) and a
// mass storage drive (MSC, the SD card). This replaces the one the pico SDK's
// stdio USB brings, which has only the serial port.

#ifndef _PICOFACE_TUSB_CONFIG_H_
#define _PICOFACE_TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE)

#define CFG_TUD_CDC             (1)
#define CFG_TUD_CDC_RX_BUFSIZE  (256)
#define CFG_TUD_CDC_TX_BUFSIZE  (256)

#define CFG_TUD_MSC             (1)
// one block at a time: the least RAM (the PC sends bigger writes in blocks of this size)
#define CFG_TUD_MSC_EP_BUFSIZE  (512)

#define CFG_TUD_HID             (0)
#define CFG_TUD_MIDI            (0)
#define CFG_TUD_VENDOR          (0)

#ifdef __cplusplus
}
#endif

#endif
