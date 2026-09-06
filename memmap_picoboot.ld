MEMORY
{
    /*
     * Shrink flash region to make space for the payload.
     * 1.5MiB is plenty, and probably bigger than the original BS2 which we are constrained by anyway.
     * This results in `warning: redeclaration of memory region `FLASH'`.
     * Safe to ignore.
     *
     * Bumped from 512k to 768k: the composite USB (CDC+MSC), the JSON
     * config-file parser, and flash-backed controller_config storage
     * pushed the firmware past the original 512k. gekkoboot itself is
     * only tens of KB, so PAYLOAD still has huge headroom even after
     * this increase, and it stays far below the 3MB mark where
     * controller_config.c's own flash region starts (see FLASH_CONFIG_OFFSET
     * in controller_config.c) -- no collision.
     */
    FLASH(rx) : ORIGIN = 0x10000000, LENGTH = 768k
    PAYLOAD(r) : ORIGIN = ORIGIN(FLASH) + LENGTH(FLASH), LENGTH = 2048k - LENGTH(FLASH)
}

__payload = ORIGIN(PAYLOAD);
__payload_end = __payload + LENGTH(PAYLOAD);
