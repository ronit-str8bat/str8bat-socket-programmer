#pragma once
#include <stdint.h>
#include <stdbool.h>
uint64_t time_us_64(void);
void busy_wait_us_32(uint32_t us);
void sleep_ms(uint32_t ms);
