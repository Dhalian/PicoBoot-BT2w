/**
 * Copyright (c) 2025 Maciej Kobus
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "hardware/gpio.h"
#include "pio.h"
#include "picoboot.pio.h"

void on_transfer_program_init(PIO pio, uint sm, uint offset, uint clk_pin, uint cs_pin, uint out_pin)
{
    pio_sm_config c = on_transfer_program_get_default_config(offset);

    pio_gpio_init(pio, clk_pin);
    pio_gpio_init(pio, cs_pin);

    // RP2350 errata E9: the internal pull-down leaks enough current that a
    // pin relying on it can read as ~2.1V ("weak low") instead of a clean
    // logic 0. CS/CLK are actively driven push-pull by the GameCube, so no
    // internal pull is needed here at all -- disable both (up and down) to
    // remove any ambiguity on the PIO's input threshold. Harmless on RP2040.
    gpio_set_pulls(clk_pin, false, false);
    gpio_set_pulls(cs_pin, false, false);

    sm_config_set_jmp_pin(&c, cs_pin);
    sm_config_set_in_pins(&c, cs_pin);

    // Set CS pin as input
    pio_sm_set_consecutive_pindirs(pio, sm, cs_pin, 1, false);

    // Shift to right, autopull with threshold 32
    sm_config_set_out_shift(&c, false, true, 32);

    // Run at full system clock
    sm_config_set_clkdiv(&c, 1.f);

    // Load configuration and jump to start of the program
    pio_sm_init(pio, sm, offset, &c);
}

void clocked_output_program_init(PIO pio, uint sm, uint offset, uint data_pin, uint clk_pin, uint cs_pin)
{
    pio_sm_config c = clocked_output_program_get_default_config(offset);

    pio_gpio_init(pio, clk_pin);
    pio_gpio_init(pio, cs_pin);

    pio_gpio_init(pio, data_pin);

    // See on_transfer_program_init(): disable internal pulls on all three
    // pins so we don't rely on the RP2350's broken pull-down (errata E9)
    // while CS/CLK are being read and DI is still floating/input.
    gpio_set_pulls(clk_pin, false, false);
    gpio_set_pulls(cs_pin, false, false);
    gpio_set_pulls(data_pin, false, false);

    sm_config_set_jmp_pin(&c, clk_pin);
    sm_config_set_in_pins(&c, cs_pin);

    // Out and Set pins have to overlap so we can make line floating (=set it as input)
    sm_config_set_out_pins(&c, data_pin, 1);
    sm_config_set_set_pins(&c, data_pin, 1);

    // Set CS and CLK as inputs
    pio_sm_set_consecutive_pindirs(pio, sm, cs_pin, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, clk_pin, 1, false);

    pio_sm_set_consecutive_pindirs(pio, sm, data_pin, 1, false);

    // Shift to right, autopull with threshold 32
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

    // Run at full system clock
    sm_config_set_clkdiv(&c, 1.f);
    //

    pio_sm_init(pio, sm, offset, &c);
}
