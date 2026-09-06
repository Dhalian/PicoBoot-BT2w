cmake_minimum_required(VERSION 3.13)

if(DEFINED PICO_BOARD AND NOT ${PICO_BOARD} STREQUAL "pico2_w")
    message(FATAL_ERROR "PICO_BOARD must be set to 'pico2_w' or not set at all")
else()
    set(PICO_BOARD "pico2_w")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

include(cmake/extract_version.cmake)

include(pico_sdk_import.cmake)

project(picoboot LANGUAGES C CXX ASM VERSION ${PROJECT_VERSION_STRING})

set(BLUEPAD32_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/bluepad32)

set(PICO_BTSTACK_PATH ${PICO_SDK_PATH}/lib/btstack)

pico_sdk_init()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/dist)

add_executable(picoboot
        src/picoboot.c
        src/hw.c
        src/pio.c
        src/status_led.c
        src/status_led/gpio.c
        src/status_led/cyw43.c
        src/gamecube.c
        src/intercore.c
        src/my_platform.c
        src/controller_config.c
        src/usb_descriptors.c
        src/usb_cdc_stdio.c
        src/msc_disk.c
        src/json_util.c
        src/http_server.c
        src/wifi_creds.c
)

pico_generate_pio_header(picoboot
        ${CMAKE_CURRENT_LIST_DIR}/src/picoboot.pio
)

pico_generate_pio_header(picoboot
        ${CMAKE_CURRENT_LIST_DIR}/src/joybus.pio
)

pico_set_program_name(picoboot "PicoBoot BT 2W")
pico_set_program_description(picoboot "RP2350 based modchip for Nintendo GameCube with Bluetooth gamepad support")
pico_set_program_version(picoboot ${FW_VER_STRING})
pico_set_program_url(picoboot "https://github.com/webhdx/PicoBoot")

target_link_options(pico_standard_link INTERFACE "LINKER:--script=${CMAKE_CURRENT_LIST_DIR}/memmap_picoboot.ld")

pico_enable_stdio_uart(picoboot 0)

target_include_directories(picoboot PRIVATE
        src
        ${CMAKE_CURRENT_LIST_DIR}
        ${BLUEPAD32_ROOT}/src/components/bluepad32/include)

include_directories(picoboot src)
include_directories(${PICO_BTSTACK_PATH}/src)

target_link_libraries(picoboot PRIVATE
        hardware_adc
        hardware_dma
        hardware_pio
        hardware_flash
        pico_flash
        pico_unique_id
        tinyusb_device
        pico_cyw43_arch_lwip_threadsafe_background
        pico_btstack_classic
        pico_btstack_cyw43
        pico_multicore
        pico_stdlib
        bluepad32
)

add_subdirectory(${BLUEPAD32_ROOT}/src/components/bluepad32 libbluepad32)

include_directories(picoboot src)

pico_add_extra_outputs(picoboot)
