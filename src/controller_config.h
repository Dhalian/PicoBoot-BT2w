/**
 * controller_config.h
 *
 * Persistent per-controller configuration, stored directly in the Pico's
 * own onboard flash (no SD card involved -- the Pico has no wiring to the
 * GameCube's SD card reader).
 *
 * Storage scheme: a small fixed table of slots living at a flash offset
 * far away from both the firmware and the injected IPL payload (see the
 * comment above FLASH_CONFIG_OFFSET below). Each slot is exactly one
 * flash sector (4096 bytes -- the minimum erase granularity on RP2350),
 * holding one controller's profile. This is deliberately NOT a general
 * filesystem (no littlefs/FAT dependency): fewer moving parts, nothing
 * that can fail to fetch/build, and easy to reason about. A later step
 * can translate these slots into individual virtual files for a
 * USB-exposed view -- the on-disk struct is designed with that in mind
 * (fixed-size, self-describing, one slot = one controller).
 */
#ifndef CONTROLLER_CONFIG_H
#define CONTROLLER_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define CONTROLLER_CONFIG_MAX_SLOTS 32
#define CONTROLLER_CONFIG_MODEL_NAME_LEN 32

// -1 in any remap field means "no override, use the default (identity)
// mapping" -- i.e. physical button A stays A, B stays B, etc.
typedef struct {
    uint8_t mac[6];                              // Bluetooth address -- the unique ID for this controller
    char    model_name[CONTROLLER_CONFIG_MODEL_NAME_LEN]; // human-readable, e.g. "Xbox Series X" (informational only)
    uint8_t gc_port;                             // 0=P1, 1=P2, 2=P3, 3=P4
    int8_t  remap_a;                             // which physical button feeds logical A (-1 = A)
    int8_t  remap_b;                             // which physical button feeds logical B (-1 = B)
    int8_t  remap_x;                             // which physical button feeds logical X (-1 = X)
    int8_t  remap_y;                             // which physical button feeds logical Y (-1 = Y)
} controller_profile_t;

// Physical button identifiers usable in the remap_* fields above.
#define REMAP_DEFAULT -1
#define REMAP_BUTTON_A 0
#define REMAP_BUTTON_B 1
#define REMAP_BUTTON_X 2
#define REMAP_BUTTON_Y 3

// Must be called once at boot (after multicore_launch_core1, since it may
// need to briefly pause core1 to write flash safely) before using any
// other function below.
void controller_config_init(void);

// Finds the stored profile for this MAC address. If none exists yet,
// creates one with default values (next free GameCube port, no button
// remap) and persists it to flash before returning it. 'model_name' is
// only used when creating a brand new profile (purely informational).
// Returns false on unrecoverable storage error (profile fields are then
// left at safe defaults: port 0, no remap).
bool controller_config_get_or_create(const uint8_t mac[6], const char* model_name,
                                      controller_profile_t* out);

// Persists changes to an existing profile (e.g. after a user edits it).
// 'profile->mac' identifies which slot to update; the slot must already
// exist (created via controller_config_get_or_create).
// Returns false if the MAC isn't found or on a storage error.
bool controller_config_save(const controller_profile_t* profile);

#endif // CONTROLLER_CONFIG_H
