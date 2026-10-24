#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    STAGE_CONNECT, STAGE_ERASE, STAGE_PROGRAM, STAGE_VERIFY, STAGE_RESTART
} programmer_stage_t;
typedef enum {
    RESULT_OK, RESULT_IMAGE, RESULT_SWD, RESULT_TARGET, RESULT_PROTECTED,
    RESULT_RESET, RESULT_TIMEOUT, RESULT_FLASH, RESULT_VERIFY, RESULT_CLOCK
} programmer_result_t;
typedef struct {
    programmer_result_t result;
    programmer_stage_t stage;
    uint32_t address;
    uint32_t detail;
    uint32_t elapsed_ms;
    uint32_t stage_ms[5]; /* Connect/setup, erase/blank, program, verify, restart. */
    uint32_t initial_rcc_cr, initial_rcc_cfgr, initial_pwr_cr1;
    uint32_t loader_status, loader_completed;
    uint32_t standard_retry; /* One full reset/erase/rewrite after a fast flash error. */
    uint32_t fast_error_address, fast_error_detail, fast_error_status;
    uint32_t fast_rows; /* Rows acknowledged in this attempt; reset before recovery. */
    uint32_t target_halted; /* Confirmed at job completion, not a live external-debugger status. */
    uint32_t optr_before, optr_after; /* After is the last shadow read, not proof of reload. */
    uint32_t bfb2_attempted, bfb2_cleared; /* Cleared means verified AFTER option reload. */
    uint32_t option_phase; /* 1=read, 2=unlock, 3=program, 4=prepare reload,
                           * 7=reload/reconnect, 8=halt, 9=verify reload,
                           * 5=checked, 6=final check. */
    uint32_t reload_dhcsr, reload_reconnects; /* Last valid status and phase-8 reconnect attempts. */
    uint32_t uid[3]; /* STM32 96-bit UID, words at 0x1fff7590/94/98. */
    bool uid_valid;
} programmer_report_t;
typedef void (*programmer_progress_fn)(programmer_stage_t stage, uint32_t done);
programmer_report_t stm32_program(const uint8_t *image, size_t size,
                                programmer_progress_fn progress);
