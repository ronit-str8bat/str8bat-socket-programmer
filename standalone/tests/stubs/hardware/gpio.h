#pragma once
#include <stdbool.h>
#define GPIO_IN false
#define GPIO_OUT true
#define GPIO_DRIVE_STRENGTH_2MA 0
#define GPIO_SLEW_RATE_SLOW 0
void gpio_init(unsigned pin);
void gpio_put(unsigned pin, bool value);
bool gpio_get(unsigned pin);
void gpio_set_dir(unsigned pin, bool output);
void gpio_disable_pulls(unsigned pin);
void gpio_set_drive_strength(unsigned pin, unsigned strength);
void gpio_set_slew_rate(unsigned pin, unsigned rate);
