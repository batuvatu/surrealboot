
cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(usbliter8_bootsurreal C CXX ASM)
set(CMAKE_BUILD_TYPE MinSizeRel)
set(BOARD_DEFINITION ${CMAKE_SOURCE_DIR}/boards/${PICO_BOARD}.cmake)
if (NOT EXISTS ${BOARD_DEFINITION})
    set(BOARD_DEFINITION ${CMAKE_SOURCE_DIR}/boards/unknown.cmake)
endif()
include(${BOARD_DEFINITION} OPTIONAL)
if (NOT DEFINED USBLITER8_ENABLE_SHELL)
    add_compile_definitions(WITH_AUTO_MODE=1)
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(FAST_CONTROL_PATCH
    ${CMAKE_SOURCE_DIR}/../../tools/patch_fast_control.py
)
set(DUAL_CLOCK_PATCH
    ${CMAKE_SOURCE_DIR}/../../tools/patch_dualclock.py
)

if (EXISTS ${FAST_CONTROL_PATCH})
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${FAST_CONTROL_PATCH}
                ${CMAKE_SOURCE_DIR}/pio_usb/src/pio_usb_host.c
        RESULT_VARIABLE FAST_CONTROL_PATCH_RC
    )
    if (NOT FAST_CONTROL_PATCH_RC EQUAL 0)
        message(FATAL_ERROR "fast control burst patch failed")
    endif()
endif()

if (EXISTS ${DUAL_CLOCK_PATCH})
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${DUAL_CLOCK_PATCH}
                ${CMAKE_SOURCE_DIR}
        RESULT_VARIABLE DUAL_CLOCK_PATCH_RC
    )
    if (NOT DUAL_CLOCK_PATCH_RC EQUAL 0)
        message(FATAL_ERROR "dual clock patch failed")
    endif()
endif()

pico_sdk_init()
add_executable(usbliter8
    main.c
    led.c
    bus.c
    usb.c
    exploit.c
    surreal_boot.c
    dfu_helper.c
    serial_flasher.c
    generated/bootfiles_data.c
)
pico_set_program_name(usbliter8 "usbliter8-bootsurreal")
pico_set_program_version(usbliter8 "1.0.0")
target_compile_definitions(usbliter8 PRIVATE
    PICO_STACK_SIZE=0x1000
    PICO_PRINTF_SUPPORT_EXPONENTIAL=0
    PICO_PRINTF_SUPPORT_FLOAT=0
    PICO_PRINTF_SUPPORT_PTRDIFF_T=0
)
pico_enable_stdio_uart(usbliter8 0)
pico_enable_stdio_usb(usbliter8 1)
add_subdirectory(pio_usb)
target_link_libraries(usbliter8
    pico_stdlib
    pico_status_led
    hardware_pwm
    hardware_flash
    hardware_sync
    pio_usb
)
target_include_directories(usbliter8 PRIVATE ${CMAKE_SOURCE_DIR}/generated)
pico_add_extra_outputs(usbliter8)
