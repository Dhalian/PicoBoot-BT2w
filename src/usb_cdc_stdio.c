/**
 * usb_cdc_stdio.c
 *
 * A minimal replacement for pico_stdio_usb's printf-over-USB glue.
 * We can't use pico_stdio_usb directly anymore since it comes with its
 * own fixed (CDC-only) USB descriptors, and we need our own composite
 * CDC+MSC descriptor set (see usb_descriptors.c) for the config drive.
 * This plugs into the Pico SDK's generic stdio_driver_t interface the
 * same way pico_stdio_usb does internally, just writing/reading through
 * our own tud_cdc_* calls instead.
 */

#include "pico/stdio/driver.h"
#include "tusb.h"

static void usb_cdc_stdio_out_chars(const char* buf, int len)
{
    if (!tud_cdc_connected()) return;

    for (int i = 0; i < len;) {
        int n = tud_cdc_write(buf + i, (uint32_t)(len - i));
        if (n <= 0) {
            tud_task();
            continue;
        }
        i += n;
    }
    tud_cdc_write_flush();
}

static int usb_cdc_stdio_in_chars(char* buf, int len)
{
    if (!tud_cdc_connected() || !tud_cdc_available()) return PICO_ERROR_NO_DATA;
    return (int)tud_cdc_read(buf, (uint32_t)len);
}

static stdio_driver_t usb_cdc_stdio_driver = {
    .out_chars = usb_cdc_stdio_out_chars,
    .in_chars = usb_cdc_stdio_in_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = PICO_STDIO_DEFAULT_CRLF,
#endif
};

void usb_cdc_stdio_init(void)
{
    tusb_init();
    stdio_set_driver_enabled(&usb_cdc_stdio_driver, true);
}
