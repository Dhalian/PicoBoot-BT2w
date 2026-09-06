/**
 * Copyright (c) 2025 Maciej Kobus
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <string.h>
#include <tusb.h>

#include <btstack_run_loop.h>
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/structs/bus_ctrl.h"
#include <pico/cyw43_arch.h>
#include <pico/multicore.h>
#include <pico/stdlib.h>
#include <uni.h>

#include "controller_config.h"
#include "endian.h"
#include "gamecube.h"
#include "hw.h"
#include "picoboot.pio.h"
#include "pio.h"
#include "status_led.h"
#include "usb_composite.h"
#include "version.h"

struct uni_platform* get_my_platform(void);
static void bluepad_core_task(void)
{
    // Lets flash_safe_execute() (used by controller_config.c) pause THIS
    // core when a write/erase is triggered from core 0 (e.g. the "forget"
    // serial command). Without this, only writes triggered from core 1
    // itself (e.g. on controller connect) could succeed -- flash_safe_execute
    // would just time out trying to pause an unregistered core.
    multicore_lockout_victim_init();

    if (cyw43_arch_init()) {
        loge("failed to initialise cyw43_arch\n");
        return;
    }

    uni_platform_set_custom(get_my_platform());

    uni_init(0, NULL);

    btstack_run_loop_execute();
}

// Tiny serial command console, typed over the same USB debug console used
// for logs (e.g. via PuTTY). Lets you inspect/remove stored controller
// profiles without waiting for the (still read-only) USB config drive.
//   list             -- print every stored profile
//   forget XXXXXX    -- erase the profile whose MAC ends with these 6 hex
//                       chars (the same 6 chars shown in its .cfg filename)
static void process_serial_commands(void)
{
    static char line[32];
    static size_t line_len = 0;

    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\r' || c == '\n') {
            if (line_len > 0) {
                line[line_len] = '\0';

                if (strcmp(line, "list") == 0) {
                    controller_profile_t profiles[CONTROLLER_CONFIG_MAX_SLOTS];
                    int n = controller_config_list(profiles, CONTROLLER_CONFIG_MAX_SLOTS);
                    printf("Stored controller profiles (%d):\n", n);
                    for (int i = 0; i < n; i++) {
                        printf("  %02X:%02X:%02X:%02X:%02X:%02X  port=P%d  model=%s\n",
                            profiles[i].mac[0], profiles[i].mac[1], profiles[i].mac[2],
                            profiles[i].mac[3], profiles[i].mac[4], profiles[i].mac[5],
                            profiles[i].gc_port + 1, profiles[i].model_name);
                    }
                } else if (strncmp(line, "forget ", 7) == 0 && strlen(line) >= 13) {
                    unsigned int b0, b1, b2;
                    if (sscanf(line + 7, "%2x%2x%2x", &b0, &b1, &b2) == 3) {
                        uint8_t suffix[3] = {(uint8_t)b0, (uint8_t)b1, (uint8_t)b2};
                        printf("Parsed suffix: %02X %02X %02X (from input: \"%s\")\n",
                            suffix[0], suffix[1], suffix[2], line + 7);
                        bool deleted = controller_config_delete_by_suffix(suffix);
                        printf(deleted ? "Profile forgotten.\n" : "No matching profile found.\n");
                    } else {
                        printf("Usage: forget XXXXXX (6 hex chars, e.g. forget 25AD66)\n");
                    }
                } else {
                    printf("Unknown command. Try: list | forget XXXXXX\n");
                }

                line_len = 0;
            }
        } else if (line_len < sizeof(line) - 1) {
            line[line_len++] = (char)c;
        }
    }
}

static void gamecube_task(void)
{
    multicore_lockout_victim_init();
    while (1) {
        // Service USB (our debug console + the config drive) from core 0,
        // interleaved with GameCube controller polling. tud_task() must
        // only ever be called from one place -- this replaces the
        // automatic background IRQ that pico_stdio_usb used to install.
        tud_task();
        process_serial_commands();
        gamecube_comms_task();
    }
}

extern const uint32_t __payload[];
extern const uint32_t __payload_end[];

static const uint32_t payload_magic0 = 0x49504C42; // "IPLB"
static const uint32_t payload_magic1 = 0x4F4F5420; // "OOT "
static const uint32_t payload_magic2 = 0x5049434F; // "PICO"

static hw_board_type_t s_board_type;

size_t validate_payload() {
    if (BigEndian32(__payload[0]) != payload_magic0) {
        goto bad;
    }
    if (BigEndian32(__payload[1]) != payload_magic1) {
        goto bad;
    }

    size_t size = BigEndian32(__payload[2]) / sizeof(__payload[0]);
    size_t alignment = 1024 / sizeof(__payload[0]);
    size_t size_aligned = (size + alignment - 1) / alignment * alignment;

    if (&__payload[size_aligned] > __payload_end) {
        goto bad;
    }

    if (BigEndian32(__payload[size - 1]) != payload_magic2) {
        goto bad;
    }

    return size_aligned;

bad:
    return SIZE_MAX;
}

void main()
{
    // ---- Time-critical section starts here ----
    //
    // Arm the injection PIO/DMA before touching stdio_init_all() (USB)
    // or adc_init() (board detection) -- see PicoBoot v0.3.1, which never
    // showed the "boots to stock menu" bug and does the same thing.

    size_t payload_size = validate_payload();
    if (payload_size == SIZE_MAX) {
        usb_cdc_stdio_init();
        adc_init();
        s_board_type = hw_detect_board_type();
        printf("PicoBoot: Invalid payload. Entering infinite loop.\n");
        status_led_init(s_board_type);

        while (true) {
            sleep_ms(500);
            status_led_toggle();
        }
    }

    // Set 250MHz clock to get more cycles in between CLK pulses.
    // This is the lowest value I was able to make the code work.
    // Should be still considered safe for most Pico boards.
    set_sys_clock_khz(250000, true);

    // Prioritize DMA engine as it does the most work
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    gpio_set_slew_rate(PIN_DI, GPIO_SLEW_RATE_FAST);
    gpio_set_drive_strength(PIN_DI, GPIO_DRIVE_STRENGTH_12MA);

    PIO pio = pio0;

    //
    // State Machine: Transfer Start
    //
    // Counts all consecutive transfers and sets IRQ
    // when first 1 kilobyte transfer starts.
    //

    uint transfer_start_sm = pio_claim_unused_sm(pio, true);
    uint transfer_start_offset = pio_add_program(pio, &on_transfer_program);

    on_transfer_program_init(pio, transfer_start_sm, transfer_start_offset, PIN_CLK, PIN_CS, PIN_DI);

    pio_sm_put(pio, transfer_start_sm, (uint32_t) 224); // CS pulses
    pio_sm_exec(pio, transfer_start_sm, pio_encode_pull(true, true));
    pio_sm_exec(pio, transfer_start_sm, pio_encode_mov(pio_x, pio_osr));
    pio_sm_exec(pio, transfer_start_sm, pio_encode_out(pio_null, 32));

    //
    // State Machine: Clocked Output
    //
    // It waits for IRQ signal from first SM and samples clock signal
    // to output IPL data bits.
    //

    uint clocked_output_sm = pio_claim_unused_sm(pio, true);
    uint clocked_output_offset = pio_add_program(pio, &clocked_output_program);

    clocked_output_program_init(pio, clocked_output_sm, clocked_output_offset, PIN_DI, PIN_CLK, PIN_CS);

    pio_sm_put(pio, clocked_output_sm, 8191); // 8192 bits, 1024 bytes, minus 1 because counting starts from 0
    pio_sm_exec(pio, clocked_output_sm, pio_encode_pull(true, true));
    pio_sm_exec(pio, clocked_output_sm, pio_encode_mov(pio_y, pio_osr));
    pio_sm_exec(pio, clocked_output_sm, pio_encode_out(pio_null, 32));

    // Set up DMA for reading IPL to PIO FIFO
    int chan = dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config(chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio, clocked_output_sm, true));
    channel_config_set_bswap(&c, true);

    dma_channel_configure(
        chan,
        &c,
        &pio->txf[clocked_output_sm],
        __payload,
        payload_size,
        true // start immediately
    );

    // Start PIO state machines. Earliest point the console can be
    // intercepted -- everything below is no longer time-critical.
    pio_sm_set_enabled(pio, transfer_start_sm, true);
    pio_sm_set_enabled(pio, clocked_output_sm, true);

    // ---- Time-critical section ends here ----

    usb_cdc_stdio_init();
    msc_disk_init();
    adc_init();
    s_board_type = hw_detect_board_type();

    printf("PicoBoot (%s) by webhdx (c) 2025\n", FW_VER_STRING);
    printf("Board Type: %s\n", hw_board_type_to_string(s_board_type));
    printf("PicoBoot: Finished injecting payload.\n");

    // ---- Bluepad32 / GameCube Bluetooth glue (ends here at boot) ----

    // Let the console complete the IPL injection before the PIO/DMA engines
    // are torn down and reclaimed for the joybus emulation.
    sleep_ms(800);

    pio_sm_set_enabled(pio0, transfer_start_sm, false);
    pio_sm_set_enabled(pio0, clocked_output_sm, false);
    pio_sm_unclaim(pio0, transfer_start_sm);
    pio_sm_unclaim(pio0, clocked_output_sm);
    pio_clear_instruction_memory(pio0);

    if (s_board_type == HW_BOARD_TYPE_PICO_2_W) {
        // Core 1 runs Bluepad32/BTstack (init will turn on the W LED).
        multicore_launch_core1(bluepad_core_task);

        // Core 0 stays on joybus / GameCube emulation.
        gamecube_task();
    } else {
        status_led_init(s_board_type);
        status_led_on();

        while (true) {
            tight_loop_contents();
        }
    }
}
