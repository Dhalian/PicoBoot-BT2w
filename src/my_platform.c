// Example file - Public Domain
// Need help? https://tinyurl.com/bluepad32-help

#include <stddef.h>
#include <string.h>

#include <btstack_run_loop.h>
#include <pico/cyw43_arch.h>
#include <pico/time.h>
#include <uni.h>
#include <pico/multicore.h>

#include "gamecube.h"
#include "types.h"

#include "sdkconfig.h"
#include "intercore.h"
#include "controller_config.h"

// Bluepad32 v4.x removed the "safe_platform_hook" callback from the platform
// struct. Periodic code on the BT thread (core 1) is now scheduled with a
// BTstack run-loop timer instead.
#define RUMBLE_TIMER_MS 8

// Sanity check
#ifndef CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#error "Pico W must use BLUEPAD32_PLATFORM_CUSTOM"
#endif

typedef struct
{
    // Set this to keep a controller connection alive
    bool keep_alive;
    interval_s timeout_interval;
    bool connected;
    bool led_set;
    uni_hid_device_t *device_ptr;
    uint8_t gc_port;   // which GameCube port (0=P1..3=P4) this connection drives
    int8_t  remap_a;   // per-controller button remap, loaded from its saved profile
    int8_t  remap_b;
    int8_t  remap_x;
    int8_t  remap_y;
} my_playform_player_s;

my_playform_player_s _players[4] = {0};

static btstack_timer_source_t rumble_timer;
static void my_platform_bt_loop_hook(btstack_timer_source_t *ts);

// MY_PLATFORM.C FUNCTIONS RUN ON CORE 1
//
// Platform Overrides
//
static void my_platform_init(int argc, const char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    logi("my_platform: init()\n");

    controller_config_init();

    // No global button mapping here anymore -- each controller gets its
    // own remap (defaulting to identity/no swap) from its saved profile,
    // applied per-device in my_platform_on_controller_data(). This is
    // what makes different controllers able to have different mappings.
}


static void my_platform_on_init_complete(void) {
    logi("my_platform: on_init_complete()\n");

    // Safe to call "unsafe" functions since they are called from BT thread

    // Start scanning
    uni_bt_enable_new_connections_unsafe(true);

    // Keep previously-paired controllers' link keys so they can
    // auto-reconnect on power-up instead of requiring the user to
    // re-pair from scratch every single boot. (Previously this
    // unconditionally called uni_bt_del_keys_unsafe(), wiping all
    // bonds every time.)
    uni_bt_list_keys_unsafe();

    // Turn off LED once init is done.
    //cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);

    //    uni_bt_service_set_enabled(true);

    uni_property_dump_all();

    // Schedule periodic rumble processing on this (BT) thread.
    btstack_run_loop_set_timer_handler(&rumble_timer, my_platform_bt_loop_hook);
    btstack_run_loop_set_timer(&rumble_timer, RUMBLE_TIMER_MS);
    btstack_run_loop_add_timer(&rumble_timer);
}

// Fallback label when the controller never reports a readable name over
// Bluetooth (observed with some Xbox controllers -- d->name stays empty
// even after on_device_ready). controller_type is set independently of
// that, based on VID/PID recognition, so it's a more reliable source.
// Only covers the two Xbox variants that are already used elsewhere in
// this file (proven to compile in this codebase) -- anything else just
// falls back to "Unknown controller" as before.
static const char* controller_type_name(uni_hid_device_t* d)
{
    switch (d->controller_type) {
        case k_eControllerType_XBox360Controller: return "Xbox 360 Controller";
        case k_eControllerType_XBoxOneController: return "Xbox One / Series Controller";
        default: return NULL;
    }
}

static void my_platform_on_device_connected(uni_hid_device_t* d) {
    //logi("my_platform: device connected: %p\n", d);
    uint8_t idx = uni_hid_device_get_idx_for_instance(d);
    static intercore_msg_s core1playermsg = {.id = IC_MSG_CONNECT};

    switch(idx)
    {
        case 0 ... 3:
        {
            controller_profile_t profile;
            const char* model_name = (d->name[0] != '\0') ? d->name : controller_type_name(d);
            if (!model_name) model_name = "Unknown controller";
            controller_config_get_or_create(d->conn.btaddr, model_name, &profile);

            _players[idx].connected = true;
            _players[idx].device_ptr = d;
            _players[idx].led_set = false;
            _players[idx].gc_port = profile.gc_port;
            _players[idx].remap_a = profile.remap_a;
            _players[idx].remap_b = profile.remap_b;
            _players[idx].remap_x = profile.remap_x;
            _players[idx].remap_y = profile.remap_y;

            core1playermsg.data = _players[idx].gc_port;
            core0_send_message_safe(&core1playermsg);
        }
        break;

        default:
        // Do nothing for other cases
        break;
    }
}

static void my_platform_on_device_disconnected(uni_hid_device_t* d) {
    //logi("my_platform: device connected: %p\n", d);
    uint8_t idx = uni_hid_device_get_idx_for_instance(d);
    static intercore_msg_s core1playermsg = {.id = IC_MSG_DISCONNECT};

    switch(idx)
    {
        case 0 ... 3:
            core1playermsg.data = _players[idx].gc_port;
            core0_send_message_safe(&core1playermsg);
            _players[idx].connected = false;
            _players[idx].led_set = false;
            _players[idx].device_ptr = NULL;
        break;

        default:
        // Do nothing for other cases
        break;

    }
}

static uni_error_t my_platform_on_device_ready(uni_hid_device_t* d) {
    //logi("my_platform: device ready: %p\n", d);

    // Fix up a profile that was stamped "Unknown controller" at connect
    // time (name/type not resolved yet back then) now that we know more.
    const char* resolved_name = (d->name[0] != '\0') ? d->name : controller_type_name(d);
    if (resolved_name) {
        controller_profile_t profile;
        if (controller_config_get_or_create(d->conn.btaddr, resolved_name, &profile)) {
            if (strncmp(profile.model_name, "Unknown controller", sizeof(profile.model_name)) == 0
                || profile.model_name[0] == '\0') {
                strncpy(profile.model_name, resolved_name, CONTROLLER_CONFIG_MODEL_NAME_LEN - 1);
                profile.model_name[CONTROLLER_CONFIG_MODEL_NAME_LEN - 1] = '\0';
                controller_config_save(&profile);
            }
        }
    }

    // You can reject the connection by returning an error.
    return UNI_ERROR_SUCCESS;
}

void _my_platform_process_rumble(uint8_t gc_port, bool rumble)
{
    bool state = rumble;

    // gc_port (which GameCube controller port, 0=P1..3=P4) is no longer
    // guaranteed to be the same number as the Bluepad32 connection slot
    // (a saved profile can reassign a controller to a different port),
    // so look up which _players[] entry currently drives this port.
    int slot = -1;
    for (int i = 0; i < 4; i++) {
        if (_players[i].connected && _players[i].gc_port == gc_port) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return;

    uni_hid_device_t *d = _players[slot].device_ptr;

    // Short-circuit (||) is required here: if the player isn't connected,
    // 'd' may be NULL (never connected) or a dangling pointer to a device
    // Bluepad32 has already freed/reused (just disconnected). The original
    // bitwise '|' evaluated both sides unconditionally, dereferencing 'd'
    // even when it was invalid.
    if (!_players[slot].connected || d == NULL || d->report_parser.play_dual_rumble == NULL) return;

    if(state)
    d->report_parser.play_dual_rumble(d, 0, 32, 128, 40);
    else
    d->report_parser.play_dual_rumble(d, 0, 0, 0, 0);
}

static void my_platform_on_controller_data(uni_hid_device_t* d, uni_controller_t* ctl) {
    uni_gamepad_t* gp;

    uint8_t idx = uni_hid_device_get_idx_for_instance(d);
    uint8_t gc_port = _players[idx].gc_port;

    if(!_players[idx].led_set && (d->report_parser.set_player_leds!=NULL))
    {
        // Set player LEDs -- lit according to the GameCube port, not the
        // raw Bluepad32 connection slot, so it matches what's printed on
        // the console screen (P1/P2/P3/P4).
        if(d->report_parser.set_player_leds != NULL)
        {
            d->report_parser.set_player_leds(d, ((1<<gc_port)&0xF));
            _players[idx].led_set = true;
        }
        
    }

    // Send input message if it's a gamepad of valid type
    switch (ctl->klass) {
        case UNI_CONTROLLER_CLASS_GAMEPAD:

            gp = &ctl->gamepad;

            // Per-controller button remap, loaded from this device's saved
            // profile (controller_config.h). Defaults to identity (no
            // change) unless the profile says otherwise -- this replaces
            // both the old global mapping and the old Xbox-specific swap,
            // since either controller-brand-based approach can't give two
            // different controllers two different mappings.
            {
                bool physical[4];
                physical[REMAP_BUTTON_A] = gp->buttons & BUTTON_A;
                physical[REMAP_BUTTON_B] = gp->buttons & BUTTON_B;
                physical[REMAP_BUTTON_X] = gp->buttons & BUTTON_X;
                physical[REMAP_BUTTON_Y] = gp->buttons & BUTTON_Y;

                int8_t ra = _players[idx].remap_a;
                int8_t rb = _players[idx].remap_b;
                int8_t rx = _players[idx].remap_x;
                int8_t ry = _players[idx].remap_y;

                gp->buttons &= ~(BUTTON_A | BUTTON_B | BUTTON_X | BUTTON_Y);
                gp->buttons |= physical[ra == REMAP_DEFAULT ? REMAP_BUTTON_A : ra] ? BUTTON_A : 0;
                gp->buttons |= physical[rb == REMAP_DEFAULT ? REMAP_BUTTON_B : rb] ? BUTTON_B : 0;
                gp->buttons |= physical[rx == REMAP_DEFAULT ? REMAP_BUTTON_X : rx] ? BUTTON_X : 0;
                gp->buttons |= physical[ry == REMAP_DEFAULT ? REMAP_BUTTON_Y : ry] ? BUTTON_Y : 0;
            }

            // Send intercore message for input update
            static intercore_msg_s inputmsg = {.id = IC_MSG_INPUT};
            static uint8_t inputcounter = 0; // Increment inputs
            
            inputmsg.gp.axis_x          = gp->axis_x;
            inputmsg.gp.axis_y          = gp->axis_y;
            inputmsg.gp.axis_rx         = gp->axis_rx;
            inputmsg.gp.axis_ry         = gp->axis_ry;
            inputmsg.gp.dpad            = gp->dpad;
            inputmsg.gp.brake           = gp->brake;
            inputmsg.gp.throttle        = gp->throttle;
            inputmsg.gp.buttons         = gp->buttons;
            inputmsg.gp.misc_buttons    = gp->misc_buttons;

            inputmsg.data = (gc_port&3) | (inputcounter<<2);
            core0_send_message_safe(&inputmsg);
            inputcounter = (inputcounter+1) % 0b111111;

            break;
        case UNI_CONTROLLER_CLASS_BALANCE_BOARD:
            // Do something
            uni_balance_board_dump(&ctl->balance_board);
            break;
        case UNI_CONTROLLER_CLASS_MOUSE:
            // Do something
            uni_mouse_dump(&ctl->mouse);
            break;
        case UNI_CONTROLLER_CLASS_KEYBOARD:
            // Do something
            uni_keyboard_dump(&ctl->keyboard);
            break;
        default:
            loge("Unsupported controller class: %d\n", ctl->klass);
            break;
    }

}

static const uni_property_t* my_platform_get_property(uni_property_idx_t idx) {
    ARG_UNUSED(idx);
    return NULL;
}

static void my_platform_on_oob_event(uni_platform_oob_event_t event, void* data) {
    return;
    
    switch (event) {
        case UNI_PLATFORM_OOB_GAMEPAD_SYSTEM_BUTTON:
            // Optional: do something when "system" button gets pressed.
            break;

        case UNI_PLATFORM_OOB_BLUETOOTH_ENABLED:
            // When the "bt scanning" is on / off. Could be triggered by different events
            // Useful to notify the user
            //logi("my_platform_on_oob_event: Bluetooth enabled: %d\n", (bool)(data));
            break;

        default:
            //logi("my_platform_on_oob_event: unsupported event: 0x%04x\n", event);
            break;
    }
}

static void my_platform_bt_loop_hook(btstack_timer_source_t *ts)
{
    static intercore_msg_s core1msg = {0};
    static uint8_t _rumble_status = 0;

    if(core1_get_message_safe(&core1msg))
    {
        switch(core1msg.id)
        {
            default:
            break;

            case IC_MSG_RUMBLE:
                _rumble_status = core1msg.data & 0b1111;
            break;
        }
    }

    _my_platform_process_rumble(0, (_rumble_status & 0b1000));
    _my_platform_process_rumble(1, (_rumble_status & 0b100));
    _my_platform_process_rumble(2, (_rumble_status & 0b10));
    _my_platform_process_rumble(3, (_rumble_status & 0b1));

    // Re-schedule so this hook keeps running on the BT thread.
    btstack_run_loop_set_timer(ts, RUMBLE_TIMER_MS);
    btstack_run_loop_add_timer(ts);
}

//
// Entry Point
//
struct uni_platform* get_my_platform(void) {
    static struct uni_platform plat = {
        .name = "My Platform",
        .init = my_platform_init,
        .on_init_complete = my_platform_on_init_complete,
        .on_device_connected = my_platform_on_device_connected,
        .on_device_disconnected = my_platform_on_device_disconnected,
        .on_device_ready = my_platform_on_device_ready,
        .on_oob_event = my_platform_on_oob_event,
        .on_controller_data = my_platform_on_controller_data,
        .get_property = my_platform_get_property
    };

    return &plat;
}
