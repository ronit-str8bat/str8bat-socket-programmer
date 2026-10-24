/* GPIO trace tests of the actual SWD host: posted reads, TAR wrapping, bit
 * order, parity failures, WAIT timeout, FAULT and invalid ACK handling.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "swd.h"
#include "programmer_config.h"
#include "hardware/gpio.h"
static bool direction[32], level[32];
static unsigned incoming[65536], outgoing[262144], in_count, in_pos, out_count;
static uint64_t ticks;
static bool always_wait;
static uint64_t oe_enabled_at;
static unsigned oe_enables;
uint64_t time_us_64(void) { return ticks += 100; }
void busy_wait_us_32(uint32_t us) { ticks += us; }
void sleep_ms(uint32_t ms) { ticks += ms * 1000u; }
void gpio_init(unsigned p) { assert(p != 6 && p != STATUS_LED_PIN); direction[p] = false; level[p] = false; }
void gpio_disable_pulls(unsigned p) { (void)p; }
void gpio_set_drive_strength(unsigned p, unsigned s) { (void)p; (void)s; }
void gpio_set_slew_rate(unsigned p, unsigned s) { (void)p; (void)s; }
void gpio_set_dir(unsigned p, bool output) {
    assert(p != 6 && p != STATUS_LED_PIN); /* GP4 is OE; SWD must not touch GP5/6. */
    if (p == SWDIO_PIN) assert(!level[SWCLK_PIN]);
    direction[p] = output;
}
void gpio_put(unsigned p, bool v) {
    assert(p != 6 && p != STATUS_LED_PIN);
    if (p == SWD_OE_PIN && v && !level[p]) {
        assert(direction[p]);
        oe_enabled_at=ticks; ++oe_enables;
    }
    if (p == SWCLK_PIN && v) {
        assert(direction[SWD_OE_PIN] && level[SWD_OE_PIN]);
        assert(ticks-oe_enabled_at >= SWD_OE_SETTLE_US);
    }
    if (p == SWCLK_PIN && v && direction[SWDIO_PIN]) {
        if (!always_wait) {
            assert(out_count < sizeof(outgoing)/sizeof(outgoing[0]));
            outgoing[out_count++] = level[SWDIO_PIN];
        }
    }
    /* Target changes its driven bit after each rising edge. Sampling GPIO
     * must not itself advance the target, or a wrong sampling phase is hidden.
     * The queued stream includes turnaround cycles as well as ACK/data bits.
     */
    if (p == SWCLK_PIN && v && !level[p] && !direction[SWDIO_PIN]) {
        if (!always_wait) assert(in_pos < in_count);
        ++in_pos;
    }
    level[p] = v;
}
bool gpio_get(unsigned p) {
    if (direction[p]) return level[p];
    assert(p == SWDIO_PIN);
    assert(!level[SWCLK_PIN]); /* Read before the next target update edge. */
    if (always_wait) { unsigned pattern[] = {0,0,1,0,0}; return pattern[in_pos % 5]; }
    assert(in_pos < in_count);
    return incoming[in_pos];
}
#if SWD_USE_PIO
/* Protocol tests also exercise the PIO call path. The assembled PIO program
 * itself has an independent instruction-level test in test_pio.py. */
void swd_pio_init(void) {}
void swd_pio_frequency(unsigned khz) { (void)khz; }
void swd_pio_direction(bool tx) { if (tx) gpio_put(SWDIO_PIN,1); gpio_set_dir(SWDIO_PIN,tx); }
void swd_pio_clock_enable(bool on) { gpio_set_dir(SWCLK_PIN,on); }
bool swd_pio_out(uint32_t data, unsigned count) {
    for (unsigned i=0;i<count;++i) {
        gpio_put(SWDIO_PIN,data&1); gpio_put(SWCLK_PIN,1); gpio_put(SWCLK_PIN,0); data>>=1;
    }
    return true;
}
bool swd_pio_in(uint32_t *data, unsigned count) {
    *data=0;
    for (unsigned i=0;i<count;++i) {
        *data|=(uint32_t)gpio_get(SWDIO_PIN)<<i;
        gpio_put(SWCLK_PIN,1); gpio_put(SWCLK_PIN,0);
    }
    return true;
}
#endif
static void bits(uint32_t value, unsigned count) {
    while (count--) { incoming[in_count++] = value & 1; value >>= 1; }
}
static void response(unsigned ack, bool read, uint32_t data, bool bad_parity) {
    bits(0, 1); bits(ack, 3);
    if (ack == 1 && read) { bits(data, 32); bits(__builtin_parity(data) ^ bad_parity, 1); }
    bits(0, 1);
}
static uint32_t sent(unsigned offset, unsigned count) {
    uint32_t v = 0;
    for (unsigned i=0; i<count; ++i) v |= outgoing[offset+i] << i;
    return v;
}
static void reset_fake(void) {
    memset(direction, 0, sizeof direction); memset(level, 0, sizeof level);
    in_count=in_pos=out_count=0; ticks=0; always_wait=false;
    oe_enabled_at=0; oe_enables=0;
    swd_init();
    assert(direction[SWD_OE_PIN] && !level[SWD_OE_PIN]);
}
int main(void) {
    reset_fake();
    assert(START_PIN == 18);
    assert(SWD_OE_PIN == 4);
    swd_reset_assert(true);
    assert(direction[NRST_PIN] && !level[NRST_PIN]);
    /* Releasing SWD must preserve the NRST hold. */
    swd_release_lines();
    assert(direction[NRST_PIN] && !level[NRST_PIN]);
    /* Retrying a failed job must preserve reset throughout SWD initialization. */
    swd_init();
    assert(direction[NRST_PIN] && !level[NRST_PIN]);
    swd_reset_assert(false);
    assert(!direction[NRST_PIN]);
    response(1,true,0x2ba01477,false); /* DPIDR */
    response(1,false,0,false); /* ABORT */
    response(1,false,0,false); /* SELECT */
    response(1,false,0,false); /* Power request */
    response(1,true,0xa0000000,false); /* Power ACK */
    response(1,false,0,false); /* AP CSW */
    response(1,true,0,false); /* Flush RDBUFF */
    response(1,true,0xa0000000,false); /* Flush CTRL/STAT */
    assert(swd_connect() && in_pos==in_count);
    assert(level[SWD_OE_PIN] && oe_enables==1); /* No toggles at RX/TX turnarounds. */
    assert(sent(64,16)==0xe79e && sent(146,8)==0xa5);
    swd_reset_assert(true);
    swd_reset_assert(false);
    swd_reset_assert(true); /* Failure cleanup with target held in reset. */
    swd_release_lines();
    assert(direction[NRST_PIN] && !level[NRST_PIN]);
    assert(!direction[SWCLK_PIN] && !direction[SWDIO_PIN]);
    assert(!level[SWD_OE_PIN]);
    reset_fake();
    response(1,false,0,false); /* TAR */
    response(1,true,0xdeadbeef,false); /* Posted AP read: old value */
    response(1,true,0x12345678,false); /* RDBUFF */
    response(1,true,0xa0000000,false); /* DP CTRL/STAT */
    uint32_t value;
    assert(swd_read32(0x08000000,&value) && value==0x12345678);
    assert(in_pos==in_count);
    assert(sent(0,8)==0x8b); /* AP TAR write request */
    assert(sent(8,32)==0x08000000);
    reset_fake();
    response(1,false,0,false); response(1,true,0,false);
    response(1,true,0x11111111,false); response(1,true,0xa0000000,false);
    response(1,false,0,false); response(1,true,0,false);
    response(1,true,0x22222222,false); response(1,true,0xa0000000,false);
    uint32_t values[2];
    assert(swd_read_words(0x080003fc,values,2));
    assert(values[0]==0x11111111 && values[1]==0x22222222 && in_pos==in_count);
    reset_fake();
    response(1,false,0,false); response(1,true,0,true);
    assert(!swd_read32(0x08000000,&value) && swd_last_error()==SWD_PARITY);
    reset_fake(); response(4,false,0,false);
    assert(!swd_read32(0,&value) && swd_last_error()==SWD_FAULT);
    swd_reset_assert(true); swd_release_lines(); /* The job's failure cleanup. */
    assert(!level[SWD_OE_PIN] && direction[NRST_PIN]);
    swd_init(); /* The job's recovery/reconnect must start with OE low. */
    assert(!level[SWD_OE_PIN] && direction[NRST_PIN]);
    in_count=in_pos=out_count=0;
    response(1,false,0,false); response(1,true,0,false);
    response(1,true,0x12345678,false); response(1,true,0xa0000000,false);
    assert(swd_read32(0x08000000,&value) && level[SWD_OE_PIN]);
    reset_fake(); always_wait=true;
    assert(!swd_read32(0,&value) && swd_last_error()==SWD_WAIT_TIMEOUT);
    assert(ticks < 100000);
    reset_fake(); bits(0,1); bits(7,3); bits(0,32); bits(0,2);
    assert(!swd_read32(0,&value) && swd_last_error()==SWD_BAD_ACK);
    reset_fake();
    /* Block write crosses TAR's 1 KiB auto-increment wrap. */
    for (unsigned i=0;i<2;++i) {
        response(1,false,0,false); response(1,false,0,false);
        response(1,true,0,false); response(1,true,0xa0000000,false);
    }
    uint32_t block[]={0x12345678,0xabcdef01};
    assert(swd_write_words(0x200003fc,block,2) && in_pos==in_count);
    assert(sent(8,32)==0x200003fc && sent(57,32)==block[0]);
    assert(sent(138,32)==0x20000400 && sent(187,32)==block[1]);
    reset_fake();
    response(1,false,0,false); /* TAR */
    response(2,false,0,false); /* First DRW: WAIT, no data accepted */
    response(1,false,0,false); /* Same DRW retried */
    response(1,false,0,false); /* Next word */
    response(1,true,0,false); response(1,true,0xa0000000,false);
    assert(swd_write_words(0x20001000,block,2) && in_pos==in_count);
    assert(sent(73,32)==block[0] && sent(122,32)==block[1]);
    reset_fake();
    response(1,false,0,false); response(4,false,0,false);
    assert(!swd_write_words(0x20001000,block,2) && swd_last_error()==SWD_FAULT);
    assert(out_count==65); /* No data or subsequent word written after FAULT. */
    reset_fake();
    assert(!swd_write_words(0x0807e800,block,2) && out_count==0); /* Raw flash burst prohibited. */
    assert(!swd_write_words(0x20017ffc,block,2) && out_count==0);
    assert(!swd_read_words(0xfffffffc,block,2) && out_count==0);
    /* Real 16 KiB transfer: TAR must be reloaded at EACH of 16 boundaries,
     * preserving all 4096 words and flushing posted writes in every chunk.
     */
    reset_fake();
    static uint32_t large[4096];
    for (unsigned chunk=0;chunk<16;++chunk) {
        response(1,false,0,false); /* TAR */
        for (unsigned i=0;i<256;++i) {
            large[chunk*256+i]=0x55000000u+chunk*256+i;
            response(1,false,0,false);
        }
        response(1,true,0,false); response(1,true,0xa0000000,false);
    }
    assert(swd_write_words(0x20001000,large,4096) && in_pos==in_count);
    for (unsigned chunk=0;chunk<16;++chunk) {
        unsigned first=chunk*(49u*257u+32u);
        assert(sent(first+8,32)==0x20001000+chunk*1024);
        for (unsigned i=0;i<256;++i)
            assert(sent(first+49u*(i+1u)+8u,32)==large[chunk*256+i]);
    }
    puts("SWD clock-phase, 16 KiB/16 TAR chunks, WAIT retry and FAULT-stop tests passed");
}
