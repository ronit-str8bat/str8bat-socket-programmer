#pragma once
#include <stdint.h>
extern uint8_t fake_flash[0x400000];
#define XIP_BASE ((uintptr_t)fake_flash)
