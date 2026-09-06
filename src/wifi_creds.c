/**
 * wifi_creds.c
 *
 * See wifi_creds.h. Same storage pattern as controller_config.c (magic +
 * CRC32 + flash_safe_execute for multicore-safe writes), just a single
 * record instead of a table of slots, living in its own flash sector
 * right after the 32 controller-profile slots (3MB + 128KB = 3.125MB;
 * controller_config.c's 32 slots * 4KB each = 128KB span exactly
 * 3MB-3.125MB, so this sits immediately after with no gap or overlap).
 */

#include "wifi_creds.h"

#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

#define FLASH_WIFI_OFFSET (3u * 1024 * 1024 + 32u * 4096u) // right after controller_config's slots
#define FLASH_WIFI_SLOT_SIZE FLASH_SECTOR_SIZE              // 4096 bytes, one erase unit
#define FLASH_WIFI_MAGIC 0x49464957u                        // "WIFI" (byte-swapped for readability in hex dumps)

typedef struct {
    uint32_t magic;
    char     ssid[WIFI_CREDS_SSID_MAX_LEN];
    char     password[WIFI_CREDS_PASSWORD_MAX_LEN];
    uint32_t crc32;
} wifi_record_t;

_Static_assert(sizeof(wifi_record_t) <= FLASH_WIFI_SLOT_SIZE,
               "wifi_record_t must fit in one flash sector");

static uint32_t crc32_compute(const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));
        }
    }
    return ~crc;
}

static const wifi_record_t* record_ptr(void)
{
    return (const wifi_record_t*)(XIP_BASE + FLASH_WIFI_OFFSET);
}

bool wifi_creds_load(char* ssid, size_t ssid_size, char* password, size_t password_size)
{
    wifi_record_t rec;
    memcpy(&rec, record_ptr(), sizeof(rec));

    if (rec.magic != FLASH_WIFI_MAGIC) return false;

    uint32_t expected_crc = rec.crc32;
    rec.crc32 = 0;
    if (crc32_compute(&rec, sizeof(rec)) != expected_crc) return false;

    strncpy(ssid, rec.ssid, ssid_size - 1);
    ssid[ssid_size - 1] = '\0';
    strncpy(password, rec.password, password_size - 1);
    password[password_size - 1] = '\0';
    return true;
}

typedef struct {
    uint8_t buffer[FLASH_WIFI_SLOT_SIZE];
} write_ctx_t;

static void write_unsafe(void* param)
{
    write_ctx_t* ctx = (write_ctx_t*)param;
    flash_range_erase(FLASH_WIFI_OFFSET, FLASH_WIFI_SLOT_SIZE);
    flash_range_program(FLASH_WIFI_OFFSET, ctx->buffer, FLASH_WIFI_SLOT_SIZE);
}

bool wifi_creds_save(const char* ssid, const char* password)
{
    wifi_record_t rec = {0};
    rec.magic = FLASH_WIFI_MAGIC;
    strncpy(rec.ssid, ssid, sizeof(rec.ssid) - 1);
    strncpy(rec.password, password, sizeof(rec.password) - 1);
    rec.crc32 = 0;
    rec.crc32 = crc32_compute(&rec, sizeof(rec));

    static write_ctx_t ctx; // static: keep it off the stack
    memset(&ctx, 0xFF, sizeof(ctx));
    memcpy(ctx.buffer, &rec, sizeof(rec));

    int rc = flash_safe_execute(write_unsafe, &ctx, 1000);
    return rc == PICO_OK;
}
