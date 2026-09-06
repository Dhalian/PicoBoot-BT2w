/**
 * usb_descriptors.c
 *
 * Adapted from TinyUSB's official examples/device/cdc_msc example (the
 * project's own recommended starting point for a composite CDC+MSC
 * device). Declares one CDC interface (our debug console, replacing
 * pico_stdio_usb's built-in one) and one MSC interface (the "USB drive"
 * that will expose controller config files).
 */

#include <stdio.h>
#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"

// Raspberry Pi's own VID, with a distinct PID so this doesn't collide
// with other Pico firmware the host may have seen before.
#define USB_VID 0x2E8A
#define USB_PID 0x000A

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_MSC,
    ITF_NUM_TOTAL
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC,
    STRID_MSC,
};

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,

    // Use Interface Association Descriptor (IAD) for CDC, required for
    // multiple-interface composite devices with a CDC function.
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,

    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,

    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,

    .bNumConfigurations = 0x01
};

uint8_t const* tud_descriptor_device_cb(void)
{
    return (uint8_t const*)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT 0x02
#define EPNUM_CDC_IN 0x82
#define EPNUM_MSC_OUT 0x03
#define EPNUM_MSC_IN 0x83

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

uint8_t const desc_fs_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    // Interface number, string index, EP notif address and size, EP data address (out, in) and size.
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

uint8_t const* tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_fs_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+
static char const* string_desc_arr[] = {
    (const char[]){0x09, 0x04}, // 0: English (0x0409)
    "PicoBoot BT",              // 1: Manufacturer
    "PicoBoot BT Config Drive", // 2: Product
    NULL,                       // 3: Serial -- generated from flash unique ID below
    "PicoBoot BT Debug Console",// 4: CDC interface
    "PicoBoot BT Config",       // 5: MSC interface
};

static uint16_t _desc_str[32 + 1];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chr_count;

    if (index == STRID_LANGID) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else if (index == STRID_SERIAL) {
        // Use the board's unique flash ID as the serial number, same
        // approach pico_stdio_usb uses -- avoids Windows treating every
        // firmware rebuild as a "new" device needing re-enumeration.
        char serial[17];
        pico_unique_board_id_t id;
        pico_get_unique_board_id(&id);
        for (int i = 0; i < 8; i++) {
            snprintf(&serial[i * 2], 3, "%02X", id.id[i]);
        }
        chr_count = strlen(serial);
        for (size_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = serial[i];
        }
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;
        const char* str = string_desc_arr[index];
        chr_count = strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (size_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
