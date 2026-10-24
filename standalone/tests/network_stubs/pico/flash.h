#pragma once
#include <stdint.h>
#define PICO_OK 0
int flash_safe_execute(void (*fn)(void *),void *arg,uint32_t timeout);
