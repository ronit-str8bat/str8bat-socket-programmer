#include "ui_logic.h"
#include "programmer_config.h"

bool status_led_on(bool logs_full, bool clock_set, bool wifi_connected, uint32_t now) {
    if (logs_full) return (now / 250u) % 2u == 0;
    if (!clock_set) return (now / 500u) % 2u == 0;
    return wifi_connected;
}

void start_button_reset(start_button_t *b, bool pressed, uint32_t now) {
    *b = (start_button_t){ .raw_pressed = pressed, .stable_pressed = pressed,
                          .armed = false, .changed_ms = now };
}

bool start_button_update(start_button_t *b, bool pressed, uint32_t now) {
    if (pressed != b->raw_pressed) {
        b->raw_pressed = pressed;
        b->changed_ms = now;
    }
    if ((uint32_t)(now - b->changed_ms) < DEBOUNCE_MS) return false;
    if (!pressed) { /* A stable release is required after boot and every job. */
        b->stable_pressed = false;
        b->armed = true;
        return false;
    }
    if (!b->stable_pressed) {
        b->stable_pressed = true;
        if (b->armed) {
            b->armed = false;
            return true;
        }
    }
    return false;
}
