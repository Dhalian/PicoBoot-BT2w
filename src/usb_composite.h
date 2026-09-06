#ifndef USB_COMPOSITE_H
#define USB_COMPOSITE_H

// Initializes TinyUSB and registers our custom stdio-over-CDC driver.
// Call this once, in place of stdio_init_all(), before using printf().
void usb_cdc_stdio_init(void);

// Prepares the in-memory disk image (msc_disk.c). Call once at boot,
// any time before the first MSC read could occur.
void msc_disk_init(void);

#endif // USB_COMPOSITE_H
