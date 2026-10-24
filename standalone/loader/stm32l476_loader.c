/* Executed only from STM32 SRAM1: code, constants, stack and input data.
 * No application code, interrupts, C runtime or flash-resident reads/calls.
 * Pico mass-erases both banks, blank-checks them, and uploads a complete block
 * plus CRC before starting this routine. It only polls debug registers while
 * we run; flash readback takes place after BKPT. See RM0351 fast programming.
 */
#include "loader_protocol.h"
#include "crc32_nibble.h"
#define REG32(a) (*(volatile uint32_t *)(a))
#define FLASH_SR REG32(0x40022010u)
#define FLASH_CR REG32(0x40022014u)
#define RCC_CR REG32(0x40021000u)
#define RCC_CFGR REG32(0x40021008u)
#define FLASH_PG 1u
#define FLASH_FSTPG (1u << 18)
#define FLASH_EOP 1u
/* RM0351 FLASH_SR.EOP is set only when FLASH_CR.EOPIE is enabled. We poll the
 * flag with PRIMASK=1; enabling its generation must not execute a flash ISR.
 */
#define FLASH_EOPIE (1u << 24)
#define ERROR_MASK 0x0000c3fau
#define BUSY 0x10000u
#define POLL_LIMIT 2400000u /* Bounded independently of the Pico's wall-clock timeout. */

static void feed_iwdg(void) { REG32(0x40003000u) = 0xaaaau; }
static uint32_t crc32(const volatile uint8_t *p, uint32_t n) {
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0; i < n; ++i) {
        crc = crc32_update_byte(crc, p[i]);
        if (!(i & 255u)) feed_iwdg();
    }
    return ~crc;
}
static uint32_t wait_flash(volatile loader_mailbox_t *m) {
    uint32_t polls = POLL_LIMIT;
    while ((FLASH_SR & BUSY) && --polls) feed_iwdg();
    m->detail = FLASH_SR;
    if (!polls) return LOADER_TIMEOUT;
    return (m->detail & ERROR_MASK) ? LOADER_FLASH_ERROR : LOADER_SUCCESS;
}

static uint32_t finish_write(volatile loader_mailbox_t *m, int require_eop) {
    uint32_t result = wait_flash(m);
    if (result == LOADER_SUCCESS) {
        /* A same-bank read can abort fast programming without an error bit.
         * Require a NEW completion flag; a stale EOP is cleared before writes.
         * Full host byte verification remains mandatory after this check.
         */
        if (require_eop && !(m->detail & FLASH_EOP)) result = LOADER_NO_EOP;
        if (m->detail & FLASH_EOP) FLASH_SR = FLASH_EOP;
    }
    /* FSTPG is a session setting, not a per-row pulse. Keep it enabled after
     * successful fast rows, including while the Pico uploads the next block.
     * Host ends the fast pass explicitly before sparse writes or verification.
     */
    if ((!require_eop || result != LOADER_SUCCESS) && !(FLASH_SR & BUSY)) FLASH_CR = 0;
    return result;
}

static int dense_row(uint32_t source) {
    /* Programming an all-ones double word consumes ECC bits. Use standard
     * programming for any row containing one, preserving erased user settings.
     * Test ONLY the SRAM source, never flash while FSTPG is active.
     */
    for (uint32_t i = 0; i < LOADER_FAST_ROW_SIZE; i += 8)
        if (REG32(source + i) == 0xffffffffu &&
            REG32(source + i + 4) == 0xffffffffu) return 0;
    return 1;
}

static uint32_t program_row(volatile loader_mailbox_t *m, uint32_t dest, uint32_t source) {
    FLASH_SR = FLASH_EOP;
    if (!(FLASH_CR & FLASH_FSTPG))
        FLASH_CR = FLASH_FSTPG | FLASH_EOPIE; /* Set once for the fast pass. */
    __asm volatile("dsb" ::: "memory");
    /* 64 ordered 32-bit stores = 32 double words in one aligned 256-byte row.
     * Keep this loop tight: no SWD, watchdog, status poll or function call
     * between words. At 24 MHz it meets the RM0351 data-delivery requirement.
     * Interrupts are masked for the entire loader invocation.
     */
    for (uint32_t i = 0; i < LOADER_FAST_ROW_SIZE; i += 4)
        REG32(dest + i) = REG32(source + i);
    __asm volatile("dsb" ::: "memory");
    return finish_write(m, 1);
}

__attribute__((section(".text.entry"), used, noreturn))
void loader_entry(volatile loader_mailbox_t *m) {
    __asm volatile("cpsid i" ::: "memory");
    uint32_t result = LOADER_INVALID;
    m->completed = 0;
    m->failure_address = m->address;
    m->detail = 0;
    if ((m->address & 7u) || (m->size & 7u) || !m->size ||
        m->address < 0x08000000u || m->address >= 0x08080000u ||
        m->size > 0x08080000u - m->address) goto done;
    if (m->operation == LOADER_BLANK_CHECK) {
        for (uint32_t offset = 0; offset < m->size; offset += 4) {
            uint32_t word = REG32(m->address + offset);
            if (word != 0xffffffffu) {
                result = LOADER_NOT_BLANK; m->detail = word;
                m->failure_address = m->address + offset;
                goto done;
            }
            if (!(offset & 255u)) feed_iwdg();
        }
        m->completed = m->size; result = LOADER_SUCCESS; goto done;
    }
    if ((m->operation != LOADER_PROGRAM && m->operation != LOADER_PROGRAM_FAST &&
         m->operation != LOADER_PROGRAM_SPARSE) ||
        m->size > LOADER_BUFFER_SIZE) goto done;
    if (crc32((const volatile uint8_t *)LOADER_BUFFER, m->size) != m->crc32) {
        result = LOADER_BAD_DATA; goto done;
    }
    if (FLASH_CR & 0x80000000u) { result = LOADER_FLASH_ERROR; m->detail = FLASH_CR; goto done; }
    result = wait_flash(m);
    if (result != LOADER_SUCCESS) goto done;
    /* Never mix PG with an active fast session. Host must end that pass first. */
    if (m->operation != LOADER_PROGRAM_FAST && (FLASH_CR & FLASH_FSTPG)) {
        result = LOADER_INVALID; goto done;
    }
    /* Host establishes MSI 24 MHz, AHB divide-by-one, normal run mode before
     * erase. Refuse fast mode if these explicit preconditions were lost.
     */
    if (m->operation == LOADER_PROGRAM_FAST &&
        ((RCC_CR & 0xfbu) != 0x9bu || (RCC_CFGR & 0xffu) != 0 ||
         (REG32(0x40007000u) & 0x4000u) || (REG32(0x40007014u) & 0x600u))) {
        result = LOADER_BAD_CLOCK; m->detail = RCC_CR; goto done;
    }
    for (uint32_t offset = 0; offset < m->size;) {
        uint32_t dest = m->address + offset, source = LOADER_BUFFER + offset;
        m->failure_address = dest;
        int full_dense_row = !(dest & (LOADER_FAST_ROW_SIZE - 1u)) &&
            m->size - offset >= LOADER_FAST_ROW_SIZE && dense_row(source);
        if (full_dense_row && m->operation != LOADER_PROGRAM) {
            if (m->operation == LOADER_PROGRAM_FAST) {
                result = program_row(m, dest, source);
                if (result != LOADER_SUCCESS) break; /* No retry of a partial row. */
            } /* Sparse pass skips rows already written by the fast pass. */
            offset += LOADER_FAST_ROW_SIZE;
            m->completed = offset;
            feed_iwdg();
            continue;
        }
        if (m->operation == LOADER_PROGRAM_FAST) {
            /* Skip the entire sparse/partial row. Do not rediscover a dense
             * suffix as a new fast row. No flash write occurs in this branch.
             */
            uint32_t skip = LOADER_FAST_ROW_SIZE - (dest & (LOADER_FAST_ROW_SIZE - 1u));
            if (skip > m->size - offset) skip = m->size - offset;
            offset += skip; m->completed = offset; feed_iwdg();
            continue;
        }
        uint32_t lo = REG32(LOADER_BUFFER + offset), hi = REG32(LOADER_BUFFER + offset + 4);
        if (lo != 0xffffffffu || hi != 0xffffffffu) {
            FLASH_SR = FLASH_EOP;
            /* Standard recovery uses the established polling sequence: BSY
             * clears and errors are absent, followed by full host readback.
             * EOP is optional in polling mode, as in ST HAL_FLASH_Program.
             */
            FLASH_CR = FLASH_PG;
            REG32(dest) = lo;
            /* ST's standard double-word sequence uses an ISB between halves. */
            __asm volatile("isb" ::: "memory");
            REG32(dest + 4) = hi;
            __asm volatile("dsb" ::: "memory");
            result = finish_write(m, 0);
            if (result != LOADER_SUCCESS) break;
        }
        offset += 8;
        m->completed = offset;
        feed_iwdg();
    }
    if ((m->operation != LOADER_PROGRAM_FAST || result != LOADER_SUCCESS) &&
        !(FLASH_SR & BUSY)) FLASH_CR = 0;
done:
    m->status = result;
    __asm volatile("dsb\n bkpt #0" ::: "memory");
    for (;;) __asm volatile("b .");
}
