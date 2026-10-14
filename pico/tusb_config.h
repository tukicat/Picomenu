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
