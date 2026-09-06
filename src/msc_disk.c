/**
 * msc_disk.c
 *
 * STEP 1 of the USB config-drive feature: a tiny, entirely static,
 * read-only FAT12 disk image held in RAM, containing a single test
 * file. The goal of this step is only to confirm the Pico enumerates
 * as a USB drive on Windows *at all*, before wiring in the real
 * per-controller config files from controller_config.c.
 *
 * This is adapted from TinyUSB's own examples/device/cdc_msc/src/msc_disk.c
 * (the project's reference implementation for exactly this kind of
 * minimal in-memory FAT12 volume).
 */

#include <string.h>

#include "tusb.h"

#define DISK_BLOCK_NUM 16   // 16 * 512 bytes = 8KB volume -- plenty for a few small text files later
#define DISK_BLOCK_SIZE 512

static const char readme_contents[] =
    "PicoBoot BT - Controller Config Drive\r\n"
    "======================================\r\n"
    "\r\n"
    "This is step 1 of the USB config drive: this file is here only to\r\n"
    "confirm the drive itself shows up correctly on your PC. Per-controller\r\n"
    ".cfg files will appear here in a later step.\r\n";

// --- Boot sector (BPB) ---
static uint8_t const boot_sector[DISK_BLOCK_SIZE] = {
    0xEB, 0x3C, 0x90,                         // Jump instruction
    'M', 'S', 'D', 'O', 'S', '5', '.', '0',   // OEM name
    0x00, 0x02,                               // Bytes per sector (512)
    0x01,                                     // Sectors per cluster
    0x01, 0x00,                               // Reserved sectors
    0x02,                                     // Number of FATs
    0x10, 0x00,                               // Root directory entries (16)
    DISK_BLOCK_NUM, 0x00,                     // Total sectors (16)
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
    // Remaining bytes are zero-filled up to offset 510, then boot signature.
    [510] = 0x55,
    [511] = 0xAA,
};

// --- FAT12 table (sector 1): reserved clusters 0/1, then a chain for
// README.TXT (starts at cluster 2, spans as many clusters as it needs).
static uint8_t fat_table[DISK_BLOCK_SIZE] = {
    0xF8, 0xFF, 0xFF, // Cluster 0/1 reserved (media descriptor echoed)
    0xFF, 0x0F,       // Cluster 2 (README.TXT) marked as end-of-chain
};

// --- Root directory (sector 2): one entry for README.TXT ---
static uint8_t root_dir[DISK_BLOCK_SIZE] = {0};

static void root_dir_init(void)
{
    // 8.3 filename: "README  TXT"
    memcpy(&root_dir[0], "README  TXT", 11);
    root_dir[11] = 0x01;              // Attribute: read-only
    // Bytes 12-25: reserved/timestamps, left zero.
    root_dir[26] = 2;                 // Starting cluster (low byte) = 2
    root_dir[27] = 0;                 // Starting cluster (high byte)
    uint32_t size = sizeof(readme_contents) - 1; // exclude the C string's null terminator
    memcpy(&root_dir[28], &size, 4);  // File size (little-endian)
}

void msc_disk_init(void)
{
    root_dir_init();
}

//--------------------------------------------------------------------+
// MSC callbacks
//--------------------------------------------------------------------+

// Invoked when received SCSI INQUIRY -- fill vendor/product/revision strings.
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

// Invoked to check whether the device is ready (always true -- no removable media logic needed).
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    return true;
}

// Invoked when received SCSI_CMD_READ_CAPACITY_10.
void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size)
{
    (void)lun;
    *block_count = DISK_BLOCK_NUM;
    *block_size = DISK_BLOCK_SIZE;
}

// Invoked when received Start Stop Unit command (eject request etc.) -- nothing to do.
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    (void)start;
    (void)load_eject;
    return true;
}

// Invoked when received READ10 command.
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize)
{
    (void)lun;
    if (lba >= DISK_BLOCK_NUM) return -1;

    uint8_t const* src;
    if (lba == 0) {
        src = boot_sector;
    } else if (lba == 1) {
        src = fat_table;
    } else if (lba == 2) {
        src = root_dir;
    } else if (lba == 3) {
        // File data cluster -- copy from readme_contents, zero-padded to fill the sector.
        static uint8_t data_sector[DISK_BLOCK_SIZE];
        memset(data_sector, 0, sizeof(data_sector));
        size_t len = sizeof(readme_contents) - 1;
        if (len > DISK_BLOCK_SIZE) len = DISK_BLOCK_SIZE;
        memcpy(data_sector, readme_contents, len);
        src = data_sector;
    } else {
        static uint8_t zero_sector[DISK_BLOCK_SIZE] = {0};
        src = zero_sector;
    }

    memcpy(buffer, src + offset, bufsize);
    return (int32_t)bufsize;
}

// Invoked when received WRITE10 command. Step 1 is read-only: report success
// without actually storing anything (the OS may still probe with a write
// during mount, e.g. to update a "last accessed" timestamp -- silently
// accepting avoids scaring the OS into thinking the drive is faulty).
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize)
{
    (void)lun;
    (void)lba;
    (void)offset;
    (void)buffer;
    return (int32_t)bufsize;
}

// Invoked when received an SCSI command not handled by any other callback.
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize)
{
    (void)lun;
    (void)buffer;
    (void)bufsize;

    void const* response = NULL;
    int32_t resplen = 0;

    switch (scsi_cmd[0]) {
        default:
            // Report a failure for anything we don't explicitly handle.
            resplen = -1;
            break;
    }

    return resplen;
}
