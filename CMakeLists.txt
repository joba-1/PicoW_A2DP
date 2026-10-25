# mkdir build && cd build
# cmake -DPICO_BOARD=pico2_w ..     # or pico_w (default)
# make -j
#
# All tunables below are cache variables, override them with -D<NAME>=<value>

cmake_minimum_required(VERSION 3.13...3.31)

if (NOT DEFINED ENV{PICO_SDK_PATH} AND (NOT PICO_SDK_PATH))
    set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/../pico-sdk)
endif ()

if (NOT DEFINED ENV{PICO_BOARD} AND (NOT PICO_BOARD))
    set(PICO_BOARD pico_w)
endif ()

include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

project(picow-a2dp VERSION 2.0.0 LANGUAGES C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# btstack 1.8 (bundled since pico-sdk 2.3.0) changed the btstack_audio interface
if (PICO_SDK_VERSION_STRING VERSION_LESS "2.3.0")
    message(FATAL_ERROR "pico-sdk ${PICO_SDK_VERSION_STRING} is too old, need >= 2.3.0")
endif ()

# Bluetooth
set(BT_NAME "PicoW A2DP" CACHE STRING "Bluetooth device name, 00:00:00:00:00:00 is replaced by the device address")
set(BT_PIN "0000" CACHE STRING "PIN for legacy pairing (only very old sources use it, others pair via SSP)")

# I2S DAC (e.g. PCM5102)
set(I2S_DATA_PIN 9 CACHE STRING "I2S data GPIO (DIN)")
set(I2S_CLOCK_PIN_BASE 10 CACHE STRING "I2S bit clock GPIO (BCK), LRCK is the next GPIO")

# Optional GPIOs, -1 disables them
set(CONN_PIN 26 CACHE STRING "GPIO driven high while a source is connected (-1: unused)")
set(RUN_PIN -1 CACHE STRING "GPIO wired to RUN to hard reset on fatal errors (-1: unused)")

# Audio
set(A2DP_LATENCY_MS 200 CACHE STRING "Target jitter buffer in ms. Lower: less delay, higher: fewer dropouts")
set(VOLUME_RANGE_DB 50 CACHE STRING "Attenuation in dB at the lowest volume step above mute")
set(VOLUME_DEFAULT 100 CACHE STRING "Volume 0..127 used until a source sets one (then stored in flash)")
option(VOLUME_ABSOLUTE "Let the source control the volume of this sink (AVRCP absolute volume)" ON)

# Robustness and debugging
option(REBOOT_ON_DISCONNECT "Reboot when the source disconnects, for a clean state" ON)
set(WATCHDOG_MS 3000 CACHE STRING "Reboot if the main loop stalls for this long (0: no watchdog)")
option(STDIO_UART "Use uart (e.g. picoprobe) instead of usb for stdio" OFF)
option(PRINT_STATS "Print audio buffer statistics every 10s while streaming" OFF)

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
    src/main.c
    src/bt.c
    src/sdp.c
    src/a2dp.c
    src/avrcp.c
    src/volume.c
    src/btstack_audio_pico_i2s.c
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    VERSION="${PROJECT_VERSION}"
    BT_NAME="${BT_NAME}"
    BT_PIN="${BT_PIN}"
    PICO_AUDIO_I2S_DATA_PIN=${I2S_DATA_PIN}
    PICO_AUDIO_I2S_CLOCK_PIN_BASE=${I2S_CLOCK_PIN_BASE}
    CONN_PIN=${CONN_PIN}
    RUN_PIN=${RUN_PIN}
    A2DP_LATENCY_MS=${A2DP_LATENCY_MS}
    VOLUME_RANGE_DB=${VOLUME_RANGE_DB}
    VOLUME_DEFAULT=${VOLUME_DEFAULT}
    VOLUME_ABSOLUTE=$<BOOL:${VOLUME_ABSOLUTE}>
    REBOOT_ON_DISCONNECT=$<BOOL:${REBOOT_ON_DISCONNECT}>
    WATCHDOG_MS=${WATCHDOG_MS}
    PRINT_STATS=$<BOOL:${PRINT_STATS}>
)

target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-unused-parameter)

if (STDIO_UART)
    pico_enable_stdio_uart(${PROJECT_NAME} 1)
    pico_enable_stdio_usb(${PROJECT_NAME} 0)
else ()
    pico_enable_stdio_uart(${PROJECT_NAME} 0)
    pico_enable_stdio_usb(${PROJECT_NAME} 1)
endif ()

target_link_libraries(${PROJECT_NAME}
    pico_stdlib
    hardware_watchdog
    pico_audio_i2s
    pico_btstack_sbc_decoder
    pico_btstack_classic
    pico_btstack_cyw43
    pico_cyw43_arch_none
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR} # For btstack config
)

pico_add_extra_outputs(${PROJECT_NAME})
