/**
 * msc_disk.c
 *
 * STEP 2 of the USB config-drive feature: the virtual disk now reflects
 * the REAL controller profiles stored in flash (controller_config.c).
 * Each known controller becomes one .cfg file (JSON content), named
 * after the last 3 bytes of its Bluetooth address (plain FAT 8.3 names
 * only support 8 characters, so the full 6-byte address doesn't fit
 * without also implementing VFAT long filenames).
 *
 * Read-only for now (step 3 will add write support so edits made in a
 * text editor / the future web page get saved back to flash). The
 * snapshot of files is built once, at boot (msc_disk_init) -- a
 * controller that pairs for the first time later in the same session
 * won't show up until the next reboot/replug. Good enough for now;
 * can be revisited later if that turns out to be annoying in practice.
 */

#include <stdio.h>
#include <string.h>

#include "tusb.h"
#include "controller_config.h"

#define DISK_BLOCK_SIZE 512
#define MAX_FILES 16 // matches the 16 root-directory entries declared in the boot sector below
#define DISK_BLOCK_NUM (3 + MAX_FILES) // boot + FAT + root dir + one data cluster per possible file

typedef struct {
    char     short_name[11]; // 8.3 padded, no dot
    uint32_t size;
    char     content[384];
} virtual_file_t;

static virtual_file_t s_files[MAX_FILES];
static int s_file_count = 0;

static uint8_t s_fat_table[DISK_BLOCK_SIZE];
static uint8_t s_root_dir[DISK_BLOCK_SIZE];

// --- Boot sector (BPB) ---
static uint8_t s_boot_sector[DISK_BLOCK_SIZE] = {
    0xEB, 0x3C, 0x90,                         // Jump instruction
    'M', 'S', 'D', 'O', 'S', '5', '.', '0',   // OEM name
    0x00, 0x02,                               // Bytes per sector (512)
    0x01,                                     // Sectors per cluster
    0x01, 0x00,                               // Reserved sectors
    0x01,                                     // Number of FATs (we only serve one)
    0x10, 0x00,                               // Root directory entries (16 -- matches MAX_FILES)
    0x00, 0x00,                               // Total sectors -- filled in at init (may exceed 1 byte)
    0xF8,                                     // Media descriptor (fixed disk)
    0x01, 0x00,                               // Sectors per FAT
    0x01, 0x00,                               // Sectors per track
    0x01, 0x00,                               // Number of heads
    0x00, 0x00, 0x00, 0x00,                   // Hidden sectors
    0x00, 0x00, 0x00, 0x00,                   // Large total sectors (unused)
    0x00,                                     // Drive number
    0x00,                                     // Reserved
    0x29,                                     // Extended boot signature
    0x34, 0x12, 0x00, 0x00,                   // Volume serial number
    'P', 'I', 'C', 'O', 'B', 'O', 'O', 'T', ' ', ' ', ' ', // Volume label (11 bytes)
    'F', 'A', 'T', '1', '2', ' ', ' ', ' ',   // Filesystem type
    [510] = 0x55,
    [511] = 0xAA,
};

static const char* remap_name(int8_t r)
{
    switch (r) {
        case REMAP_BUTTON_A: return "A";
        case REMAP_BUTTON_B: return "B";
        case REMAP_BUTTON_X: return "X";
        case REMAP_BUTTON_Y: return "Y";
        default: return "default";
    }
}

static void build_short_name(const uint8_t mac[6], char out11[11])
{
    static const char hex[] = "0123456789ABCDEF";
    char name8[8];
    // Last 3 bytes of the MAC (the device-specific part, not the
    // manufacturer OUI) as 6 uppercase hex chars, space-padded to 8.
    for (int i = 0; i < 3; i++) {
        name8[i * 2] = hex[(mac[3 + i] >> 4) & 0xF];
        name8[i * 2 + 1] = hex[mac[3 + i] & 0xF];
    }
    name8[6] = ' ';
    name8[7] = ' ';
    memcpy(out11, name8, 8);
    memcpy(out11 + 8, "CFG", 3);
}

// Correct FAT12 entry packing: two 12-bit entries share 3 bytes.
static void fat12_set_entry(uint8_t* fat, int cluster, uint16_t value)
{
    int offset = cluster + (cluster / 2); // == cluster * 3 / 2
    if (cluster % 2 == 0) {
        fat[offset] = (uint8_t)(value & 0xFF);
        fat[offset + 1] = (uint8_t)((fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F));
    } else {
        fat[offset] = (uint8_t)((fat[offset] & 0x0F) | ((value & 0x0F) << 4));
        fat[offset + 1] = (uint8_t)((value >> 4) & 0xFF);
    }
}

void msc_disk_init(void)
{
    controller_config_init();

    // Fill in total-sectors field now that DISK_BLOCK_NUM is known.
    s_boot_sector[19] = (uint8_t)(DISK_BLOCK_NUM & 0xFF);
    s_boot_sector[20] = (uint8_t)((DISK_BLOCK_NUM >> 8) & 0xFF);

    controller_profile_t profiles[MAX_FILES];
    s_file_count = controller_config_list(profiles, MAX_FILES);

    memset(s_fat_table, 0, sizeof(s_fat_table));
    s_fat_table[0] = 0xF8;
    s_fat_table[1] = 0xFF;
    s_fat_table[2] = 0xFF;

    memset(s_root_dir, 0, sizeof(s_root_dir));

    for (int i = 0; i < s_file_count; i++) {
        build_short_name(profiles[i].mac, s_files[i].short_name);

        int len = snprintf(s_files[i].content, sizeof(s_files[i].content),
            "{\r\n"
            "  \"mac\": \"%02X:%02X:%02X:%02X:%02X:%02X\",\r\n"
            "  \"model\": \"%s\",\r\n"
            "  \"port\": %d,\r\n"
            "  \"remap\": {\"a\": \"%s\", \"b\": \"%s\", \"x\": \"%s\", \"y\": \"%s\"}\r\n"
            "}\r\n",
            profiles[i].mac[0], profiles[i].mac[1], profiles[i].mac[2],
            profiles[i].mac[3], profiles[i].mac[4], profiles[i].mac[5],
            profiles[i].model_name,
            profiles[i].gc_port + 1, // shown as 1-based P1..P4
            remap_name(profiles[i].remap_a), remap_name(profiles[i].remap_b),
            remap_name(profiles[i].remap_x), remap_name(profiles[i].remap_y));

        if (len < 0) len = 0;
        if ((size_t)len >= sizeof(s_files[i].content)) len = sizeof(s_files[i].content) - 1;
        s_files[i].size = (uint32_t)len;

        int cluster = 2 + i;
        fat12_set_entry(s_fat_table, cluster, 0xFFF); // single-cluster file, end-of-chain

        uint8_t* entry = &s_root_dir[i * 32];
        memcpy(entry, s_files[i].short_name, 11);
        entry[11] = 0x20; // Attribute: archive (writable -- step 3 will need this)
        entry[26] = (uint8_t)(cluster & 0xFF);
        entry[27] = (uint8_t)((cluster >> 8) & 0xFF);
        memcpy(&entry[28], &s_files[i].size, 4);
    }
}

//--------------------------------------------------------------------+
// MSC callbacks
//--------------------------------------------------------------------+

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    const char vid[] = "PicoBoot";
    const char pid[] = "BT Config Drive";
    const char rev[] = "1.0";
    memcpy(vendor_id, vid, strlen(vid));
    memcpy(product_id, pid, strlen(pid));
    memcpy(product_rev, rev, strlen(rev));
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size)
{
    (void)lun;
    *block_count = DISK_BLOCK_NUM;
    *block_size = DISK_BLOCK_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    (void)start;
    (void)load_eject;
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize)
{
    (void)lun;
    if (lba >= DISK_BLOCK_NUM) return -1;

    static uint8_t sector[DISK_BLOCK_SIZE];
    memset(sector, 0, sizeof(sector));

    if (lba == 0) {
        memcpy(sector, s_boot_sector, DISK_BLOCK_SIZE);
    } else if (lba == 1) {
        memcpy(sector, s_fat_table, DISK_BLOCK_SIZE);
    } else if (lba == 2) {
        memcpy(sector, s_root_dir, DISK_BLOCK_SIZE);
    } else {
        int file_index = (int)lba - 3;
        if (file_index < s_file_count) {
            size_t len = s_files[file_index].size;
            if (len > DISK_BLOCK_SIZE) len = DISK_BLOCK_SIZE;
            memcpy(sector, s_files[file_index].content, len);
        }
        // else: unused cluster, sector stays zero-filled
    }

    memcpy(buffer, sector + offset, bufsize);
    return (int32_t)bufsize;
}

// Still read-only in step 2 -- accept writes silently without storing
// anything, so the OS doesn't think the drive is faulty. Step 3 will
// parse and persist real edits here.
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize)
{
    (void)lun;
    (void)lba;
    (void)offset;
    (void)buffer;
    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize)
{
    (void)lun;
    (void)buffer;
    (void)bufsize;
    (void)scsi_cmd;
    return -1; // Report failure for anything not explicitly handled above.
}
