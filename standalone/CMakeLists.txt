cmake_minimum_required(VERSION 3.20)
set(PICO_BOARD pico2_w CACHE STRING "Standalone programmer board")
include(${CMAKE_CURRENT_LIST_DIR}/../pico_sdk_import.cmake)
project(standalone_programmer C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()
if(NOT PICO_BOARD STREQUAL "pico2_w")
    message(FATAL_ERROR "This application requires Pico 2 W, including its CYW43 LED")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(TARGET_FIRMWARE "" CACHE FILEPATH "STM32 production .bin to embed")
if(NOT EXISTS "${TARGET_FIRMWARE}")
    message(FATAL_ERROR "Set TARGET_FIRMWARE to the production_firmware_v4.0.2.bin path")
endif()
set(GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED})
option(STANDALONE_SWD_PIO "Use hardware PIO SWD instead of bit banging" ON)
option(STANDALONE_FAST_PROGRAM "Use STM32 fast programming for dense 256-byte rows" ON)
option(STANDALONE_USB_DIAGNOSTICS "Expose read-only result reports over USB CDC; never wait for a host" ON)
option(STANDALONE_HALT_AFTER_VERIFY "Leave verified target halted for external readback; do not boot product" OFF)
option(STANDALONE_NETWORK "Enable Wi-Fi provisioning, HTTPS target OTA and LittleFS logs" OFF)
option(STANDALONE_LOCAL_AP "Always-on local WPA2 hotspot and CSV API (no Internet services)" OFF)
option(STANDALONE_RGB_COMMON_ANODE "Use active-low external RGB outputs for a direct-drive common-anode LED" OFF)
option(STANDALONE_STATUS_LED_COMMON_ANODE "Use an active-low output for the direct-drive common-anode status LED" OFF)
option(STANDALONE_FACTORY_MASTER "Build the factory master distributor role" OFF)
option(STANDALONE_FACTORY_SLAVE "Build the factory slave programmer role" OFF)
set(FACTORY_GROUP_KEY "" CACHE STRING "Shared private key for factory master/slave transfer")
if(STANDALONE_LOCAL_AP AND NOT STANDALONE_NETWORK)
    message(FATAL_ERROR "STANDALONE_LOCAL_AP requires STANDALONE_NETWORK")
endif()
if(STANDALONE_FACTORY_MASTER AND STANDALONE_FACTORY_SLAVE)
    message(FATAL_ERROR "Choose only one factory role")
endif()
if((STANDALONE_FACTORY_MASTER OR STANDALONE_FACTORY_SLAVE) AND NOT STANDALONE_LOCAL_AP)
    message(FATAL_ERROR "Factory master/slave builds require STANDALONE_LOCAL_AP")
endif()
string(LENGTH "${FACTORY_GROUP_KEY}" FACTORY_GROUP_KEY_LENGTH)
if((STANDALONE_FACTORY_MASTER OR STANDALONE_FACTORY_SLAVE) AND
   (NOT FACTORY_GROUP_KEY MATCHES "^[A-Za-z0-9._-]+$" OR FACTORY_GROUP_KEY_LENGTH LESS 16 OR FACTORY_GROUP_KEY_LENGTH GREATER 63))
    message(FATAL_ERROR "Factory builds require a 16..63 character FACTORY_GROUP_KEY containing letters, digits, dot, underscore or hyphen")
endif()
set(FACTORY_ROLE_VALUE 0)
if(STANDALONE_FACTORY_MASTER)
    set(FACTORY_ROLE_VALUE 1)
elseif(STANDALONE_FACTORY_SLAVE)
    set(FACTORY_ROLE_VALUE 2)
endif()
set(TARGET_FIRMWARE_VERSION "v4.0.2" CACHE STRING "Version of embedded STM32 binary")
string(LENGTH "${TARGET_FIRMWARE_VERSION}" VERSION_LENGTH)
if(NOT TARGET_FIRMWARE_VERSION MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$" OR VERSION_LENGTH GREATER 31)
    message(FATAL_ERROR "Firmware version must be 1..31 CSV-safe letters, digits, dots, underscores or hyphens")
endif()
configure_file(include/firmware_version.h.in ${GENERATED}/firmware_version.h @ONLY)
add_custom_command(OUTPUT ${GENERATED}/target_loader.h
    COMMAND ${CMAKE_C_COMPILER} -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
        -Os -ffreestanding -fno-builtin -fno-unwind-tables -fno-asynchronous-unwind-tables
        -ffunction-sections -fdata-sections -nostdlib -Wall -Wextra -Werror
        -I${CMAKE_CURRENT_LIST_DIR}/include
        -Wl,--build-id=none,--gc-sections,-T,${CMAKE_CURRENT_LIST_DIR}/loader/loader.ld
        ${CMAKE_CURRENT_LIST_DIR}/loader/stm32l476_loader.c -o ${GENERATED}/target_loader.elf
    COMMAND ${CMAKE_OBJCOPY} -O binary ${GENERATED}/target_loader.elf ${GENERATED}/target_loader.bin
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/embed_loader.py
        ${GENERATED}/target_loader.bin ${GENERATED}/target_loader.h
    DEPENDS loader/stm32l476_loader.c loader/loader.ld include/loader_protocol.h include/crc32_nibble.h tools/embed_loader.py
    VERBATIM)
add_custom_command(OUTPUT ${GENERATED}/firmware_image.c ${GENERATED}/firmware_image.h
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/embed_firmware.py
        --input ${TARGET_FIRMWARE} --output ${GENERATED}
    DEPENDS ${TARGET_FIRMWARE} ${CMAKE_CURRENT_LIST_DIR}/tools/embed_firmware.py
    VERBATIM)
add_executable(standalone_programmer
    src/main.c src/ui_logic.c src/crc32.c src/swd.c src/stm32_programmer.c
    ${GENERATED}/firmware_image.c ${GENERATED}/target_loader.h)
if(STANDALONE_SWD_PIO)
    target_sources(standalone_programmer PRIVATE src/swd_pio.c)
    target_compile_definitions(standalone_programmer PRIVATE SWD_USE_PIO=1)
    pico_generate_pio_header(standalone_programmer ${CMAKE_CURRENT_LIST_DIR}/src/swd_wire.pio)
    target_link_libraries(standalone_programmer PRIVATE hardware_pio hardware_clocks)
endif()
target_include_directories(standalone_programmer PRIVATE include ${GENERATED})
target_compile_definitions(standalone_programmer PRIVATE PICO_CORE1_STACK_SIZE=4096)
target_compile_definitions(standalone_programmer PRIVATE
    STANDALONE_NETWORK=$<BOOL:${STANDALONE_NETWORK}>
    STANDALONE_LOCAL_AP=$<BOOL:${STANDALONE_LOCAL_AP}>
    STM32_HALT_AFTER_VERIFY=$<BOOL:${STANDALONE_HALT_AFTER_VERIFY}>
    RGB_LED_COMMON_ANODE=$<BOOL:${STANDALONE_RGB_COMMON_ANODE}>
    STATUS_LED_COMMON_ANODE=$<BOOL:${STANDALONE_STATUS_LED_COMMON_ANODE}>
    FACTORY_ROLE=${FACTORY_ROLE_VALUE}
    $<$<BOOL:${STANDALONE_FACTORY_MASTER}>:FACTORY_GROUP_KEY=\"${FACTORY_GROUP_KEY}\">
    $<$<BOOL:${STANDALONE_FACTORY_SLAVE}>:FACTORY_GROUP_KEY=\"${FACTORY_GROUP_KEY}\">
    STM32_USE_FAST_PROGRAM=$<BOOL:${STANDALONE_FAST_PROGRAM}>)
target_compile_options(standalone_programmer PRIVATE -Wall -Wextra -Werror)
target_link_libraries(standalone_programmer PRIVATE
    pico_stdlib pico_multicore hardware_gpio hardware_sync)
if(STANDALONE_NETWORK)
    target_compile_definitions(standalone_programmer PRIVATE PICO_STACK_SIZE=16384)
    add_library(programmer_littlefs STATIC third_party/littlefs/lfs.c third_party/littlefs/lfs_util.c)
    target_include_directories(programmer_littlefs PUBLIC third_party/littlefs)
    target_compile_definitions(programmer_littlefs PRIVATE LFS_NO_DEBUG LFS_NO_WARN)
    target_sources(standalone_programmer PRIVATE third_party/pico_ap/dhcpserver.c third_party/pico_ap/dnsserver.c)
    target_include_directories(standalone_programmer PRIVATE third_party/pico_ap)
    set_source_files_properties(third_party/pico_ap/dhcpserver.c third_party/pico_ap/dnsserver.c
        PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter")
    if(STANDALONE_LOCAL_AP)
        target_link_libraries(standalone_programmer PRIVATE hardware_powman)
        target_sources(standalone_programmer PRIVATE src/services/local_storage.c src/services/local_clock.c
            src/services/local_portal.c src/services/local_network.c)
        if(STANDALONE_FACTORY_SLAVE)
            target_sources(standalone_programmer PRIVATE src/services/factory_client.c)
        endif()
    else()
        execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/create_device_key.py
            ${CMAKE_CURRENT_LIST_DIR}/private/device_key.h COMMAND_ERROR_IS_FATAL ANY)
        target_sources(standalone_programmer PRIVATE src/services/storage.c src/services/protocol.c
            src/services/http_client.c src/services/portal.c src/services/network.c src/services/usb_export.c)
        target_include_directories(standalone_programmer PRIVATE private)
        target_link_libraries(standalone_programmer PRIVATE pico_lwip_sntp)
    endif()
    target_link_libraries(standalone_programmer PRIVATE programmer_littlefs
        pico_cyw43_arch_lwip_poll pico_flash hardware_flash pico_rand pico_unique_id hardware_watchdog
        pico_mbedtls pico_lwip_mbedtls)
    # A linker ASSERT fails before a UF2 can overlap the OTA/storage partition.
    target_link_options(standalone_programmer PRIVATE
        "-Wl,--defsym=RAM_LENGTH=0x7c000"
        "-Wl,--defsym=SCRATCH_Y_ORIGIN=0x2007c000"
        "-Wl,--defsym=SCRATCH_Y_LENGTH=0x4000"
        "-Wl,--defsym=__programmer_app_limit=0x10180000"
        "-Wl,-T,${CMAKE_CURRENT_LIST_DIR}/storage_guard.ld")
else()
    target_link_libraries(standalone_programmer PRIVATE pico_cyw43_arch_none)
endif()
# Diagnostics are optional; button-driven programming never waits for USB.
pico_enable_stdio_usb(standalone_programmer ${STANDALONE_USB_DIAGNOSTICS})
target_compile_definitions(standalone_programmer PRIVATE
    STANDALONE_USB_DIAGNOSTICS=$<BOOL:${STANDALONE_USB_DIAGNOSTICS}>
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
    PICO_STDIO_USB_POST_CONNECT_WAIT_DELAY_MS=0
    PICO_STDIO_USB_STDOUT_TIMEOUT_US=1000)
pico_enable_stdio_uart(standalone_programmer 0)
pico_add_extra_outputs(standalone_programmer)
