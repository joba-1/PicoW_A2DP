# Host unit tests for the hardware independent parts
# cmake -S test -B build-test && cmake --build build-test && ctest --test-dir build-test --output-on-failure

cmake_minimum_required(VERSION 3.13...3.31)
project(picow-a2dp-test C)
set(CMAKE_C_STANDARD 11)

if (DEFINED ENV{PICO_SDK_PATH})
    set(BTSTACK_DEFAULT $ENV{PICO_SDK_PATH}/lib/btstack)
else ()
    set(BTSTACK_DEFAULT ${CMAKE_CURRENT_LIST_DIR}/../../pico-sdk/lib/btstack)
endif ()
set(BTSTACK_ROOT ${BTSTACK_DEFAULT} CACHE PATH "btstack sources, default from PICO_SDK_PATH")

file(GLOB SBC_CODEC_SOURCES
    ${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/srce/*.c
    ${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/srce/*.c
)

add_executable(test_host
    test_host.c
    ${BTSTACK_ROOT}/src/classic/btstack_sbc_bluedroid.c
    ${BTSTACK_ROOT}/src/classic/btstack_sbc_plc.c
    ${BTSTACK_ROOT}/src/btstack_util.c
    ${SBC_CODEC_SOURCES}
)

target_include_directories(test_host PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}            # btstack_config.h for the host
    ${CMAKE_CURRENT_LIST_DIR}/../src
    ${BTSTACK_ROOT}/src
    ${BTSTACK_ROOT}/src/classic
    ${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include
    ${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include
)

target_compile_options(test_host PRIVATE -Wall -Wextra)
target_link_libraries(test_host m)

enable_testing()
add_test(NAME test_host COMMAND test_host)

# a2dp receive path simulation with btstack's codec, ring buffer and resampler
add_executable(test_a2dp_sim
    test_a2dp_sim.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/a2dp.c
    ${BTSTACK_ROOT}/src/classic/btstack_sbc_bluedroid.c
    ${BTSTACK_ROOT}/src/classic/btstack_sbc_plc.c
    ${BTSTACK_ROOT}/src/btstack_ring_buffer.c
    ${BTSTACK_ROOT}/src/btstack_resample.c
    ${BTSTACK_ROOT}/src/btstack_audio.c
    ${BTSTACK_ROOT}/src/btstack_util.c
    ${SBC_CODEC_SOURCES}
)
target_include_directories(test_a2dp_sim PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../src
    ${BTSTACK_ROOT}/src
    ${BTSTACK_ROOT}/src/classic
    ${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include
    ${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include
    ${BTSTACK_ROOT}/3rd-party/yxml
)
target_compile_definitions(test_a2dp_sim PRIVATE A2DP_LATENCY_MS=200 PRINT_STATS=1)
target_compile_options(test_a2dp_sim PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_a2dp_sim m)
add_test(NAME test_a2dp_sim COMMAND test_a2dp_sim)
