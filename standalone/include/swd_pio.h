#pragma once
#include <stdbool.h>
#include <stdint.h>
void swd_pio_init(void);
void swd_pio_frequency(unsigned khz);
void swd_pio_direction(bool transmit);
void swd_pio_clock_enable(bool enable);
bool swd_pio_out(uint32_t bits, unsigned count);
bool swd_pio_in(uint32_t *bits, unsigned count);
