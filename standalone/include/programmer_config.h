#pragma once

/* PROVISIONAL fixture pin map. Confirm against the actual socket schematic.
 * All four Picos use the same image and these local pins. No socket multiplexer.
 * GP2/3 use the TXS0102 translator, enabled by GP4. START is GP18.
 * GP5 drives the external status LED; GP6 is unused.
 */
#define SWCLK_PIN       2
#define SWDIO_PIN       3
#define NRST_PIN        1
#define START_PIN       18
#define SWD_OE_PIN      4
#define STATUS_LED_PIN  5
#define FACTORY_ROLE_NONE   0u
#define FACTORY_ROLE_MASTER 1u
#define FACTORY_ROLE_SLAVE  2u
#ifndef FACTORY_ROLE
#define FACTORY_ROLE FACTORY_ROLE_NONE
#endif
#if FACTORY_ROLE != FACTORY_ROLE_NONE && FACTORY_ROLE != FACTORY_ROLE_MASTER && FACTORY_ROLE != FACTORY_ROLE_SLAVE
#error "FACTORY_ROLE must be NONE, MASTER or SLAVE"
#endif
#define SWD_OE_SETTLE_US 2u
/* Default: common-cathode RGB, active HIGH. The optional common-anode build
 * uses active LOW with the common tied to 3V3 and a resistor per cathode.
 * Transistor-driven common-anode assemblies may still require active HIGH.
 */
#ifndef RGB_LED_COMMON_ANODE
#define RGB_LED_COMMON_ANODE 0
#endif
#ifndef STATUS_LED_COMMON_ANODE
#define STATUS_LED_COMMON_ANODE 0
#endif
#define RED_LED_PIN     7
#define GREEN_LED_PIN   8
#define BLUE_LED_PIN    9

/* Preserve the existing total delay at each SWDIO direction handoff. */
#define SWD_DIRECTION_SETTLE_US 2u

/* PIO clock requests; characterize the actual wiring before raising these.
 * Minimum phase budget includes propagation, setup/hold, wiring and margin.
 * Direction handoff retains SWD_DIRECTION_SETTLE_US with no per-bit software wait.
 */
#ifndef SWD_USE_PIO
#define SWD_USE_PIO 0 /* Host tests / diagnostic bit-bang build. CMake enables PIO. */
#endif
#define SWD_CONNECT_KHZ        100u
#define SWD_PROGRAM_KHZ        2000u
#define SWD_MIN_HALF_PERIOD_NS 250u
#define SWD_PIO_TIMEOUT_US     10000u
#define RAM_BLOCK_BYTES        16384u
#ifndef STM32_HALT_AFTER_VERIFY
#define STM32_HALT_AFTER_VERIFY 0 /* Diagnostic build: keep CPU halted for external readback. */
#endif
#if STANDALONE_LOCAL_AP && FACTORY_ROLE == FACTORY_ROLE_MASTER
#define PROGRAMMER_BUILD_ID "factory-master-v21"
#elif STANDALONE_LOCAL_AP && FACTORY_ROLE == FACTORY_ROLE_SLAVE
#define PROGRAMMER_BUILD_ID "factory-slave-v21"
#elif STANDALONE_LOCAL_AP
#define PROGRAMMER_BUILD_ID "local-ap-v16"
#elif STANDALONE_NETWORK
#define PROGRAMMER_BUILD_ID "network-v3-usb"
#elif STM32_HALT_AFTER_VERIFY
#define PROGRAMMER_BUILD_ID "fast-v3-halt"
#else
#define PROGRAMMER_BUILD_ID "fast-v3"
#endif
#ifndef STM32_USE_FAST_PROGRAM
#define STM32_USE_FAST_PROGRAM 1
#endif
#ifndef STM32_USE_RAM_LOADER
#define STM32_USE_RAM_LOADER   1
#endif
#define LOADER_TIMEOUT_MS      2000u

/* Shared protocol deadlines. The optional bit-bang fallback rounds clock
 * requests to whole-microsecond half-periods and therefore caps at 500 kHz.
 */
#define SWD_WAIT_TIMEOUT_US    50000u
#define CONNECT_TIMEOUT_MS    1000u
#define ERASE_TIMEOUT_MS       15000u
#define WRITE_TIMEOUT_MS      100u
#define OPTION_TIMEOUT_MS     2000u
#define SESSION_TIMEOUT_MS    300000u
#define DEBOUNCE_MS           100u
#define STATUS_BLINK_HALF_MS  250u

/* Target policy: STM32L476JE 512 KiB, RDP0. Do not silently erase larger parts.
 * Mass erase destroys ALL main flash, including calibration/configuration.
 * Clear BFB2 only when set, preserving all other option settings. Never change
 * RDP/WRP/PCROP or program OTP/external flash. Verify options before main erase.
 */
#define TARGET_FLASH_BASE     0x08000000u
#define TARGET_FLASH_BYTES    (512u * 1024u)
#define TARGET_DEVICE_ID      0x415u
