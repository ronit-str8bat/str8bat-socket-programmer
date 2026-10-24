#include "pico/stdlib.h"
#include "hardware/gpio.h"

/*
 * GPIO fixture test -- Pico 2 W
 *
 * GP5 and GP7..GP9 are common-anode LED cathodes:
 * LOW = LED on, HIGH = LED off.
 *
 * Timing settings for this fixture test.
 */
#define TEST_ON_OFF_MS       1000u
#define TEST_FINAL_PAUSE_MS  2000u
#define BUTTON_DEBOUNCE_MS    100u

#define GP1_PIN   1u
#define GP2_PIN   2u
#define GP3_PIN   3u
#define GP4_PIN   4u
#define GP5_PIN   5u
#define GP7_PIN   7u
#define GP8_PIN   8u
#define GP9_PIN   9u
#define GP18_PIN 18u

static const uint8_t output_pins[] = {
    GP1_PIN, GP2_PIN, GP3_PIN, GP4_PIN, GP5_PIN, GP7_PIN, GP8_PIN, GP9_PIN
};

static void common_anode_led(uint8_t pin, bool on) {
    gpio_put(pin, !on);
}

/* GP18 is active-low. This test monitors its debounced state only. */
static bool gp18_pressed_debounced(uint32_t now_ms) {
    static bool raw_pressed;
    static bool stable_pressed;
    static uint32_t raw_changed_ms;
    bool sample_pressed = !gpio_get(GP18_PIN);

    if (sample_pressed != raw_pressed) {
        raw_pressed = sample_pressed;
        raw_changed_ms = now_ms;
    }
    if (raw_pressed != stable_pressed && now_ms - raw_changed_ms >= BUTTON_DEBOUNCE_MS) {
        stable_pressed = raw_pressed;
    }
    return stable_pressed;
}

int main(void) {
    for (unsigned int i = 0; i < sizeof(output_pins) / sizeof(output_pins[0]); ++i) {
        gpio_init(output_pins[i]);
        gpio_put(output_pins[i], false);
        gpio_set_dir(output_pins[i], GPIO_OUT);
    }

    gpio_init(GP18_PIN);
    gpio_set_dir(GP18_PIN, GPIO_IN);
    gpio_pull_up(GP18_PIN);

    while (true) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        uint32_t cycle_ms = now_ms % (6u * TEST_ON_OFF_MS + TEST_FINAL_PAUSE_MS);
        bool gp123_high = ((now_ms / TEST_ON_OFF_MS) & 1u) == 0u;
        bool gp7_on = gp123_high;
        bool gp5_on = cycle_ms < TEST_ON_OFF_MS;
        bool gp8_on = cycle_ms >= 2u * TEST_ON_OFF_MS && cycle_ms < 3u * TEST_ON_OFF_MS;
        bool gp9_on = cycle_ms >= 4u * TEST_ON_OFF_MS && cycle_ms < 5u * TEST_ON_OFF_MS;

        gpio_put(GP1_PIN, gp123_high);
        gpio_put(GP2_PIN, gp123_high);
        gpio_put(GP3_PIN, gp123_high);
        gpio_put(GP4_PIN, true);
        common_anode_led(GP5_PIN, gp5_on);
        common_anode_led(GP7_PIN, gp7_on);
        common_anode_led(GP8_PIN, gp8_on);
        common_anode_led(GP9_PIN, gp9_on);

        (void)gp18_pressed_debounced(now_ms);
        sleep_ms(5u);
    }
}
