/* STM32L476JE main-flash programmer. Fast rows and standard sparse writes.
 * Main-flash operations use the validated SRAM loader. Option reload may
 * reset debug controls; reestablish halt and verify options before erasing.
 */
#include "stm32_programmer.h"
#include "programmer_config.h"
#include "swd.h"
#include "pico/stdlib.h"
#include "crc32.h"
#include "loader_protocol.h"
#if STM32_USE_RAM_LOADER
#include "target_loader.h"
#endif
#include <string.h>

#define DHCSR       0xe000edf0u
#define DEMCR       0xe000edfcu
#define FLASH_ACR   0x40022000u
#define FLASH_KEYR  0x40022008u
#define FLASH_OPTKEYR 0x4002200cu
#define FLASH_SR    0x40022010u
#define FLASH_CR    0x40022014u
#define FLASH_OPTR  0x40022020u
#define FLASH_BSY   0x00010000u
#define FLASH_ERRORS 0x0000c3fau
#define FLASH_MER   0x00008004u
#define FLASH_LOCK  0x80000000u
#define FLASH_OPTLOCK 0x40000000u
#define FLASH_OPTSTRT 0x00020000u
#define FLASH_EOPIE 0x01000000u
#define FLASH_OBL_LAUNCH 0x08000000u
#define FLASH_BFB2  0x00100000u
#define DHCSR_RESET_ST (1u << 25)

/* All STM32L476 option registers; untouched shadows are preserved by OPTSTRT.
 * Keep a snapshot for reload verification, retry checks and the final check.
 */
static const uint32_t option_addresses[] = {
    FLASH_OPTR, 0x40022024u, 0x40022028u, 0x4002202cu, 0x40022030u,
    0x40022044u, 0x40022048u, 0x4002204cu, 0x40022050u
};
#define OPTION_COUNT (sizeof(option_addresses) / sizeof(option_addresses[0]))
static uint32_t expected_options[OPTION_COUNT];
static bool options_saved;
static bool setup_clock(bool for_option_reload);

static uint64_t session_deadline;
static uint64_t stage_started;
static programmer_report_t report;
static void finish_stage(void) {
    uint64_t now = time_us_64();
    report.stage_ms[report.stage] += (uint32_t)((now - stage_started) / 1000u);
    stage_started = now;
}
static bool failed(programmer_result_t result, uint32_t detail) {
    report.result = result; report.detail = detail;
    return false;
}
static bool read32(uint32_t address, uint32_t *value) {
    report.address = address;
    return swd_read32(address, value) || failed(RESULT_SWD, swd_last_error());
}
static bool write32(uint32_t address, uint32_t value) {
    report.address = address;
    return swd_write32(address, value) || failed(RESULT_SWD, swd_last_error());
}
static bool time_available(void) {
    return time_us_64() < session_deadline || failed(RESULT_TIMEOUT, 0);
}
static bool wait_bits(uint32_t address, uint32_t mask, uint32_t expected, uint32_t ms) {
    uint64_t deadline = time_us_64() + (uint64_t)ms * 1000u;
    do {
        uint32_t value;
        if (!time_available() || !read32(address, &value)) return false;
        if ((value & mask) == expected) return true;
        busy_wait_us_32(50);
    } while (time_us_64() < deadline);
    return failed(RESULT_TIMEOUT, address);
}
static bool flash_wait(uint32_t ms) {
    if (!wait_bits(FLASH_SR, FLASH_BSY, 0, ms)) return false;
    uint32_t sr;
    if (!read32(FLASH_SR, &sr)) return false;
    if (sr & FLASH_ERRORS) return failed(RESULT_FLASH, sr);
    return true;
}
static bool require_halted(void) {
    uint32_t dhcsr;
    if (!read32(DHCSR, &dhcsr)) return false;
    return (dhcsr & (1u << 17)) != 0 || failed(RESULT_RESET, dhcsr);
}

static bool reset_and_halt(void) {
    swd_init(); /* Reinitialize PIO and direction state for every START/retry. */
    swd_set_frequency(SWD_CONNECT_KHZ);
    swd_reset_assert(true);
    sleep_ms(20);
    if (!swd_connect()) return failed(RESULT_SWD, swd_last_error());
    /* Catch reset before releasing the target, including an initially blank MCU.
     * A fixture NRST fault is not silently replaced with software reset.
     */
    if (!write32(DHCSR, 0xa05f0003u) || !write32(DEMCR, 1u)) return false;
    swd_reset_assert(false);
    if (!wait_bits(DHCSR, 1u << 17, 1u << 17, CONNECT_TIMEOUT_MS)) {
        /* A failed SWD read is not evidence that the reset pin failed. */
        if (report.result == RESULT_TIMEOUT) return failed(RESULT_RESET, report.detail);
        return false;
    }
    return true;
}

static bool identify_target(void) {
    uint32_t value;
    if (!read32(0xe0042000u, &value)) return false;
    if ((value & 0xfffu) != TARGET_DEVICE_ID) return failed(RESULT_TARGET, value);
    for (unsigned i = 0; i < 3; ++i)
        if (!read32(0x1fff7590u + 4u * i, &report.uid[i])) return false;
    report.uid_valid = true;
    if (!read32(0x1fff75e0u, &value)) return false;
    if ((value & 0xffffu) * 1024u != TARGET_FLASH_BYTES) return failed(RESULT_TARGET, value);
    return true;
}

static bool read_options(uint32_t *options) {
    for (size_t i = 0; i < OPTION_COUNT; ++i)
        if (!read32(option_addresses[i], &options[i])) return false;
    report.optr_after = options[0];
    return true;
}

static bool check_options(void) {
    uint32_t actual[OPTION_COUNT];
    if (!read_options(actual)) return false;
    for (size_t i = 0; i < OPTION_COUNT; ++i) {
        if (actual[i] != expected_options[i]) {
            report.address = option_addresses[i];
            return failed(RESULT_PROTECTED, actual[i]);
        }
    }
    return true;
}

static bool check_protection(const uint32_t *options) {
    uint32_t value = options[0];
    report.address = FLASH_OPTR;
    /* No RDP downgrade or protection removal. BFB2 is handled separately. */
    if ((value & 0xffu) != 0xaau)
        return failed(RESULT_PROTECTED, value);
    const unsigned wrp[] = {3, 4, 7, 8};
    for (size_t i = 0; i < 4; ++i) {
        value = options[wrp[i]];
        report.address = option_addresses[wrp[i]];
        if ((value & 0xffu) <= ((value >> 16) & 0xffu)) return failed(RESULT_PROTECTED, value);
    }
    const unsigned pcrop[] = {1, 5};
    for (size_t i = 0; i < 2; ++i) {
        value = options[pcrop[i]];
        uint32_t end = options[pcrop[i] + 1];
        report.address = option_addresses[pcrop[i]];
        if ((value & 0xffffu) <= (end & 0xffffu)) return failed(RESULT_PROTECTED, value);
    }
    return true;
}

static bool freeze_watchdogs(void) {
    uint32_t value;
    /* Freeze watchdogs while halted, including the diagnostic handoff. */
    return read32(0xe0042008u, &value) && write32(0xe0042008u, value | 0x1800u);
}

static bool wait_reload_halt(void) {
    /* Used ONLY after a valid fresh reset indication following OBL_LAUNCH.
     * Reset can disrupt subsequent SWD transfers too. Reestablish the link
     * before retrying debug control/status accesses; never replay option/flash
     * writes or issue another target reset. Reload can also clear debug enable:
     * restore vector catch and request halt, rather than only polling S_HALT.
     * Require two consecutive observations with debug enabled and CPU halted. */
    uint64_t deadline = time_us_64() + (uint64_t)CONNECT_TIMEOUT_MS * 1000u;
    bool connected = true, last_read_ok = true, halt_requested = false;
    unsigned halted_reads = 0;
    do {
        if (!time_available()) return false;
        if (!connected) {
            ++report.reload_reconnects;
            swd_init(); /* Also recovers a stopped PIO engine; preserves NRST. */
            swd_set_frequency(SWD_CONNECT_KHZ);
            connected = swd_connect();
        }
        uint32_t value;
        if (connected && !halt_requested) {
            /* These debug-control writes are idempotent and safe to repeat
             * after a lost ACK. Option writes/OBL_LAUNCH are never repeated. */
            connected = swd_write32(DEMCR, 1u) && swd_write32(DHCSR, 0xa05f0003u);
            halt_requested = connected;
        }
        if (connected && swd_read32(DHCSR, &value)) {
            last_read_ok = true;
            report.reload_dhcsr = value;
            if ((value & 0x20003u) == 0x20003u) {
                if (++halted_reads == 2) return true;
            } else {
                halted_reads = 0;
                halt_requested = false;
            }
        } else {
            last_read_ok = false;
            connected = false;
            halt_requested = false;
            halted_reads = 0;
        }
        sleep_ms(10);
    } while (time_us_64() < deadline);
    report.address = DHCSR;
    return last_read_ok ? failed(RESULT_TIMEOUT, DHCSR) :
                          failed(RESULT_SWD, swd_last_error());
}

static bool clear_bfb2(void) {
    uint32_t value, uid[3];
    memcpy(uid, report.uid, sizeof uid);
    report.bfb2_attempted = 1; /* Exactly one attempt; never retry an option write. */
    /* ES0250 section 2.2.23: OBL_LAUNCH can load corrupted options if MSI
     * is running at >= 8 MHz. Establish 4 MHz before touching option bytes;
     * main-flash setup restores 24 MHz only AFTER successful reload checks. */
    if (!setup_clock(true)) return false;
    /* Re-read every shadow before touching flash. Refuse unstable reads. */
    if (!check_options() || !require_halted() ||
        !wait_bits(FLASH_SR, FLASH_BSY, 0, OPTION_TIMEOUT_MS) || !read32(FLASH_SR, &value)) return false;
    if (value & 0x8000u) return failed(RESULT_FLASH, value); /* Invalid options: do not rewrite. */
    report.option_phase = 2;
    if (!write32(FLASH_SR, FLASH_ERRORS | 1u) || !read32(FLASH_CR, &value)) return false;
    if (value & FLASH_LOCK) {
        if (!write32(FLASH_KEYR, 0x45670123u) || !write32(FLASH_KEYR, 0xcdef89abu)) return false;
    }
    if (!read32(FLASH_CR, &value)) return false;
    if (value & FLASH_LOCK) return failed(RESULT_FLASH, value);
    if (value & FLASH_OPTLOCK) {
        if (!write32(FLASH_OPTKEYR, 0x08192a3bu) || !write32(FLASH_OPTKEYR, 0x4c5d6e7fu)) return false;
    }
    if (!read32(FLASH_CR, &value)) return false;
    if (value & (FLASH_LOCK | FLASH_OPTLOCK)) return failed(RESULT_FLASH, value);
    if (!write32(FLASH_CR, 0)) return false;
    report.option_phase = 3;
    expected_options[0] &= ~FLASH_BFB2; /* The ONLY option bit we change. */
    if (!write32(FLASH_OPTR, expected_options[0]) || !check_options() ||
        !write32(FLASH_CR, FLASH_OPTSTRT | FLASH_EOPIE) || !flash_wait(OPTION_TIMEOUT_MS)) return false;
    /* RM0351: EOP is generated only with EOPIE set. CPU stays halted, so no
     * interrupt handler executes. Require fresh completion, not just a shadow write. */
    if (!read32(FLASH_SR, &value)) return false;
    if (!(value & 1u)) return failed(RESULT_FLASH, value);
    if (!write32(FLASH_CR, 0) || !write32(FLASH_SR, 1u)) return false;

    report.option_phase = 4;
    /* Arm reset vector catch before option reload. If the reload clears debug
     * controls, wait_reload_halt must restore them; the application may have
     * run in that gap, so all options are checked again after halting.
     * Clear the old sticky reset flag BEFORE issuing OBL_LAUNCH.
     * No physical reset is issued below: a NEW S_RESET_ST proves OBL happened.
     */
    if (!write32(DEMCR, 1u) || !read32(DEMCR, &value)) return false;
    if (!(value & 1u)) return failed(RESULT_RESET, value);
    if (!write32(DHCSR, 0xa05f0003u) || !require_halted() ||
        !wait_bits(DHCSR, DHCSR_RESET_ST, 0, CONNECT_TIMEOUT_MS)) return false;
    /* A reset may interrupt the AP flush of THIS write. Never replay it. Its
     * effect must instead be proven by a fresh reset, halt and reloaded options.
     */
    report.option_phase = 7; /* Reload command attempted; reconnect next. */
    (void)swd_write32(FLASH_CR, FLASH_OBL_LAUNCH);
    uint64_t deadline = time_us_64() + (uint64_t)CONNECT_TIMEOUT_MS * 1000u;
    bool connected = false;
    do {
        sleep_ms(10);
        if (swd_connect() && swd_read32(DHCSR, &value)) { connected = true; break; }
    } while (time_us_64() < deadline && time_us_64() < session_deadline);
    report.address = DHCSR;
    if (!connected) return failed(RESULT_SWD, swd_last_error());
    report.reload_dhcsr = value;
    if (!(value & DHCSR_RESET_ST)) return failed(RESULT_RESET, value);
    report.option_phase = 8;
    if (!wait_reload_halt()) return false;
    report.option_phase = 9;
    if (!identify_target()) return false;
    if (memcmp(uid, report.uid, sizeof uid)) return failed(RESULT_TARGET, report.uid[0]);
    if (!check_options() || !read32(FLASH_SR, &value)) return false;
    if (value & FLASH_ERRORS) return failed(RESULT_FLASH, value);
    if (!write32(FLASH_CR, FLASH_OPTLOCK | FLASH_LOCK) || !read32(FLASH_CR, &value)) return false;
    if ((value & (FLASH_OPTLOCK | FLASH_LOCK)) != (FLASH_OPTLOCK | FLASH_LOCK))
        return failed(RESULT_FLASH, value);
    report.bfb2_cleared = 1;
    report.option_phase = 5;
    return freeze_watchdogs();
}

static bool connect_halt(void) {
    if (!reset_and_halt() || !identify_target()) return false;
    if (!options_saved) {
        report.option_phase = 1;
        if (!read_options(expected_options)) return false;
        report.optr_before = expected_options[0];
        if (!check_protection(expected_options)) return false;
        options_saved = true;
        if (!freeze_watchdogs()) return false;
        if ((expected_options[0] & FLASH_BFB2) && !clear_bfb2()) return false;
    } else {
        /* The standard-programming retry must never repeat option programming. */
        if (!check_options() || !freeze_watchdogs()) return false;
    }
    uint32_t value;
    if (!read32(0x40021060u, &value) || !write32(0x40021060u, value | 1u)) return false;
    if (!read32(0x40010000u, &value)) return false;
    if (value & 0x100u) return failed(RESULT_TARGET, value); /* FB_MODE swap */
    return true;
}

static bool setup_clock(bool for_option_reload) {
    uint32_t cr, cfgr, value;
    if (!require_halted()) return false;
    if (!read32(0x40021000u, &cr) || !read32(0x40021008u, &cfgr)) return false;
    report.initial_rcc_cr = cr;
    report.initial_rcc_cfgr = cfgr;
    /* Establish clocks explicitly: a valid halted application can still have
     * HSI/HSE/PLL selected or AHB division. Do not assume reset-default RCC.
     * Enable/read back the APB1 power interface before using PWR registers.
     */
    if (!read32(0x40021058u, &value) || !write32(0x40021058u, value | 0x10000000u)) return false;
    if (!wait_bits(0x40021058u, 0x10000000u, 0x10000000u, 100)) return false;
    if (!read32(0x40007000u, &value)) return false;
    report.initial_pwr_cr1 = value;
    unsigned vos = (value >> 9) & 3u;
    if (vos != 1 && vos != 2) return failed(RESULT_CLOCK, value);
    /* Keep the existing valid voltage range. Exit low-power run and wait for
     * the regulator to be ready BEFORE selecting a clock above 2 MHz.
     */
    if ((value & 0x4000u) && !write32(0x40007000u, value & ~0x4000u)) return false;
    if (!wait_bits(0x40007000u, 0x4000u, 0, 100) ||
        !wait_bits(0x40007014u, 0x600u, 0, 100)) return false;
    /* Disable and flush instruction/data caches. No CPU execution from flash
     * until reset; four wait states cover the previous normal clock while
     * switching sources. Three remain conservative at both 4 and 24 MHz.
     */
    if (!write32(FLASH_ACR, 4u) || !write32(FLASH_ACR, 0x1804u) ||
        !write32(FLASH_ACR, 4u) || !wait_bits(FLASH_ACR, 7u, 4u, 100)) return false;
    /* Use HSI16 as an independent intermediate source, so changing MSI does
     * not alter a running PLL input. Preserve prescalers until off the PLL.
     */
    if (!read32(0x40021000u, &cr) || !write32(0x40021000u, cr | 0x100u) ||
        !wait_bits(0x40021000u, 0x400u, 0x400u, 100)) return false;
    if (!write32(0x40021008u, (cfgr & ~3u) | 1u) ||
        !wait_bits(0x40021008u, 12u, 4u, 100)) return false;
    if (!read32(0x40021000u, &cr) || !write32(0x40021000u, cr & ~0x15000000u) ||
        !wait_bits(0x40021000u, 0x2a000000u, 0, 100)) return false;
    if (!read32(0x40021000u, &cr) || !write32(0x40021000u, cr | 1u) ||
        !wait_bits(0x40021000u, 2u, 2u, 100)) return false;
    if (!read32(0x40021000u, &cr)) return false;
    /* MSI range 6 = 4 MHz for option reload; range 9 = 24 MHz for the loader.
     * Select RCC_CR's range (MSIRGSEL), enable MSI and check its ready bit. */
    uint32_t msi_config = for_option_reload ? 0x69u : 0x99u;
    if (!write32(0x40021000u, (cr & ~0xf4u) | msi_config) ||
        !wait_bits(0x40021000u, 0xfbu, msi_config | 2u, 100)) return false;
    if (!read32(0x40021008u, &cfgr) || !write32(0x40021008u, cfgr & ~0xf3u) ||
        !wait_bits(0x40021008u, 0xffu, 0, 100)) return false;
    if (!write32(FLASH_ACR, 3u) || !wait_bits(FLASH_ACR, 7u, 3u, 100) ||
        !require_halted()) return false;
    swd_set_frequency(for_option_reload ? SWD_CONNECT_KHZ : SWD_PROGRAM_KHZ);
    return true;
}

static bool erase_flash(void) {
    uint32_t value;
    if (!wait_bits(FLASH_SR, FLASH_BSY, 0, ERASE_TIMEOUT_MS)) return false;
    if (!write32(FLASH_SR, FLASH_ERRORS | 1u) || !read32(FLASH_CR, &value)) return false;
    if (value & FLASH_LOCK) {
        if (!write32(FLASH_KEYR, 0x45670123u) || !write32(FLASH_KEYR, 0xcdef89abu)) return false;
    }
    if (!read32(FLASH_CR, &value)) return false;
    if (value & FLASH_LOCK) return failed(RESULT_FLASH, value);
    return write32(FLASH_CR, FLASH_MER) && write32(FLASH_CR, FLASH_MER | 0x10000u) &&
           flash_wait(ERASE_TIMEOUT_MS) && write32(FLASH_CR, 0);
}

#if !STM32_USE_RAM_LOADER
static bool program_image(const uint8_t *image, size_t size, programmer_progress_fn progress, bool use_fast) {
    (void)use_fast;
    if (!write32(FLASH_CR, 1)) return false;
    for (size_t offset = 0; offset < size; offset += 8) {
        if (!time_available()) return false;
        uint32_t lo, hi;
        memcpy(&lo, image + offset, 4); memcpy(&hi, image + offset + 4, 4);
        /* Erased all-ones words must stay physically erased: programming FF
         * consumes STM32L4 ECC bits and prevents later application writes.
         * They are still included in the full byte-by-byte verify below.
         */
        if (lo != 0xffffffffu || hi != 0xffffffffu) {
            report.address = TARGET_FLASH_BASE + (uint32_t)offset;
            if (!swd_write_doubleword(report.address, lo, hi))
                return failed(RESULT_SWD, swd_last_error());
            if (!flash_wait(WRITE_TIMEOUT_MS)) return false;
        }
        if (progress && !(offset & 1023u)) progress(STAGE_PROGRAM, (uint32_t)offset);
    }
    return write32(FLASH_CR, 0) && write32(FLASH_CR, FLASH_LOCK);
}
#else
_Static_assert(RAM_BLOCK_BYTES == LOADER_BUFFER_SIZE && RAM_BLOCK_BYTES % LOADER_FAST_ROW_SIZE == 0,
               "Host and target block size/alignment differ");
/* Core 1 has a 4 KiB stack. Keep the 16 KiB staging buffer in Pico BSS instead.
 * The single programming worker owns it; stm32_program is not reentrant.
 */
static uint32_t transfer_block[RAM_BLOCK_BYTES / 4];
static bool core_write(unsigned reg, uint32_t value) {
    return write32(0xe000edf8u, value) && write32(0xe000edf4u, reg | 0x10000u) &&
           wait_bits(DHCSR, 1u << 16, 1u << 16, 100);
}
static bool upload_loader(void) {
    if (!require_halted()) return false;
    if (!swd_write_words(LOADER_CODE_ADDRESS, target_loader, sizeof(target_loader) / 4))
        return failed(RESULT_SWD, swd_last_error());
    /* Never execute a loader that was not read back exactly. */
    uint32_t actual[64];
    for (size_t offset = 0; offset < sizeof(target_loader); offset += sizeof(actual)) {
        size_t n = sizeof(target_loader) - offset;
        if (n > sizeof(actual)) n = sizeof(actual);
        if (!swd_read_words(LOADER_CODE_ADDRESS + (uint32_t)offset, actual, n / 4))
            return failed(RESULT_SWD, swd_last_error());
        if (memcmp(actual, (const uint8_t *)target_loader + offset, n))
            return failed(RESULT_SWD, SWD_AP_ERROR);
    }
    return true;
}
static bool run_loader(uint32_t operation, uint32_t address, uint32_t size, uint32_t crc) {
    if (!require_halted() || !time_available()) return false;
    loader_mailbox_t m = { .operation = operation, .address = address, .size = size, .crc32 = crc };
    uint32_t words[sizeof(m) / 4];
    memcpy(words, &m, sizeof(m));
    if (!swd_write_words(LOADER_MAILBOX, words, sizeof(words) / 4))
        return failed(RESULT_SWD, swd_last_error());
    /* Mask interrupts while halted, then set every execution-context register
     * needed by the freestanding loader; the product vector table is not used.
     */
    if (!write32(DHCSR, 0xa05f000bu) || !core_write(20, 1) ||
        !core_write(17, LOADER_STACK_TOP) || !core_write(13, LOADER_STACK_TOP) ||
        !core_write(0, LOADER_MAILBOX) || !core_write(14, 0xffffffffu) ||
        !core_write(16, 0x01000000u) || !core_write(15, LOADER_CODE_ADDRESS) ||
        !write32(0xe000ed30u, 0x1fu) || !write32(DHCSR, 0xa05f0009u)) return false;
    if (!wait_bits(DHCSR, 1u << 17, 1u << 17, LOADER_TIMEOUT_MS)) return false;
    uint32_t dfsr;
    if (!read32(0xe000ed30u, &dfsr)) return false;
    if ((dfsr & 0x1fu) != 2u) return failed(RESULT_RESET, dfsr); /* Expected BKPT only. */
    if (!swd_read_words(LOADER_MAILBOX, words, sizeof(words) / 4))
        return failed(RESULT_SWD, swd_last_error());
    memcpy(&m, words, sizeof(m));
    report.address = m.failure_address;
    if (m.operation != operation || m.address != address || m.size != size || m.crc32 != crc)
        return failed(RESULT_SWD, SWD_AP_ERROR);
    report.loader_status = m.status;
    report.loader_completed = m.completed;
    if (m.status == LOADER_TIMEOUT) return failed(RESULT_TIMEOUT, m.detail);
    if (m.status == LOADER_BAD_DATA) return failed(RESULT_SWD, SWD_PARITY);
    if (m.status == LOADER_BAD_CLOCK) return failed(RESULT_CLOCK, m.detail);
    if (m.status != LOADER_SUCCESS || m.completed != size) return failed(RESULT_FLASH, m.detail);
    return true;
}
static unsigned rows_in_block(size_t count, bool *has_sparse, bool *has_data,
                              size_t *sparse_start, size_t *sparse_end) {
    unsigned dense_rows = 0;
    *has_sparse = *has_data = false;
    *sparse_start = count; *sparse_end = 0;
    for (size_t row = 0; row < count; row += LOADER_FAST_ROW_SIZE) {
        size_t end = row + LOADER_FAST_ROW_SIZE;
        if (end > count) end = count;
        bool dense = end - row == LOADER_FAST_ROW_SIZE, populated = false;
        for (size_t i = row / 4; i < end / 4; i += 2) {
            bool erased = transfer_block[i] == 0xffffffffu && transfer_block[i+1] == 0xffffffffu;
            if (erased) dense = false;
            else populated = true;
        }
        if (dense) ++dense_rows;
        if (populated && !dense) {
            if (!*has_sparse) *sparse_start = row;
            *sparse_end = end;
            *has_sparse = true;
        }
        if (populated) *has_data = true;
    }
    return dense_rows;
}
static bool program_image(const uint8_t *image, size_t size, programmer_progress_fn progress, bool use_fast) {
    /* All fast rows first, in ascending addresses. Leave FSTPG set across
     * rows, skipped erased regions and SRAM uploads. Only after that session
     * ends may we program the sparse rows with PG. This avoids toggling the
     * controller's fast sequence at every row/block or interleaving PG/FSTPG.
     */
    for (unsigned pass = 0; pass < (use_fast ? 2u : 1u); ++pass) {
        uint32_t operation = !use_fast ? LOADER_PROGRAM :
                             pass == 0 ? LOADER_PROGRAM_FAST : LOADER_PROGRAM_SPARSE;
        for (size_t offset = 0; offset < size; offset += RAM_BLOCK_BYTES) {
            if (!time_available() || !require_halted()) return false;
            size_t count = size - offset;
            if (count > sizeof(transfer_block)) count = sizeof(transfer_block);
            memcpy(transfer_block, image + offset, count);
            bool sparse, data;
            size_t sparse_start, sparse_end;
            unsigned dense = rows_in_block(count, &sparse, &data, &sparse_start, &sparse_end);
            bool needed = operation == LOADER_PROGRAM_FAST ? dense != 0 :
                          operation == LOADER_PROGRAM_SPARSE ? sparse : data;
            if (needed) {
                size_t first = 0, bytes = count;
                if (operation == LOADER_PROGRAM_SPARSE) {
                    /* Trim whole leading/trailing rows, preserving alignment
                     * and every sparse byte. Internal dense rows are still
                     * skipped by the loader, never programmed a second time.
                     */
                    first = sparse_start; bytes = sparse_end - first;
                    memmove(transfer_block, (const uint8_t *)transfer_block + first, bytes);
                }
                report.address = TARGET_FLASH_BASE + (uint32_t)(offset + first);
                if (!swd_write_words(LOADER_BUFFER, transfer_block, bytes / 4))
                    return failed(RESULT_SWD, swd_last_error());
                if (!run_loader(operation, report.address,
                                (uint32_t)bytes, image_crc32(image + offset + first, bytes))) return false;
                if (operation == LOADER_PROGRAM_FAST) report.fast_rows += dense;
            }
            if (progress) progress(STAGE_PROGRAM, (uint32_t)(offset + count));
        }
        /* Every helper call has halted after BSY cleared. End the fast session
         * once, before any standard writes or flash readback. */
        if (!write32(FLASH_CR, 0)) return false;
    }
    return write32(FLASH_CR, FLASH_LOCK);
}
#endif

static bool blank_check(void) {
#if STM32_USE_RAM_LOADER
    return run_loader(LOADER_BLANK_CHECK, TARGET_FLASH_BASE, TARGET_FLASH_BYTES, 0);
#else
    uint32_t words[64];
    for (uint32_t offset = 0; offset < TARGET_FLASH_BYTES; offset += sizeof(words)) {
        if (!require_halted() || !time_available()) return false;
        if (!swd_read_words(TARGET_FLASH_BASE + offset, words, 64))
            return failed(RESULT_SWD, swd_last_error());
        for (unsigned i = 0; i < 64; ++i) if (words[i] != 0xffffffffu) {
            report.address = TARGET_FLASH_BASE + offset + i * 4;
            return failed(RESULT_FLASH, words[i]);
        }
    }
    return true;
#endif
}

static bool verify_image(const uint8_t *image, size_t size, programmer_progress_fn progress) {
    /* One TAR window per read, with a halt check before every 1 KiB and after
     * the final block. Static storage keeps the 4 KiB worker stack bounded.
     * Every byte (including FF) is still transferred and compared on the Pico.
     */
    static uint32_t buffer[256];
    for (size_t offset = 0; offset < size; offset += sizeof(buffer)) {
        if (!time_available() || !require_halted()) return false;
        size_t count = size - offset;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        report.address = TARGET_FLASH_BASE + (uint32_t)offset;
        if (!swd_read_words(report.address, buffer, count / 4))
            return failed(RESULT_SWD, swd_last_error());
        const uint8_t *actual = (const uint8_t *)buffer;
        for (size_t i = 0; i < count; ++i) {
            if (actual[i] != image[offset + i]) {
                report.address += (uint32_t)i;
                return failed(RESULT_VERIFY, ((uint32_t)image[offset+i] << 8) | actual[i]);
            }
        }
        if (progress) progress(STAGE_VERIFY, (uint32_t)(offset + count));
    }
    return require_halted();
}

programmer_report_t stm32_program(const uint8_t *image, size_t size, programmer_progress_fn progress) {
    uint64_t start = time_us_64();
    session_deadline = start + (uint64_t)SESSION_TIMEOUT_MS * 1000u;
    report = (programmer_report_t){ .result = RESULT_OK, .stage = STAGE_CONNECT };
    options_saved = false;
    bool use_fast = STM32_USE_RAM_LOADER && STM32_USE_FAST_PROGRAM;
    stage_started = start;
    uint32_t sp = 0, vector = 0;
    if (size >= 8) { memcpy(&sp, image, 4); memcpy(&vector, image + 4, 4); }
    if (size < 8 || size > TARGET_FLASH_BYTES || (size & 7u) || !(vector & 1u) ||
        (vector & ~1u) < TARGET_FLASH_BASE || (vector & ~1u) >= TARGET_FLASH_BASE + size ||
        (sp & 7u) || !((sp > 0x20000000u && sp <= 0x20018000u) ||
                       (sp > 0x10000000u && sp <= 0x10008000u))) {
        failed(RESULT_IMAGE, (uint32_t)size);
        return report;
    }
retry_from_reset:
    if (progress) progress(STAGE_CONNECT, 0);
    if (!connect_halt() || !setup_clock(false)) goto fail;
#if STM32_USE_RAM_LOADER
    if (!upload_loader()) goto fail;
#endif
    finish_stage(); report.stage = STAGE_ERASE;
    if (progress) progress(STAGE_ERASE, 0);
    if (!require_halted() || !check_options() || !erase_flash() || !blank_check()) goto fail;
    finish_stage(); report.stage = STAGE_PROGRAM;
    if (progress) progress(STAGE_PROGRAM, 0);
    if (!program_image(image, size, progress, use_fast)) {
        /* A fast-specific controller/completion error can leave a partial row.
         * Never replay writes over it. Save the original evidence, reset/halt,
         * then MASS ERASE and blank-check again before ONE standard-only retry.
         * No retry for SWD, CRC, protection, timeout or verification failures.
         */
        if (!use_fast || report.result != RESULT_FLASH ||
            (report.loader_status != LOADER_FLASH_ERROR && report.loader_status != LOADER_NO_EOP))
            goto fail;
        report.fast_error_address = report.address;
        report.fast_error_detail = report.detail;
        report.fast_error_status = report.loader_status;
        report.standard_retry = 1;
        report.fast_rows = 0;
        use_fast = false;
        finish_stage(); report.stage = STAGE_CONNECT;
        report.result = RESULT_OK; report.detail = 0;
        report.loader_status = LOADER_PENDING; report.loader_completed = 0;
        goto retry_from_reset;
    }
    finish_stage(); report.stage = STAGE_VERIFY;
    if (progress) progress(STAGE_VERIFY, 0);
    if (!verify_image(image, size, progress)) goto fail;
    report.option_phase = 6;
    if (!check_options()) goto fail; /* Never signal green after an option change. */
    finish_stage(); report.stage = STAGE_RESTART;
    if (progress) progress(STAGE_RESTART, 0);
#if STM32_HALT_AFTER_VERIFY
    /* Diagnostic handoff: never reset or disable debugging after comparison.
     * Keep C_DEBUGEN/C_HALT asserted and confirm S_HALT before releasing SWD.
     * NRST is already released. Watchdog debug-freeze and reset catch remain
     * enabled; a second probe must attach without resetting/resuming the CPU.
     */
    if (!write32(DHCSR, 0xa05f0003u) || !require_halted()) goto fail;
    report.target_halted = 1;
    swd_release_lines();
#else
    /* Assert reset BEFORE disabling debug: never let the old PC/application
     * run in the gap between verification and the final hardware reset.
     */
    swd_reset_assert(true);
    if (!write32(DEMCR, 0) || !write32(DHCSR, 0xa05f0000u)) goto fail;
    sleep_ms(20);
    swd_release_lines();
    swd_reset_assert(false);
#endif
    finish_stage();
    report.elapsed_ms = (uint32_t)((time_us_64() - start) / 1000u);
    return report;
fail:
    /* Never run a partly programmed image. Red state keeps reset asserted.
     * A new START job reinitializes the link. No further retry or RDP changes.
     */
    swd_reset_assert(true);
    swd_release_lines();
    finish_stage();
    report.elapsed_ms = (uint32_t)((time_us_64() - start) / 1000u);
    return report;
}
