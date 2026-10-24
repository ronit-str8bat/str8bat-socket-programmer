#include "swd_pio.h"
#include "programmer_config.h"
#include "swd_wire.pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

static PIO const wire_pio = pio0;
static int sm = -1;
static unsigned offset;

void swd_pio_frequency(unsigned khz) {
    if (!khz) khz = 1;
    uint32_t hz = clock_get_hz(clk_sys);
    /* Integer divider rounds DOWN the requested speed and enforces the
     * configurable translator budget on BOTH high and low phases.
     */
    uint64_t by_rate = ((uint64_t)hz + 4000ull * khz - 1) / (4000ull * khz);
    uint64_t by_phase = ((uint64_t)hz * SWD_MIN_HALF_PERIOD_NS + 1999999999ull) / 2000000000ull;
    uint32_t divider = (uint32_t)(by_rate > by_phase ? by_rate : by_phase);
    if (!divider) divider = 1;
    if (divider > 65535) divider = 65535;
    pio_sm_set_clkdiv_int_frac(wire_pio, (unsigned)sm, (uint16_t)divider, 0);
}

void swd_pio_init(void) {
    if (sm < 0) {
        sm = pio_claim_unused_sm(wire_pio, true);
        offset = pio_add_program(wire_pio, &swd_wire_program);
    }
    pio_sm_set_enabled(wire_pio, (unsigned)sm, false);
    pio_gpio_init(wire_pio, SWCLK_PIN);
    pio_gpio_init(wire_pio, SWDIO_PIN);
    pio_sm_config c = swd_wire_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, SWCLK_PIN);
    sm_config_set_out_pins(&c, SWDIO_PIN, 1);
    sm_config_set_in_pins(&c, SWDIO_PIN);
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_in_shift(&c, true, false, 32);
    pio_sm_init(wire_pio, (unsigned)sm, offset, &c);
    pio_sm_set_pins_with_mask(wire_pio, (unsigned)sm, 0, (1u << SWCLK_PIN) | (1u << SWDIO_PIN));
    pio_sm_set_pindirs_with_mask(wire_pio, (unsigned)sm, 0, (1u << SWCLK_PIN) | (1u << SWDIO_PIN));
    swd_pio_frequency(SWD_CONNECT_KHZ);
    pio_sm_set_enabled(wire_pio, (unsigned)sm, true);
}

void swd_pio_clock_enable(bool enable) {
    pio_sm_set_pindirs_with_mask(wire_pio, (unsigned)sm, enable ? 1u << SWCLK_PIN : 0, 1u << SWCLK_PIN);
}
void swd_pio_direction(bool transmit) {
    if (transmit) pio_sm_set_pins_with_mask(wire_pio, (unsigned)sm, 1u << SWDIO_PIN, 1u << SWDIO_PIN);
    pio_sm_set_pindirs_with_mask(wire_pio, (unsigned)sm, transmit ? 1u << SWDIO_PIN : 0, 1u << SWDIO_PIN);
}

static bool exchange(uint32_t data, unsigned count, bool read, uint32_t *reply) {
    if (!count || count > 32) return false;
    uint64_t deadline = time_us_64() + SWD_PIO_TIMEOUT_US;
    /* At most two TX words, only one outstanding command. Completion is pushed
     * after the final falling edge, never before the wire has become idle.
     */
    while (pio_sm_is_tx_fifo_full(wire_pio, (unsigned)sm)) {
        if (time_us_64() >= deadline) return false;
    }
    pio_sm_put(wire_pio, (unsigned)sm, count - 1u + (read ? 32u : 0u));
    if (!read) {
        while (pio_sm_is_tx_fifo_full(wire_pio, (unsigned)sm)) {
            if (time_us_64() >= deadline) return false;
        }
        pio_sm_put(wire_pio, (unsigned)sm, data);
    }
    while (pio_sm_is_rx_fifo_empty(wire_pio, (unsigned)sm)) {
        if (time_us_64() >= deadline) {
            pio_sm_set_enabled(wire_pio, (unsigned)sm, false);
            pio_sm_set_pins_with_mask(wire_pio, (unsigned)sm, 0, 1u << SWCLK_PIN);
            return false;
        }
    }
    uint32_t result = pio_sm_get(wire_pio, (unsigned)sm);
    if (reply) *reply = count == 32 ? result : result >> (32u - count);
    return true;
}
bool swd_pio_out(uint32_t bits, unsigned count) { return exchange(bits, count, false, 0); }
bool swd_pio_in(uint32_t *bits, unsigned count) { return exchange(0, count, true, bits); }
