cmake_minimum_required(VERSION 3.20)

# This is intentionally a separate, minimal test firmware.  It does not
# include the programmer, Wi-Fi, flash storage, or STM32 firmware image.
set(PICO_BOARD pico2_w CACHE STRING "Test board")
include(${CMAKE_CURRENT_LIST_DIR}/../pico_sdk_import.cmake)

project(gpio_pin_test C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(gpio_pin_test main.c)
target_link_libraries(gpio_pin_test PRIVATE pico_stdlib hardware_gpio)
target_compile_options(gpio_pin_test PRIVATE -Wall -Wextra -Werror)

# No serial connection is required for this fixture test.
pico_enable_stdio_usb(gpio_pin_test 0)
pico_enable_stdio_uart(gpio_pin_test 0)
pico_add_extra_outputs(gpio_pin_test)
