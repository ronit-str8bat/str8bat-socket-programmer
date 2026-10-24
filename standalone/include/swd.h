#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SWD_OK, SWD_WAIT_TIMEOUT, SWD_FAULT, SWD_BAD_ACK, SWD_PARITY,
    SWD_POWER_TIMEOUT, SWD_BAD_ID, SWD_AP_ERROR, SWD_PIO_TIMEOUT
} swd_error_t;
void swd_init(void);
void swd_set_half_period(unsigned us);
void swd_set_frequency(unsigned khz);
void swd_reset_assert(bool asserted);
bool swd_connect(void);
bool swd_read32(uint32_t address, uint32_t *value);
bool swd_write32(uint32_t address, uint32_t value);
bool swd_write_doubleword(uint32_t address, uint32_t lo, uint32_t hi);
bool swd_read_words(uint32_t address, uint32_t *data, size_t words);
/* Auto-incrementing block writes are for target SRAM, not raw flash bursts. */
bool swd_write_words(uint32_t address, const uint32_t *data, size_t words);
void swd_release_lines(void);
swd_error_t swd_last_error(void);
