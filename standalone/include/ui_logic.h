#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    bool raw_pressed;
    bool stable_pressed;
    bool armed;
    uint32_t changed_ms;
} start_button_t;
void start_button_reset(start_button_t *button, bool pressed, uint32_t now);
bool start_button_update(start_button_t *button, bool pressed, uint32_t now);
/* LED 2 priority: CSV full, unset clock, Wi-Fi association, otherwise OFF.
 * Period values are ON/OFF intervals, not complete blink cycles. */
bool status_led_on(bool logs_full, bool clock_set, bool wifi_connected, uint32_t now);
