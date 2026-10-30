# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

# set(CMAKE_C_STANDARD 11)
# set(CMAKE_CXX_STANDARD 17)
# set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NEVER EDIT THE NEXT LINES for Raspberry Pi Pico VS Code Extension to work ==
# if(WIN32)
#    set(USERHOME $ENV{USERPROFILE})
# else()
#    set(USERHOME $ENV{HOME})
# endif()
# set(sdkVersion 1.5.1)
# set(toolchainVersion 13_2_Rel1)
# set(picotoolVersion 2.0.0)
# include(${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
# ====================================================================================

set(WIFI_ENABLE 0 CACHE BOOL "1: the Pico W build (F - File transfer over WiFi in Tools); its own uf2")
set(PICOMENU_LOG 0 CACHE STRING "1: the firmware sends its log messages on the USB serial port (about 6 KB more)")

# The Pico W build: the WiFi chip's driver and firmware and lwIP are about 300 KB, so the
# program may be bigger, and the internal flash's file system (littlefs) is smaller: 1400 KB
# from the end of the flash, not 1800 KB. The network's name and password are in Picomenu.ini.
if("${WIFI_ENABLE}")
        message(STATUS "Pico W build: WiFi file transfer")
        set(PICO_BOARD pico_w CACHE STRING "Board type" FORCE)
        # Bluetooth too (B - Bluetooth tape in Tools): BTstack as an A2DP source, with the SBC codec
        set(WIFI_LIBS pico_cyw43_arch_lwip_poll pico_lwip_netbios pico_btstack_classic pico_btstack_cyw43 pico_btstack_sbc_encoder)
        set(WIFI_SOURCES wifi.cxx webpage.cxx bt.cxx)
        set(PICOMENU_FS_KB 1400)
        # BTstack keeps the speaker's pairing in 8 KB of the flash, just before the file system
        set(PICOMENU_RESERVE_KB 8)
else()
        message(STATUS "Pico build (no WiFi)")
        set(PICO_BOARD pico CACHE STRING "Board type" FORCE)
        set(WIFI_SOURCES)
        set(PICOMENU_FS_KB 1800)
        set(PICOMENU_RESERVE_KB 0)
endif()


# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

if (PICO_SDK_VERSION_STRING VERSION_LESS "1.4.0")
  message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.4.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

project(picoface C CXX ASM)

# Size. Everything is built for size, -Os, which is much smaller than the SDK's usual -O3 for a
# Release build (about 45 KB for the libraries, about 55 KB for picoface.cxx). The Z80 bus is served by piohandler.S, which is assembly.
set(CMAKE_C_FLAGS_RELEASE "-Os -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG")
set(CMAKE_ASM_FLAGS_RELEASE "-Os -DNDEBUG")

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

add_subdirectory(littlefs-lib)
add_subdirectory(FatFs_SPI)


add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/nmirom.bin ${CMAKE_CURRENT_BINARY_DIR}/nmirom.sym
        COMMAND sjasmplus --sym=${CMAKE_CURRENT_BINARY_DIR}/nmirom.sym ${CMAKE_CURRENT_SOURCE_DIR}/../nmirom/nmirom.asm
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../nmirom/nmirom.asm
)


add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/nmi.h ${CMAKE_CURRENT_BINARY_DIR}/nmi.c
        COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/asm_to_c_file.py
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/nmirom.bin ${CMAKE_CURRENT_BINARY_DIR}/nmirom.sym ${CMAKE_CURRENT_SOURCE_DIR}/asm_to_c_file.py
)


# Add executable. Default name is the project name, version 0.1

add_executable(picoface 
        picoface.cxx
        lz4.c
        ${WIFI_SOURCES}
        usb_command_handler.cxx
        usb_msc.cxx
        usb_descriptors.c
        file_wrapper.cxx
        sd_config.c
        tzxconv_pico.cxx
        z80/Z80.c
        piohandler.S
	${CMAKE_CURRENT_BINARY_DIR}/nmi.c
)
        
pico_set_program_name(picoface "picoface")
pico_set_program_version(picoface "0.1")

# Generate PIO header
pico_generate_pio_header(picoface ${CMAKE_CURRENT_LIST_DIR}/picoface.pio)

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(picoface 0)
pico_enable_stdio_usb(picoface 1)

# Add the standard library to the build
target_link_libraries(picoface
        pico_stdlib
        tinyusb_device)

# Add the standard include files to the build
target_include_directories(picoface PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/z80/API       # the Z80 emulator of the tape converter
  ${CMAKE_CURRENT_LIST_DIR}/z80/zeta
  ${CMAKE_CURRENT_BINARY_DIR} # 🌟 CRITICAL: Allows C++ files to find generated nmi headers naturally!
  ${CMAKE_CURRENT_LIST_DIR}/.. # for our common lwipopts or any other standard includes, if required
)


if("${WIFI_ENABLE}")
        # CYW43_HOST_NAME: the name the router shows; the read-ahead of the SD card is smaller (RAM)
        target_compile_definitions(picoface PRIVATE WIFI_ENABLE CYW43_HOST_NAME=\"PicoMenu\" READAHEAD_SECTORS=2)
        # BTstack's store (the pairing): the 8 KB before the file system, never the file system
        # itself (the SDK's own place is the end of the flash, which is the file system's). Core 1
        # (the Z80 bus) runs from RAM, so it need not be stopped while the flash is written, as
        # littlefs does it.
        math(EXPR BANK_OFFSET "(2048 - ${PICOMENU_FS_KB} - ${PICOMENU_RESERVE_KB}) * 1024")
        target_compile_definitions(picoface PRIVATE PICO_FLASH_BANK_STORAGE_OFFSET=${BANK_OFFSET}
                PICO_FLASH_BANK_TOTAL_SIZE=8192 PICO_FLASH_ASSUME_CORE1_SAFE=1)
        # The Bluetooth's variables share the last 16 KB of snap_load_buffer (memmap_picomenu_w.ld)
        pico_set_linker_script(picoface ${CMAKE_CURRENT_LIST_DIR}/memmap_picomenu_w.ld)
        # (PICOMENU_SNAP_SIZE: snap_load_buffer's size, SNAP_LOAD_BUFFER_SIZE in picoface.cxx, which checks it)
        target_link_options(picoface PRIVATE -Wl,--defsym=PICOMENU_BT_SHARED=16384 -Wl,--defsym=PICOMENU_SNAP_SIZE=132160)
        target_compile_definitions(picoface PRIVATE PICOMENU_BT_SHARED=16384 PICOMENU_SNAP_SIZE=132160)
endif()


target_compile_definitions(picoface PRIVATE
        Z80_STATIC
        # the internal flash's file system: its size, from the end of the 2 MB (littlefs-lib, fwupdate.h)
        PICOMENU_FS_KB=${PICOMENU_FS_KB}
        # flash kept between the program and the file system (the Pico W's Bluetooth pairing)
        PICOMENU_RESERVE_KB=${PICOMENU_RESERVE_KB}
        # PicoMenu: nothing prints a float, so printf leaves out the code for them (about 2 KB)
        PICO_PRINTF_SUPPORT_FLOAT=0
        PICO_PRINTF_SUPPORT_EXPONENTIAL=0
        # the messages on the USB serial port: cmake -DPICOMENU_LOG=1 for a build with them
        PICOMENU_LOG=${PICOMENU_LOG}
        )

# Add any user requested libraries
target_link_libraries(picoface 
        hardware_pio
        pico_multicore
        FatFs_SPI
      
        "${WIFI_LIBS}"
        )

target_link_libraries(picoface littlefs-lib)

# 🌟 Bulletproof Compilation Timeline Synchronization 🌟
# This forces the build system to wait for the Z80 assembler and python scripts 
# to finish generating nmi.h and nmi.c BEFORE compiling any source code files.
set_source_files_properties(piohandler.S picoface.cxx PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/nmi.h"
)
# picoface.cxx is built for size like the rest (it was -O3): about 55 KB smaller, room for the
# tape converter. Its stack was measured again for this.
set_source_files_properties(picoface.cxx PROPERTIES COMPILE_OPTIONS "-Os")
# The tape converter's emulation (tzxconv.h and the Z80 emulator) is built for speed: -O2 is
# about 30% faster than -Os on the Pico and hardly bigger
set_source_files_properties(tzxconv_pico.cxx z80/Z80.c PROPERTIES COMPILE_OPTIONS "-O2")

pico_add_extra_outputs(picoface)

# The RAM left over is the heap, which littlefs and FatFs need: fail the build if there is too
# little (see check_heap.py)
add_custom_command(TARGET picoface POST_BUILD
    COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/check_heap.py ${CMAKE_NM} $<TARGET_FILE:picoface> ${PICOMENU_FS_KB} ${PICOMENU_RESERVE_KB}
    VERBATIM)

