/* Native ARM ADIv5 SWD host, AP0 AHB-AP, one-cycle turnaround.
 * No CMSIS-DAP USB server or OpenOCD is required. All transactions are bounded.
 * WAIT is retried, FAULT/parity/protocol failures stop the job. Writes that may
 * already have reached flash are never blindly replayed after an error.
 */
#include "swd.h"
#include "programmer_config.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#if SWD_USE_PIO
#include "swd_pio.h"
#endif

#if !SWD_USE_PIO
static unsigned half_us = 5u;
static void delay(void) { busy_wait_us_32(half_us); }
#endif
static swd_error_t last_error;
static bool direction_valid, transmitting;
static bool translator_enabled;

static void direction(bool transmit) {
    if (direction_valid && transmit == transmitting) return;
    /* SWCLK is low. Release SWDIO before changing direction. */
#if SWD_USE_PIO
    swd_pio_direction(false);
#else
    gpio_set_dir(SWDIO_PIN, GPIO_IN);
#endif
    if (transmit) {
#if SWD_USE_PIO
        swd_pio_direction(true);
#else
        gpio_put(SWDIO_PIN, 1); /* Never enable an output with an unknown latch. */
        gpio_set_dir(SWDIO_PIN, GPIO_OUT);
#endif
    }
    busy_wait_us_32(SWD_DIRECTION_SETTLE_US);
    direction_valid = true;
    transmitting = transmit;
    /* OE stays high for both SWDIO directions and the complete SWD session.
     * Enable only after the idle pin states are set, before any wire clocks. */
    if (!translator_enabled) {
        gpio_put(SWD_OE_PIN, 1);
        busy_wait_us_32(SWD_OE_SETTLE_US);
        translator_enabled = true;
    }
}

static bool clock_bit(void) {
#if SWD_USE_PIO
    uint32_t bit = 0;
    if (!swd_pio_in(&bit, 1)) last_error = SWD_PIO_TIMEOUT;
    return bit != 0;
#else
    delay();
    /* Sample the current target bit BEFORE the next rising edge. The target
     * can advance SWDIO after that edge, so sampling later in the high phase
     * skips a bit (including the first ACK bit after turnaround). This follows
     * the sampling order in Arm CMSIS-DAP's SW_READ_BIT implementation.
     */
    bool bit = gpio_get(SWDIO_PIN);
    gpio_put(SWCLK_PIN, 1);
    delay();
    gpio_put(SWCLK_PIN, 0);
    return bit;
#endif
}

static void put_bits(uint32_t data, unsigned count) {
#if SWD_USE_PIO
    if (!swd_pio_out(data, count)) last_error = SWD_PIO_TIMEOUT;
#else
    for (unsigned i = 0; i < count; ++i) {
        gpio_put(SWDIO_PIN, data & 1u);
        (void)clock_bit();
        data >>= 1;
    }
#endif
}

static uint32_t get_bits(unsigned count) {
    uint32_t value = 0;
#if SWD_USE_PIO
    if (!swd_pio_in(&value, count)) last_error = SWD_PIO_TIMEOUT;
#else
    for (unsigned i = 0; i < count; ++i)
        value |= (uint32_t)clock_bit() << i;
#endif
    return value;
}

static unsigned parity(uint32_t value) { return __builtin_parity(value); }

static bool transfer(bool ap, bool read, uint8_t address, uint32_t *value) {
    if (last_error == SWD_PIO_TIMEOUT) return false;
    uint8_t fields = (ap ? 1u : 0u) | (read ? 2u : 0u) | (address & 12u);
    uint8_t request = 0x81u | (fields << 1) | (parity(fields) << 5);
    uint64_t deadline = time_us_64() + SWD_WAIT_TIMEOUT_US;
    do {
        direction(true);
        put_bits(request, 8);
        direction(false);
        (void)clock_bit(); /* Host releases before target ACK. */
        uint32_t ack = get_bits(3);
        if (last_error == SWD_PIO_TIMEOUT) return false;
        if (ack == 1u) {
            bool valid = true;
            if (read) {
                uint32_t data = get_bits(32);
                valid = get_bits(1) == parity(data);
                *value = data;
                (void)clock_bit(); /* Target releases before host drives. */
                direction(true);
            } else {
                (void)clock_bit();
                direction(true);
                put_bits(*value, 32);
                put_bits(parity(*value), 1);
            }
            put_bits(0, 8); /* Idle low clocks complete posted target accesses. */
            if (last_error == SWD_PIO_TIMEOUT) return false;
            if (!valid) { last_error = SWD_PARITY; return false; }
            return true;
        }
        /* Default ORUNDETECT=0: WAIT/FAULT has no data phase. */
        if (ack != 2u && ack != 4u) {
            for (unsigned i = 0; i < 34; ++i) (void)clock_bit();
            last_error = SWD_BAD_ACK;
            return false;
        }
        (void)clock_bit();
        direction(true);
        put_bits(0, 8);
        if (ack == 4u) { last_error = SWD_FAULT; return false; }
    } while (time_us_64() < deadline);
    last_error = SWD_WAIT_TIMEOUT;
    return false;
}

static bool dp_write(uint8_t reg, uint32_t value) { return transfer(false, false, reg, &value); }
static bool dp_read(uint8_t reg, uint32_t *value) { return transfer(false, true, reg, value); }
static bool ap_write(uint8_t reg, uint32_t value) { return transfer(true, false, reg, &value); }
static bool ap_read(uint8_t reg, uint32_t *value) { return transfer(true, true, reg, value); }

static bool flush(void) {
    uint32_t discard, status;
    if (!dp_read(12, &discard) || !dp_read(4, &status)) return false;
    if (status & 0xb2u) { last_error = SWD_AP_ERROR; return false; }
    return true;
}

void swd_init(void) {
    static bool reset_initialized;
    static bool oe_initialized;
    if (!oe_initialized) {
        gpio_init(SWD_OE_PIN);
        gpio_disable_pulls(SWD_OE_PIN);
        oe_initialized = true;
    }
    /* Preload LOW before output enable. Reinitialization also disconnects the
     * translator before resetting SWD/PIO state, without releasing NRST. */
    gpio_put(SWD_OE_PIN, 0);
    gpio_set_dir(SWD_OE_PIN, GPIO_OUT);
    busy_wait_us_32(SWD_OE_SETTLE_US);
    translator_enabled = false;
    direction_valid = false;
    last_error = SWD_OK;
    /* gpio_init releases the pin. Do this only at boot: reinitializing SWD
     * after a failed job must not release a target held in reset. */
    if (!reset_initialized) {
        gpio_init(NRST_PIN);
        gpio_put(NRST_PIN, 0);
        gpio_disable_pulls(NRST_PIN);
        gpio_set_dir(NRST_PIN, GPIO_IN);
        reset_initialized = true;
    }
    gpio_init(SWCLK_PIN);
    gpio_put(SWCLK_PIN, 0);
    gpio_set_dir(SWCLK_PIN, GPIO_IN);
    gpio_disable_pulls(SWCLK_PIN);
    gpio_init(SWDIO_PIN);
    gpio_disable_pulls(SWDIO_PIN);
    gpio_set_drive_strength(SWCLK_PIN, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(SWDIO_PIN, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_slew_rate(SWCLK_PIN, GPIO_SLEW_RATE_SLOW);
    gpio_set_slew_rate(SWDIO_PIN, GPIO_SLEW_RATE_SLOW);
#if SWD_USE_PIO
    swd_pio_init();
#endif
}

void swd_set_frequency(unsigned khz) {
#if SWD_USE_PIO
    swd_pio_frequency(khz);
#else
    half_us = khz ? (500u + khz - 1u) / khz : 5u;
    if (!half_us) half_us = 1;
#endif
}
void swd_set_half_period(unsigned us) { swd_set_frequency(us ? 500u / us : 500u); }
void swd_reset_assert(bool asserted) {
    /* Open drain: never drive NRST high; use the target-side pull-up. */
    gpio_put(NRST_PIN, 0);
    gpio_set_dir(NRST_PIN, asserted ? GPIO_OUT : GPIO_IN);
}

bool swd_connect(void) {
    last_error = SWD_OK;
#if SWD_USE_PIO
    swd_pio_clock_enable(true);
#else
    gpio_put(SWCLK_PIN, 0);
    gpio_set_dir(SWCLK_PIN, GPIO_OUT);
#endif
    direction(true);
    put_bits(0xffffffffu, 32); put_bits(0xffffffffu, 32);
    put_bits(0xe79eu, 16); /* JTAG-to-SWD selection, least-significant bit first. */
    put_bits(0xffffffffu, 32); put_bits(0xffffffffu, 32); put_bits(0, 2);
    uint32_t id, status;
    if (!dp_read(0, &id)) return false;
    if (id != 0x2ba01477u) { last_error = SWD_BAD_ID; return false; }
    if (!dp_write(0, 0x1eu) || !dp_write(8, 0) || !dp_write(4, 0x50000000u)) return false;
    uint64_t deadline = time_us_64() + CONNECT_TIMEOUT_MS * 1000u;
    do {
        if (!dp_read(4, &status)) return false;
        if ((status & 0xa0000000u) == 0xa0000000u) {
            /* APSEL=0/APBANKSEL=0, privileged data, 32-bit, increment single. */
            return ap_write(0, 0x23000052u) && flush();
        }
    } while (time_us_64() < deadline);
    last_error = SWD_POWER_TIMEOUT;
    return false;
}

bool swd_read_words(uint32_t address, uint32_t *data, size_t words) {
    if ((address & 3u) || words > (UINT32_MAX - address) / 4u || (words && !data)) {
        last_error = SWD_AP_ERROR; return false;
    }
    while (words) {
        /* TAR increment wraps on a 1 KiB boundary. Respect it explicitly. */
        size_t chunk = (1024u - (address & 1023u)) / 4u;
        if (chunk > words) chunk = words;
        uint32_t discard;
        if (!ap_write(4, address) || !ap_read(12, &discard)) return false;
        for (size_t i = 0; i + 1 < chunk; ++i)
            if (!ap_read(12, &data[i])) return false;
        if (!dp_read(12, &data[chunk - 1])) return false; /* Final posted read. */
        uint32_t status;
        if (!dp_read(4, &status)) return false;
        if (status & 0xb2u) { last_error = SWD_AP_ERROR; return false; }
        data += chunk; address += (uint32_t)chunk * 4u; words -= chunk;
    }
    return true;
}
bool swd_read32(uint32_t address, uint32_t *value) { return swd_read_words(address, value, 1); }
bool swd_write32(uint32_t address, uint32_t value) {
    return ap_write(4, address) && ap_write(12, value) && flush();
}
bool swd_write_doubleword(uint32_t address, uint32_t lo, uint32_t hi) {
    if (address & 7u) { last_error = SWD_AP_ERROR; return false; }
    /* Ordered 32-bit writes to the two halves of an aligned 64-bit flash word. */
    return ap_write(4, address) && ap_write(12, lo) && ap_write(12, hi) && flush();
}
bool swd_write_words(uint32_t address, const uint32_t *data, size_t words) {
    /* Do not use this on STM32 flash: SRAM staging only. Each WDATA transfer
     * checks ACK; WAIT is retried without advancing the pointer. FAULT or
     * uncertain completion stops the operation, never replays flash writes.
     */
    if ((address & 3u) || address < 0x20000000u || address > 0x20018000u ||
        words > (0x20018000u - address) / 4u || (words && !data)) {
        last_error = SWD_AP_ERROR; return false;
    }
    while (words) {
        size_t chunk = (1024u - (address & 1023u)) / 4u;
        if (chunk > words) chunk = words;
        if (!ap_write(4, address)) return false;
        for (size_t i = 0; i < chunk; ++i)
            if (!ap_write(12, data[i])) return false;
        if (!flush()) return false;
        address += (uint32_t)chunk * 4u; data += chunk; words -= chunk;
    }
    return true;
}
void swd_release_lines(void) {
    gpio_put(SWD_OE_PIN, 0);
    busy_wait_us_32(SWD_OE_SETTLE_US);
    translator_enabled = false;
#if SWD_USE_PIO
    swd_pio_direction(false);
    swd_pio_clock_enable(false);
#else
    gpio_set_dir(SWDIO_PIN, GPIO_IN);
    gpio_set_dir(SWCLK_PIN, GPIO_IN);
#endif
    direction_valid = false;
}
swd_error_t swd_last_error(void) { return last_error; }
