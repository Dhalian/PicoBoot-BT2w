/**
 * controller_config.c
 *
 * See controller_config.h for the overall design. Two SDK details worth
 * flagging up front:
 *
 * 1. Flash offset: FLASH_CONFIG_OFFSET below is chosen to sit well past
 *    both the firmware region (512 KB, see memmap_picoboot.ld) and the
 *    injected-payload region (which that same linker script caps at
 *    2 MB total). Pico 2 W boards ship with 4 MB of flash, so starting
 *    our table at the 3 MB mark leaves a large, untouched buffer on
 *    both sides -- no collision with anything the linker script places.
 *
 * 2. Multicore safety: this project runs core 1 continuously (Bluetooth)
 *    once boot completes, and profile writes happen live (when a
 *    controller connects, mid-game). Erasing/programming flash while the
 *    other core might be executing code *from* flash is unsafe. We use
 *    the Pico SDK's flash_safe_execute(), which pauses the other core
 *    for the duration of the write (core 0 already calls
 *    multicore_lockout_victim_init() in gamecube_task(), which is what
 *    makes it pausable this way).
 */

#include "controller_config.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

// 3 MB into flash -- see design note above. 32 slots * 4KB = 128KB used.
#define FLASH_CONFIG_OFFSET (3u * 1024 * 1024)
#define FLASH_CONFIG_SLOT_SIZE FLASH_SECTOR_SIZE // 4096 bytes, one erase unit
#define FLASH_CONFIG_MAGIC 0x50494346u            // "PICF"

typedef struct {
    uint32_t magic; // FLASH_CONFIG_MAGIC if this slot holds a valid profile
    controller_profile_t profile;
    uint32_t crc32;
    // Remaining bytes up to FLASH_CONFIG_SLOT_SIZE are left as 0xFF
    // (flash erased state) and never touched.
} slot_record_t;

_Static_assert(sizeof(slot_record_t) <= FLASH_CONFIG_SLOT_SIZE,
               "slot_record_t must fit in one flash sector");

static const uint8_t* slot_ptr(int slot)
{
    return (const uint8_t*)(XIP_BASE + FLASH_CONFIG_OFFSET + (uint32_t)slot * FLASH_CONFIG_SLOT_SIZE);
}

// Small, self-contained CRC32 (no dependency on any external lib).
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

static bool slot_is_valid(int slot, slot_record_t* out)
{
    memcpy(out, slot_ptr(slot), sizeof(slot_record_t));
    if (out->magic != FLASH_CONFIG_MAGIC) {
        return false;
    }
    uint32_t expected_crc = out->crc32;
    out->crc32 = 0;
    uint32_t actual_crc = crc32_compute(out, sizeof(slot_record_t));
    return actual_crc == expected_crc;
}

// --- flash_safe_execute() callback context/trampoline ---
typedef struct {
    int      slot;
    uint8_t  buffer[FLASH_CONFIG_SLOT_SIZE];
} write_ctx_t;

static void write_slot_unsafe(void* param)
{
    write_ctx_t* ctx = (write_ctx_t*)param;
    uint32_t offset = FLASH_CONFIG_OFFSET + (uint32_t)ctx->slot * FLASH_CONFIG_SLOT_SIZE;
    flash_range_erase(offset, FLASH_CONFIG_SLOT_SIZE);
    flash_range_program(offset, ctx->buffer, FLASH_CONFIG_SLOT_SIZE);
}

static bool write_slot(int slot, const slot_record_t* record)
{
    static write_ctx_t ctx; // static: keep it off the stack, this is sizeable
    memset(&ctx, 0xFF, sizeof(ctx));
    ctx.slot = slot;
    memcpy(ctx.buffer, record, sizeof(slot_record_t));

    // 1000ms timeout to let the other core reach a safe pause point.
    int rc = flash_safe_execute(write_slot_unsafe, &ctx, 1000);
    return rc == PICO_OK;
}

void controller_config_init(void)
{
    // Nothing to do at startup: slots are read directly from flash (XIP)
    // on demand, no RAM-resident index to build.
}

static bool find_slot_for_mac(const uint8_t mac[6], int* out_slot, slot_record_t* out_record)
{
    for (int i = 0; i < CONTROLLER_CONFIG_MAX_SLOTS; i++) {
        slot_record_t rec;
        if (slot_is_valid(i, &rec) && memcmp(rec.profile.mac, mac, 6) == 0) {
            *out_slot = i;
            *out_record = rec;
            return true;
        }
    }
    return false;
}

static bool find_free_slot(int* out_slot)
{
    for (int i = 0; i < CONTROLLER_CONFIG_MAX_SLOTS; i++) {
        slot_record_t rec;
        if (!slot_is_valid(i, &rec)) {
            *out_slot = i;
            return true;
        }
    }
    return false;
}

static uint8_t next_free_gc_port(void)
{
    bool used[4] = {false, false, false, false};
    for (int i = 0; i < CONTROLLER_CONFIG_MAX_SLOTS; i++) {
        slot_record_t rec;
        if (slot_is_valid(i, &rec) && rec.profile.gc_port < 4) {
            used[rec.profile.gc_port] = true;
        }
    }
    for (uint8_t p = 0; p < 4; p++) {
        if (!used[p]) return p;
    }
    return 0; // all taken (shouldn't happen, max 4 devices anyway)
}

bool controller_config_get_or_create(const uint8_t mac[6], const char* model_name,
                                      controller_profile_t* out)
{
    int slot;
    slot_record_t rec;

    if (find_slot_for_mac(mac, &slot, &rec)) {
        *out = rec.profile;
        return true;
    }

    // Not found -- create a new profile with defaults.
    memset(out, 0, sizeof(*out));
    memcpy(out->mac, mac, 6);
    if (model_name) {
        strncpy(out->model_name, model_name, CONTROLLER_CONFIG_MODEL_NAME_LEN - 1);
    }
    out->gc_port = next_free_gc_port();
    out->remap_a = REMAP_DEFAULT;
    out->remap_b = REMAP_DEFAULT;
    out->remap_x = REMAP_DEFAULT;
    out->remap_y = REMAP_DEFAULT;

    if (!find_free_slot(&slot)) {
        // Table full -- profile is still usable for this session, it
        // just won't be remembered on next connect.
        return false;
    }

    slot_record_t new_rec = {0};
    new_rec.magic = FLASH_CONFIG_MAGIC;
    new_rec.profile = *out;
    new_rec.crc32 = 0;
    new_rec.crc32 = crc32_compute(&new_rec, sizeof(new_rec));

    return write_slot(slot, &new_rec);
}

bool controller_config_save(const controller_profile_t* profile)
{
    int slot;
    slot_record_t rec;

    if (!find_slot_for_mac(profile->mac, &slot, &rec)) {
        return false;
    }

    rec.profile = *profile;
    rec.crc32 = 0;
    rec.crc32 = crc32_compute(&rec, sizeof(rec));

    return write_slot(slot, &rec);
}

int controller_config_list(controller_profile_t* out, int max_count)
{
    int count = 0;
    for (int i = 0; i < CONTROLLER_CONFIG_MAX_SLOTS && count < max_count; i++) {
        slot_record_t rec;
        if (slot_is_valid(i, &rec)) {
            out[count] = rec.profile;
            count++;
        }
    }
    return count;
}
