# cd build
# cmake -DPICO_BOARD=pico2_w -DCMAKE_BUILD_TYPE=Debug ..
# make -j

cmake_minimum_required(VERSION 3.27...3.31)

if (NOT DEFINED ENV{PICO_SDK_PATH} AND (NOT PICO_SDK_PATH))
    set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/../pico-sdk)
endif ()

if (NOT DEFINED ENV{PICO_BOARD} AND (NOT PICO_BOARD))
    set(PICO_BOARD pico_w)
endif ()

include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

pico_sdk_init()

# btstack 1.8 (bundled since pico-sdk 2.3.0) changed the btstack_audio interface
if (PICO_SDK_VERSION_STRING VERSION_LESS "2.3.0")
    message(FATAL_ERROR "pico-sdk ${PICO_SDK_VERSION_STRING} is too old, need >= 2.3.0")
endif ()

project(picow-a2dp CXX C ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

add_executable(${PROJECT_NAME})

# names follow the board: pico_w -> picow-a2dp.uf2, PicoW-2.3.1, pico2_w -> pico2w-a2dp.uf2, Pico2W-2.3.1
string(REPLACE "_" "" BOARD_ID "${PICO_BOARD}")
set(FIRMWARE_NAME "${BOARD_ID}-a2dp")
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${FIRMWARE_NAME}")
pico_set_program_name(${PROJECT_NAME} "${FIRMWARE_NAME}")
string(REGEX REPLACE "^pico(2?)_w$" "Pico\\1W" BOARD_LABEL "${PICO_BOARD}")

target_sources(${PROJECT_NAME} PRIVATE
    src/btstack_audio_pico_i2s.c
    src/main.c
    src/bt.c
    src/sdp.c
    src/a2dp.c
    src/avrcp.c
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    PICO_AUDIO_I2S_DATA_PIN=9
    PICO_AUDIO_I2S_CLOCK_PIN_BASE=10  # =BCK, +1=LRCLK
    # RUN_PIN=22
    CONN_PIN=26
    BT_PIN="0000"
    BT_NAME="${BOARD_LABEL}-${PICO_SDK_VERSION_STRING}"
)

target_link_libraries(${PROJECT_NAME}
    pico_stdlib
    pico_stdio_usb  # use usb, not uart for stdio
    # pico_stdio_uart  # use uart, not usb for stdio e.g. via picoprobe
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
