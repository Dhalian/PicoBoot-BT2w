/**
 * tusb_config.h
 *
 * TinyUSB configuration for a composite device: CDC (our existing debug
 * serial console) + MSC (mass storage, exposing controller config files).
 *
 * This REPLACES pico_stdio_usb's own built-in single-CDC descriptors --
 * pico_stdio_usb bakes in its own fixed device/config descriptor set, so
 * it cannot be extended with a second interface. We provide our own
 * descriptors (usb_descriptors.c) and our own tiny stdio driver
 * (usb_cdc_stdio.c) that plugs printf() into this CDC interface instead.
 */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_MCU OPT_MCU_RP2040 // Also correct for RP2350 (Pico SDK aliases this the same way for stdio_usb)
#define CFG_TUSB_OS OPT_OS_PICO
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#define CFG_TUD_ENDPOINT0_SIZE 64

// --- Device classes enabled ---
#define CFG_TUD_CDC 1
#define CFG_TUD_MSC 1
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

// --- CDC buffer sizes ---
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 256
#define CFG_TUD_CDC_EP_BUFSIZE 64

// --- MSC buffer size (one FAT sector) ---
#define CFG_TUD_MSC_EP_BUFSIZE 512

#ifdef __cplusplus
}
#endif

#endif // TUSB_CONFIG_H
