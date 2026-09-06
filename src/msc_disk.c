/**
 * msc_disk.c
 *
 * STEP 3 of the USB config-drive feature: files are now WRITABLE. Edit a
 * .cfg file's "port" or "remap" fields in a text editor and save -- the
 * change gets parsed and persisted to the controller's flash profile
 * immediately.
 *
 * This is a small hand-written JSON *field* extractor, not a general
 * JSON parser: it looks for the exact "port" and "remap" keys our own
 * generator produces and reads their values, ignoring everything else.
 * That's deliberate -- a real parser would be a lot more code for very
 * little benefit here, since the only thing writing these files is
 * either us or a human editing our own generated format.
 *
 * Known limitation: only writes that land at sector offset 0 are
 * processed (i.e. the whole file rewritten in one write, which is what
 * text editors do for files this small). A write split across multiple
 * calls at different offsets is simply accepted without effect.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tusb.h"
#include "controller_config.h"

#define DISK_BLOCK_SIZE 512
#define MAX_FILES 16 // matches the 16 root-directory entries declared in the boot sector below
#define DISK_BLOCK_NUM (3 + MAX_FILES) // boot + FAT + root dir + one data cluster per possible file

typedef struct {
    char     short_name[11]; // 8.3 padded, no dot
    uint8_t  mac[6];         // which controller this file represents
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

static int8_t parse_remap_value(const char* val)
{
    if (strcmp(val, "A") == 0) return REMAP_BUTTON_A;
    if (strcmp(val, "B") == 0) return REMAP_BUTTON_B;
    if (strcmp(val, "X") == 0) return REMAP_BUTTON_X;
    if (strcmp(val, "Y") == 0) return REMAP_BUTTON_Y;
    return REMAP_DEFAULT; // "default" or anything unrecognized
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

// Renders a profile's JSON content into s_files[file_index] and updates
// the matching root directory entry's size field. Does NOT touch the
// FAT chain or short name -- those are fixed at creation time.
static void refresh_file_content(int file_index, const controller_profile_t* profile)
{
    int len = snprintf(s_files[file_index].content, sizeof(s_files[file_index].content),
        "{\r\n"
        "  \"mac\": \"%02X:%02X:%02X:%02X:%02X:%02X\",\r\n"
        "  \"model\": \"%s\",\r\n"
        "  \"port\": %d,\r\n"
        "  \"remap\": {\"a\": \"%s\", \"b\": \"%s\", \"x\": \"%s\", \"y\": \"%s\"}\r\n"
        "}\r\n",
        profile->mac[0], profile->mac[1], profile->mac[2],
        profile->mac[3], profile->mac[4], profile->mac[5],
        profile->model_name,
        profile->gc_port + 1, // shown as 1-based P1..P4
        remap_name(profile->remap_a), remap_name(profile->remap_b),
        remap_name(profile->remap_x), remap_name(profile->remap_y));

    if (len < 0) len = 0;
    if ((size_t)len >= sizeof(s_files[file_index].content)) len = sizeof(s_files[file_index].content) - 1;
    s_files[file_index].size = (uint32_t)len;

    uint8_t* entry = &s_root_dir[file_index * 32];
    memcpy(&entry[28], &s_files[file_index].size, 4);
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
        memcpy(s_files[i].mac, profiles[i].mac, 6);

        int cluster = 2 + i;
        fat12_set_entry(s_fat_table, cluster, 0xFFF); // single-cluster file, end-of-chain

        uint8_t* entry = &s_root_dir[i * 32];
        memcpy(entry, s_files[i].short_name, 11);
        entry[11] = 0x20; // Attribute: archive (writable)
        entry[26] = (uint8_t)(cluster & 0xFF);
        entry[27] = (uint8_t)((cluster >> 8) & 0xFF);

        refresh_file_content(i, &profiles[i]);
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

// Extracts a top-level "key": value integer from a small JSON blob.
// Returns the value, or 'fallback' if the key wasn't found/parseable.
static int json_extract_int(const char* json, const char* key, int fallback)
{
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return fallback;
    p = strchr(p, ':');
    if (!p) return fallback;
    p++;
    while (*p == ' ') p++;
    return atoi(p);
}

// Extracts a "key": "string value" from a JSON blob, searching only
// within the first 'scope_len' bytes of 'json' (so remap's "a"/"b"/"x"/"y"
// keys don't accidentally match something unrelated elsewhere).
static bool json_extract_string_scoped(const char* json, size_t scope_len, const char* key,
                                        char* out, size_t out_size)
{
    char pattern[8];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    size_t pattern_len = strlen(pattern);

    for (size_t i = 0; i + pattern_len <= scope_len; i++) {
        if (memcmp(json + i, pattern, pattern_len) != 0) continue;

        const char* p = json + i + pattern_len;
        const char* end = json + scope_len;
        while (p < end && *p != ':') p++;
        if (p >= end) return false;
        p++;
        while (p < end && *p == ' ') p++;
        if (p >= end || *p != '"') return false;
        p++;

        size_t n = 0;
        while (p < end && *p != '"' && n < out_size - 1) {
            out[n++] = *p++;
        }
        out[n] = '\0';
        return true;
    }
    return false;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize)
{
    (void)lun;

    int file_index = (int)lba - 3;
    if (file_index >= 0 && file_index < s_file_count && offset == 0) {
        char content[DISK_BLOCK_SIZE + 1];
        size_t len = bufsize;
        if (len > DISK_BLOCK_SIZE) len = DISK_BLOCK_SIZE;
        memcpy(content, buffer, len);
        content[len] = '\0';

        // Only bother if this actually looks like our JSON (cheap sanity
        // check -- ignores stray writes from filesystem housekeeping).
        if (strstr(content, "\"port\"") != NULL) {
            controller_profile_t profile;
            if (controller_config_get_or_create(s_files[file_index].mac, NULL, &profile)) {
                int port_1based = json_extract_int(content, "port", profile.gc_port + 1);
                if (port_1based >= 1 && port_1based <= 4) {
                    profile.gc_port = (uint8_t)(port_1based - 1);
                }

                const char* remap_obj = strstr(content, "\"remap\"");
                if (remap_obj) {
                    size_t remap_scope_len = strlen(remap_obj);
                    char val[16];
                    if (json_extract_string_scoped(remap_obj, remap_scope_len, "a", val, sizeof(val)))
                        profile.remap_a = parse_remap_value(val);
                    if (json_extract_string_scoped(remap_obj, remap_scope_len, "b", val, sizeof(val)))
                        profile.remap_b = parse_remap_value(val);
                    if (json_extract_string_scoped(remap_obj, remap_scope_len, "x", val, sizeof(val)))
                        profile.remap_x = parse_remap_value(val);
                    if (json_extract_string_scoped(remap_obj, remap_scope_len, "y", val, sizeof(val)))
                        profile.remap_y = parse_remap_value(val);
                }

                if (controller_config_save(&profile)) {
                    printf("Config drive: saved changes for %02X:%02X:%02X:%02X:%02X:%02X "
                           "(port=P%d, remap a=%s b=%s x=%s y=%s)\n",
                        profile.mac[0], profile.mac[1], profile.mac[2],
                        profile.mac[3], profile.mac[4], profile.mac[5],
                        profile.gc_port + 1,
                        remap_name(profile.remap_a), remap_name(profile.remap_b),
                        remap_name(profile.remap_x), remap_name(profile.remap_y));
                    refresh_file_content(file_index, &profile);
                } else {
                    printf("Config drive: failed to save changes (flash write error)\n");
                }
            }
        }
    }

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
