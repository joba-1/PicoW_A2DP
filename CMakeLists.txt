# cd build
# cmake ..
# make

cmake_minimum_required(VERSION 3.27...3.27)

if (NOT DEFINED ENV{PICO_SDK_PATH} AND (NOT PICO_SDK_PATH))
    set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/../pico-sdk)
endif ()

if (NOT DEFINED ENV{PICO_BOARD} AND (NOT PICO_BOARD))
    set(PICO_BOARD pico_w)
endif ()

include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

pico_sdk_init()

project(picow-a2dp CXX C ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
    ${PICO_SDK_PATH}/lib/btstack/src/btstack_audio.c
    a2dp_sink_demo.c
    btstack_audio_pico.c
    picow_bt_example_background.c
    picow_bt_example_common.c
    picow_bt_example_common.h
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    I2S_DATA_PIN=9
    I2S_CLOCK_PIN_BASE=10
    BT_PIN="0000"
    BT_NAME="PicoW-A2DB"
)

target_link_libraries(${PROJECT_NAME}
    pico_stdlib
    pico_stdio_uart  # use uart, not usb for stdio e.g. via picoprobe
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
