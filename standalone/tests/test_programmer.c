/* Full-size fixture model for the actual host flash state machine. SRAM loader
 * execution is modeled here; tests/test_loader.py checks the compiled ARM code.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "stm32_programmer.h"
#include "programmer_config.h"
#include "swd.h"
#include "loader_protocol.h"
#include "crc32.h"
static uint8_t flash[TARGET_FLASH_BYTES], ram[0x6000];
static uint64_t ticks;
static uint32_t flash_cr, acr, cr, cfgr, pwr_cr1, apb1enr1, core_regs[21], dcrdr;
static bool hsi_stuck, msi_stuck, regulator_stuck, clock_switch_stuck, option_clock_stuck;
static bool reset_held, halted, erased, stuck_busy, protected, bad_size, link_fail;
static bool incomplete_erase, corrupt_upload, loader_timeout, loader_flash_error, lose_halt, fail_block;
static bool fast_only_error, fast_no_eop, retry_erase_failure, retry_verify_failure;
static uint32_t tamper_address;
static unsigned writes, verify_bytes, step, uploaded_blocks, blank_checked, largest_upload;
static unsigned erase_count, progress_restarts;
static unsigned uploaded_bytes, verify_calls;
static unsigned fast_calls;
static bool sparse_started;
static bool final_halt_lost, post_verify;
static unsigned post_verify_resets, post_verify_resumes, line_releases;
/* Option-register shadows and nonvolatile contents are separate: changing a
 * shadow alone must never pass the reload/persistence checks. */
static const uint32_t option_addrs[] = {0x40022020u,0x40022024u,0x40022028u,
    0x4002202cu,0x40022030u,0x40022044u,0x40022048u,0x4002204cu,0x40022050u};
static uint32_t option_shadow[9], option_nvm[9], option_initial[9], option_sr, demcr;
static unsigned option_keys, main_keys, option_programs, option_reloads, option_writes, option_reads;
static bool reset_seen, option_unlock_fail, option_program_error, option_busy, option_no_eop;
static bool reload_no_reset, reload_lost_ack, reload_link_fail, reload_no_halt, reload_wrong_uid;
static bool option_not_saved, option_other_changed, option_unstable, option_changed_after_flash;
static unsigned dhcsr_failure, reload_dhcsr_reads;
static unsigned transient_halt_errors, reload_reconnects;
static bool reload_needs_connect;
static bool reload_debug_lost;
static uint32_t dhcsr_control;
static unsigned reload_control_failure;
static swd_error_t injected_error;
uint64_t time_us_64(void) { return ticks += 10; }
void busy_wait_us_32(uint32_t us) { ticks += us; }
void sleep_ms(uint32_t ms) { ticks += ms * 1000u; }
void swd_init(void) {}
void swd_set_half_period(unsigned us) { (void)us; }
void swd_set_frequency(unsigned khz) { (void)khz; }
void swd_reset_assert(bool value) {
    reset_held = value;
    if (value && post_verify) ++post_verify_resets;
}
void swd_release_lines(void) { ++line_releases; }
bool swd_connect(void) {
    assert(reset_held || option_reloads); /* Reload reconnect does not issue its own reset. */
    if (reload_needs_connect) { ++reload_reconnects; reload_needs_connect=false; }
    return !link_fail && !(option_reloads && reload_link_fail);
}
swd_error_t swd_last_error(void) { return injected_error; }
static void emulate_loader(void) {
    assert(core_regs[0] == LOADER_MAILBOX && core_regs[15] == LOADER_CODE_ADDRESS);
    assert(core_regs[16] == 0x01000000 && core_regs[20] == 1 && core_regs[17] == LOADER_STACK_TOP);
    loader_mailbox_t m; memcpy(&m, ram + 0x800, sizeof(m));
    m.status = LOADER_SUCCESS; m.failure_address = m.address;
    assert(erased && m.address >= TARGET_FLASH_BASE && m.size <= TARGET_FLASH_BYTES);
    if (m.operation == LOADER_BLANK_CHECK) {
        for (uint32_t i = 0; i < m.size; ++i) if (flash[i] != 0xff) {
            m.status = LOADER_NOT_BLANK; m.failure_address = TARGET_FLASH_BASE + (i & ~3u); break;
        }
        blank_checked = m.size;
    } else {
        if (STM32_USE_FAST_PROGRAM && erase_count==1)
            assert(m.operation==LOADER_PROGRAM_FAST || m.operation==LOADER_PROGRAM_SPARSE);
        else assert(m.operation==LOADER_PROGRAM);
        if (m.operation!=LOADER_PROGRAM_FAST) assert(!(flash_cr & (1u<<18)));
        if (m.operation==LOADER_PROGRAM_SPARSE) sparse_started=true;
        if (m.operation==LOADER_PROGRAM_FAST) {
            assert(!sparse_started);
            if (fast_calls++) assert(flash_cr & (1u<<18));
        }
        assert(m.size <= LOADER_BUFFER_SIZE && !(m.size & 7));
        if (image_crc32(ram + 0x1000, m.size) != m.crc32) m.status = LOADER_BAD_DATA;
        else if (loader_flash_error) { m.status = LOADER_FLASH_ERROR; m.detail = 0x80; }
        else if ((fast_only_error || fast_no_eop) && m.operation == LOADER_PROGRAM_FAST) {
            /* Simulate a partially written row. Recovery must erase it first. */
            memset(flash+m.address-TARGET_FLASH_BASE,0,8);
            m.status = fast_no_eop ? LOADER_NO_EOP : LOADER_FLASH_ERROR;
            m.detail = fast_no_eop ? 0 : 0xa0;
            m.failure_address = m.address+8;
        }
        else {
            assert(blank_checked == TARGET_FLASH_BYTES);
            for (unsigned i=0; i<m.size; i+=8) {
                unsigned row=i & ~255u;
                bool dense=row+256u<=m.size;
                for (unsigned k=row; dense && k<row+256u; k+=8) {
                    bool erased_pair=true;
                    for (unsigned j=0;j<8;++j) if (ram[0x1000+k+j]!=0xff) erased_pair=false;
                    if (erased_pair) dense=false;
                }
                if ((m.operation==LOADER_PROGRAM_FAST && !dense) ||
                    (m.operation==LOADER_PROGRAM_SPARSE && dense)) continue;
                bool all_ff=true;
                for (unsigned j=0;j<8;++j) if (ram[0x1000+i+j] != 0xff) all_ff=false;
                if (!all_ff) {
                    uint32_t at=m.address-TARGET_FLASH_BASE+i;
                    assert(!(at & 7) && at+8 <= TARGET_FLASH_BYTES);
                    for (unsigned j=0;j<8;++j) assert(flash[at+j]==0xff);
                    memcpy(flash+at,ram+0x1000+i,8); ++writes;
                    if (m.operation==LOADER_PROGRAM_FAST) flash_cr=(1u<<18)|(1u<<24);
                }
            }
        }
    }
    m.completed = m.status == LOADER_SUCCESS ? m.size : 0;
    memcpy(ram+0x800,&m,sizeof(m));
    halted = !loader_timeout;
}
bool swd_read32(uint32_t a, uint32_t *v) {
    for (unsigned i=0;i<9;++i) if (a==option_addrs[i]) {
        *v=option_shadow[i];
        if (i==0) {
            ++option_reads;
            if (protected) *v=(*v & ~0xffu)|0xbbu;
            if (option_unstable && option_reads>1) *v^=1u<<16;
            if (option_changed_after_flash && verify_bytes==TARGET_FLASH_BYTES) *v|=1u<<20;
        }
        return true;
    }
    switch (a) {
    case 0xe000edf0u:
        if (option_reloads && reload_dhcsr_reads && transient_halt_errors) {
            assert(!reload_needs_connect); /* Resynchronize before retrying a failed read. */
            --transient_halt_errors; reload_needs_connect=true; return false;
        }
        assert(!reload_needs_connect);
        if ((dhcsr_failure==1 && !option_programs) ||
            (dhcsr_failure==2 && option_programs && !option_reloads) ||
            (dhcsr_failure==3 && option_reloads) ||
            (dhcsr_failure==4 && option_reloads && reload_dhcsr_reads)) return false;
        if (option_reloads) ++reload_dhcsr_reads;
        *v = (halted && !(post_verify && final_halt_lost) && !(lose_halt && (flash_cr & 0x80000000u) && erased) ? 1u << 17 : 0) | (1u<<16) | (reset_seen ? 1u<<25 : 0) | dhcsr_control;
        if (option_reloads && !halted) *v |= 1u<<24; /* Observed running status 01010000. */
        reset_seen=false; break;
    case 0xe000edfcu: *v=demcr; break;
    case 0xe000ed30u: *v=2; break;
    case 0xe0042000u: *v = 0x10076415; break;
    case 0x1fff75e0u: *v = bad_size ? 1024 : 512; break;
    case 0x1fff7590u: *v = option_reloads && reload_wrong_uid ? 0 : 0x11223344u; break;
    case 0x1fff7594u: *v = 0x55667788u; break;
    case 0x1fff7598u: *v = 0x99aabbccu; break;
    case 0x40021000u: *v = cr; break;
    case 0x40021008u: *v = cfgr; break;
    case 0x40021058u: *v = apb1enr1; break;
    case 0x40007000u: *v = (apb1enr1 & 0x10000000u) ? pwr_cr1 : 0; break;
    case 0x40007014u: *v = regulator_stuck || (pwr_cr1 & 0x4000) ? 0x200 : 0; break;
    case 0x40022000u: *v = acr; break;
    case 0x40022010u: *v = (stuck_busy || (option_busy && option_programs) ? 0x10000 : 0) | option_sr; break;
    case 0x40022014u: *v = flash_cr; break;
    default: *v = 0;
    }
    return true;
}
bool swd_write32(uint32_t a, uint32_t v) {
    if (option_reloads && ((reload_control_failure==1 && a==0xe000edfcu) ||
                          (reload_control_failure==2 && a==0xe000edf0u))) return false;
    for (unsigned i=0;i<9;++i) if (a==option_addrs[i]) {
        assert(i==0); /* Never write a protection register. */
        assert(halted && !(flash_cr & 0xc0000000u) && !erased);
        assert((option_shadow[i] ^ v)==(1u<<20) && !(v & (1u<<20)));
        assert((v & 0xffu)==0xaau);
        option_shadow[i]=v; ++option_writes; return true;
    }
    if (a==0x4002200cu) {
        assert(halted && !erased && !(flash_cr & 0x80000000u));
        assert(v==(option_keys==0 ? 0x08192a3bu : 0x4c5d6e7fu));
        if (++option_keys==2 && !option_unlock_fail) flash_cr &= ~0x40000000u;
        return true;
    }
    if (a==0x40022010u) { option_sr &= ~v; return true; }
    if (a==0xe000edfcu) demcr=v;
    if (a == 0xe000edf8u) dcrdr=v;
    if (a == 0xe000edf4u) { assert((v & 0x10000) && (v & 0xffff)<21); core_regs[v&0xffff]=dcrdr; }
    if (a == 0xe000edf0u) {
        if (post_verify && !(v & 2)) ++post_verify_resumes;
        if (v==0xa05f0000u) assert(reset_held);
        dhcsr_control=v & 0xfu;
        halted = (v & 3)==3 && !(option_reloads && reload_no_halt);
        if (v == 0xa05f0009u) emulate_loader();
    }
    if (a == 0x40022000u) acr = v;
    if (a == 0x40021058u) apb1enr1=v;
    if (a == 0x40007000u) { assert(apb1enr1 & 0x10000000); pwr_cr1=v; }
    if (a == 0x40021000u) {
        if (option_clock_stuck && (v & 0xf8u)==0x68u) return true;
        assert((acr & 7)==4 && !(pwr_cr1 & 0x4000));
        if ((cr & 0x15000000u) & ~v) assert((cfgr & 12)==4); /* Switch off PLL first. */
        if ((cr ^ v) & 0xf0u) assert(!(cr & 0x15000000u) && (cfgr & 12)==4);
        cr=(v & ~0x2a000402u) | ((v & 0x15000000u)<<1) | ((v & 1u)<<1) | ((v & 0x100u)<<2);
        if (hsi_stuck) cr &= ~0x400u;
        if (msi_stuck) cr &= ~2u;
    }
    if (a == 0x40021008u) {
        assert((acr & 7)==4);
        unsigned source=v&3;
        assert(source==0 ? (cr & 2)!=0 : source==1 && (cr & 0x400)!=0);
        cfgr=(v & ~12u) | (clock_switch_stuck ? cfgr&12u : source<<2);
    }
    if (a == 0x40022008u) {
        assert(v==((main_keys%2)==0 ? 0x45670123u : 0xcdef89abu));
        if (++main_keys%2==0) flash_cr &= ~0x80000000u;
    }
    if (a == 0x40022014u) {
        if (v & (1u<<17)) {
            /* ES0250 2.2.23: option handling uses MSI range 6 (4 MHz).
             * The previous 24 MHz setup must fail this regression. */
            assert((cr & 0xfbu)==0x6bu && (cfgr & 0xffu)==0);
            assert((v & ~(1u<<24))==(1u<<17) && halted && !erased && !(flash_cr & 0xc0000000u));
            assert(option_keys==2 && option_writes==1 && ++option_programs==1);
            for (unsigned i=0;i<9;++i) assert(option_shadow[i]==(i==0 ? option_initial[i]&~(1u<<20) : option_initial[i]));
            if (option_program_error) option_sr=8u;
            else if (!option_busy) {
                if (!option_not_saved) memcpy(option_nvm,option_shadow,sizeof option_nvm);
                if (option_other_changed) option_nvm[4]^=1u;
                option_sr=option_no_eop || !(v & (1u<<24)) ? 0 : 1u;
            }
            return true;
        }
        if (v & (1u<<27)) {
            assert((cr & 0xfbu)==0x6bu && (cfgr & 0xffu)==0);
            assert(v==(1u<<27) && halted && (demcr&1) && !reset_held && !reset_seen);
            assert(option_programs==1 && ++option_reloads==1 && !erased);
            if (!reload_no_reset) {
                reset_seen=true;
                halted=!reload_no_halt;
                if (reload_debug_lost) { dhcsr_control=0; halted=false; demcr=0; }
                memcpy(option_shadow,option_nvm,sizeof option_shadow);
                flash_cr=0xc0000000u;
            }
            return !reload_lost_ack;
        }
        flash_cr = v | (flash_cr & 0x40000000u); /* OPTLOCK requires keys to clear. */
        if (v & 0x10000u) {
            /* Main flash programming returns to the loader's 24 MHz clock. */
            assert((cr & 0xfbu)==0x9bu && (cfgr & 0xffu)==0);
            assert((v & 0x8004)==0x8004);
            assert(!(option_shadow[0] & (1u<<20)));
            if (option_programs) {
                assert(option_reloads==1 && option_shadow[0]==option_nvm[0]);
                assert((demcr & 1u) && (dhcsr_control & 3u)==3u);
            }
            memset(flash,0xff,sizeof flash); erased=true;
            ++erase_count; blank_checked=0;
            fast_calls=0; sparse_started=false;
            if (incomplete_erase || (retry_erase_failure && erase_count==2)) flash[0x7e800]=0;
        }
    }
    return true;
}
bool swd_write_words(uint32_t a, const uint32_t *v, size_t n) {
    assert(halted && a>=LOADER_CODE_ADDRESS && a+n*4 <= LOADER_CODE_ADDRESS+sizeof(ram));
    if (a == LOADER_BUFFER) {
        uploaded_bytes+=(unsigned)n*4;
        ++uploaded_blocks; if (n*4>largest_upload) largest_upload=(unsigned)n*4;
        if (fail_block) return false;
    }
    memcpy(ram+a-LOADER_CODE_ADDRESS,v,n*4);
    if (corrupt_upload && a==LOADER_BUFFER) ram[0x1000]^=1;
    return true;
}
bool swd_read_words(uint32_t a, uint32_t *v, size_t n) {
    assert(halted);
    if (a>=LOADER_CODE_ADDRESS && a+n*4<=LOADER_CODE_ADDRESS+sizeof(ram)) {
        memcpy(v,ram+a-LOADER_CODE_ADDRESS,n*4); return true;
    }
    assert(erased && a>=TARGET_FLASH_BASE && a+n*4<=TARGET_FLASH_BASE+sizeof(flash));
    assert(!(flash_cr & (1u<<18))); /* Fast session ends before readback. */
    memcpy(v,flash+a-TARGET_FLASH_BASE,n*4);
    if (tamper_address>=a && tamper_address<a+n*4) ((uint8_t*)v)[tamper_address-a]^=1;
    if (retry_verify_failure && erase_count==2 && a==TARGET_FLASH_BASE) ((uint8_t*)v)[0]^=1;
    verify_bytes+=(unsigned)n*4;
    ++verify_calls;
    return true;
}
static void progress(programmer_stage_t s, uint32_t done) {
    (void)done;
    if (s==STAGE_RESTART) post_verify=true;
    if ((unsigned)s<step) {
        assert(s==STAGE_CONNECT && step==STAGE_PROGRAM && progress_restarts==0);
        ++progress_restarts;
    }
    step=s;
}
static void reset_fake(void) {
    ticks=0; flash_cr=0xc0000000u; acr=0x600; cr=0x63;
    const uint32_t options[]={0xffeff8aa,0xffffffff,0xffff0000,0xff00ffff,0xff00ffff,
                             0xffffffff,0xffff0000,0xff00ffff,0xff00ffff};
    memcpy(option_shadow,options,sizeof options);
    memcpy(option_nvm,options,sizeof options);
    memcpy(option_initial,options,sizeof options);
    option_sr=demcr=option_keys=main_keys=option_programs=option_reloads=option_writes=option_reads=0;
    reset_seen=option_unlock_fail=option_program_error=option_busy=option_no_eop=false;
    reload_no_reset=reload_lost_ack=reload_link_fail=reload_no_halt=reload_wrong_uid=false;
    option_not_saved=option_other_changed=option_unstable=option_changed_after_flash=false;
    dhcsr_failure=reload_dhcsr_reads=0; injected_error=SWD_BAD_ACK;
    transient_halt_errors=reload_reconnects=0; reload_needs_connect=false;
    reload_debug_lost=false; dhcsr_control=0;
    reload_control_failure=0;
    cfgr=0; pwr_cr1=0x200; apb1enr1=0;
    hsi_stuck=msi_stuck=regulator_stuck=clock_switch_stuck=option_clock_stuck=false;
    halted=true; reset_held=erased=stuck_busy=protected=bad_size=link_fail=false;
    incomplete_erase=corrupt_upload=loader_timeout=loader_flash_error=lose_halt=fail_block=false;
    fast_only_error=fast_no_eop=retry_erase_failure=retry_verify_failure=false;
    erase_count=progress_restarts=0;
    uploaded_bytes=verify_calls=0;
    final_halt_lost=post_verify=false;
    post_verify_resets=post_verify_resumes=line_releases=0;
    fast_calls=0; sparse_started=false;
    tamper_address=0; writes=verify_bytes=step=uploaded_blocks=blank_checked=largest_upload=0;
    memset(flash,0x12,sizeof flash); memset(ram,0,sizeof ram);
}
static void enable_bfb2(void) {
    option_shadow[0] |= 1u<<20;
    memcpy(option_nvm,option_shadow,sizeof option_nvm);
    memcpy(option_initial,option_shadow,sizeof option_initial);
}
int main(int argc, char **argv) {
    static uint8_t image[TARGET_FLASH_BYTES]; memset(image,0xff,sizeof image);
    uint32_t sp=0x20001000,rv=TARGET_FLASH_BASE+0x101;
    memcpy(image,&sp,4); memcpy(image+4,&rv,4);
    memset(image+0x100,0x5a,256); /* Exercise fast pass before sparse sentinels. */
    const uint32_t sentinels[]={0x3fc,0x400,0x7f8,0x800,0x3fff8,0x40000,0x7e7f8,0x7e800,0x7eff8,0x7fff8};
    for (unsigned i=0;i<sizeof(sentinels)/sizeof(sentinels[0]);++i) image[sentinels[i]]=(uint8_t)i;
    reset_fake(); programmer_report_t r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_OK && !reset_held && verify_bytes==sizeof image);
    assert(blank_checked==sizeof flash && !memcmp(image,flash,sizeof image));
    assert(verify_calls==512);
    assert(r.target_halted==STM32_HALT_AFTER_VERIFY && line_releases==1);
#if STM32_HALT_AFTER_VERIFY
    assert(halted && post_verify_resets==0 && post_verify_resumes==0);
#else
    assert(!halted && post_verify_resets==1 && post_verify_resumes==1);
#endif
    assert(r.fast_rows==(STM32_USE_FAST_PROGRAM ? 1u : 0u));
    assert(r.uid_valid && r.uid[0]==0x11223344u && r.uid[1]==0x55667788u && r.uid[2]==0x99aabbccu);
    assert((cfgr & 0xff)==0 && (cr & 0xfb)==0x9b && (acr & 7)==3);
    assert(!option_keys && !option_programs && !option_reloads && !option_writes);
    assert(!r.bfb2_attempted && !r.bfb2_cleared && r.optr_before==0xffeff8aa);
    /* Preserve parity errors at initial halt; never mislabel them as reset faults. */
    reset_fake(); dhcsr_failure=1; injected_error=SWD_PARITY;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_SWD && r.detail==SWD_PARITY && r.address==0xe000edf0u);
    assert(!erased && reset_held);
    /* Distinguish pre-launch, reconnect and post-launch halt failures. All
     * three can have the same DHCSR address and parity detail on hardware. */
    for (unsigned failure=2; failure<=4; ++failure) {
        reset_fake(); enable_bfb2(); dhcsr_failure=failure; injected_error=SWD_PARITY;
        r=stm32_program(image,sizeof image,progress);
        assert(r.result==RESULT_SWD && r.detail==SWD_PARITY && r.address==0xe000edf0u);
        assert(r.option_phase==(failure==2 ? 4u : failure==3 ? 7u : 8u));
        assert(option_programs==1 && option_reloads==(failure==2 ? 0u : 1u));
        assert(!r.bfb2_cleared && !erased && !writes && reset_held);
        assert(ticks<5000000u); /* Permanent link loss still has a bounded exit. */
    }
    /* Reproduce the captured v6 phase-8 failure after a valid reset status.
     * Transient failures must reconnect without repeating the option write. */
    const swd_error_t reload_errors[]={SWD_BAD_ACK,SWD_PARITY,SWD_FAULT,SWD_PIO_TIMEOUT};
    for (unsigned i=0;i<sizeof reload_errors/sizeof reload_errors[0];++i) {
        reset_fake(); enable_bfb2(); transient_halt_errors=2; injected_error=reload_errors[i];
        r=stm32_program(image,sizeof image,progress);
        assert(r.result==RESULT_OK && r.bfb2_cleared && reload_reconnects==2);
        assert(r.reload_reconnects==2 && (r.reload_dhcsr & (1u<<17)));
        assert(option_programs==1 && option_reloads==1 && option_writes==1);
        assert(!memcmp(image,flash,sizeof image));
    }
    /* Recovering the link must not excuse unchanged/corrupted nonvolatile options. */
    for (unsigned changed=0;changed<2;++changed) {
        reset_fake(); enable_bfb2(); transient_halt_errors=1;
        option_not_saved=!changed; option_other_changed=changed;
        r=stm32_program(image,sizeof image,progress);
        assert(r.result==RESULT_PROTECTED && r.option_phase==9 && r.reload_reconnects==1);
        assert(!r.bfb2_cleared && !erased && !writes && reset_held);
        assert(option_programs==1 && option_reloads==1);
    }
    /* Captured v7 timeout: link is readable but debugging is off and CPU runs.
     * A new halt request is necessary; status polling alone cannot fix this. */
    for (unsigned transient=0;transient<2;++transient) {
        reset_fake(); enable_bfb2(); reload_debug_lost=true; transient_halt_errors=transient;
        r=stm32_program(image,sizeof image,progress);
        assert(r.result==RESULT_OK && r.bfb2_cleared);
        assert((r.reload_dhcsr & 0x20003u)==0x20003u);
        assert(option_programs==1 && option_reloads==1 && !memcmp(image,flash,sizeof image));
    }
    for (unsigned control=1;control<=2;++control) {
        reset_fake(); enable_bfb2(); reload_debug_lost=true; reload_control_failure=control;
        r=stm32_program(image,sizeof image,progress);
        assert(r.result==RESULT_SWD && r.option_phase==8 && !r.bfb2_cleared);
        assert(!erased && !writes && reset_held && ticks<5000000u);
        assert(option_programs==1 && option_reloads==1);
    }
    /* Successful option reload, including the reset interrupting its SWD flush. */
    for (unsigned lost=0;lost<2;++lost) {
        reset_fake(); enable_bfb2(); reload_lost_ack=lost;
        r=stm32_program(image,sizeof image,progress);
        assert(r.result==RESULT_OK && r.optr_before==0xfffff8aa && r.optr_after==0xffeff8aa);
        assert(r.bfb2_attempted==1 && r.bfb2_cleared==1 && r.option_phase==6);
        assert(option_programs==1 && option_reloads==1 && option_writes==1);
        assert(!memcmp(image,flash,sizeof image) && verify_bytes==sizeof image);
        assert(option_nvm[0]==(option_initial[0]&~(1u<<20)));
        for (unsigned i=1;i<9;++i) assert(option_nvm[i]==option_initial[i]);
    }
    /* Non-default options (including single-bank layout) are preserved. */
    reset_fake(); option_shadow[0]=0x039b70aa; enable_bfb2();
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_OK && option_nvm[0]==(0x039b70aa & ~(1u<<20)));
    /* Every failed option attempt must stop BEFORE main erase or loader upload. */
    bool *faults[]={&option_unlock_fail,&option_program_error,&option_busy,&option_no_eop,
                   &reload_no_reset,&reload_link_fail,&reload_no_halt,&reload_wrong_uid,
                   &option_not_saved,&option_other_changed,&option_unstable};
    for (unsigned i=0;i<sizeof faults/sizeof faults[0];++i) {
        reset_fake(); enable_bfb2(); *faults[i]=true;
        r=stm32_program(image,sizeof image,progress);
        assert(r.result!=RESULT_OK && r.stage==STAGE_CONNECT && reset_held);
        assert(!erased && !writes && !uploaded_blocks && !r.bfb2_cleared);
        assert(option_programs<=1 && option_reloads<=1 && ticks<5000000u);
    }
    reset_fake(); enable_bfb2(); option_sr=0x8000u;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_FLASH && !option_keys && !erased);
    reset_fake(); enable_bfb2(); protected=true;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_PROTECTED && !option_keys && !erased);
    /* All WRP/PCROP areas remain protected: never unlock options to bypass them. */
    const unsigned protected_fields[]={1,3,4,5,7,8};
    for (unsigned i=0;i<sizeof protected_fields/sizeof protected_fields[0];++i) {
        reset_fake(); option_shadow[protected_fields[i]]=0; enable_bfb2();
        r=stm32_program(image,sizeof image,progress);
        assert(r.result==RESULT_PROTECTED && !option_keys && !erased);
    }
    reset_fake(); option_changed_after_flash=true;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_PROTECTED && r.stage==STAGE_VERIFY && reset_held);
#if STM32_USE_FAST_PROGRAM
    reset_fake(); enable_bfb2(); fast_only_error=true;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_OK && erase_count==2 && r.standard_retry==1);
    assert(option_programs==1 && option_reloads==1); /* No repeated option writes. */
#endif
    /* Valid non-reset clock configurations must be normalized before erase. */
    const uint32_t initial_clocks[][3]={
        {0x0000009b,0x00,0x200}, /* Already running MSI at 24 MHz */
        {0x03000500,0x8f,0x200}, /* PLL system clock, AHB divided, MSI off */
        {0x00030000,0x0a,0x200}, /* HSE system clock, MSI/HSI off */
        {0x00000500,0x05,0x400}, /* HSI, voltage range 2 */
        {0x0000002b,0x00,0x4400}, /* Low-power run, slow MSI, voltage range 2 */
    };
    for (unsigned i=0;i<sizeof(initial_clocks)/sizeof(initial_clocks[0]);++i) {
        reset_fake(); cr=initial_clocks[i][0]; cfgr=initial_clocks[i][1]; pwr_cr1=initial_clocks[i][2];
        r=stm32_program(image,sizeof image,progress);
        assert(r.result==RESULT_OK && (cfgr & 0xff)==0 && (cr & 0xfb)==0x9b);
        assert(r.initial_rcc_cr==initial_clocks[i][0] && r.initial_rcc_cfgr==initial_clocks[i][1]);
        assert(r.initial_pwr_cr1==initial_clocks[i][2] && !(pwr_cr1 & 0x4000));
        /* BFB2 correction must also reach 4 MHz from each initial clock, then
         * restore 24 MHz before erase. Assertions at OPTSTRT/OBL enforce this. */
        reset_fake(); enable_bfb2();
        cr=initial_clocks[i][0]; cfgr=initial_clocks[i][1]; pwr_cr1=initial_clocks[i][2];
        r=stm32_program(image,sizeof image,progress);
        assert(r.result==RESULT_OK && r.bfb2_cleared && option_reloads==1);
        assert(option_nvm[0]==0xffeff8aau && (cr & 0xfbu)==0x9bu);
    }
    /* A failed clock-range change must stop BEFORE any option key/write/erase. */
    reset_fake(); enable_bfb2(); option_clock_stuck=true;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_TIMEOUT && r.address==0x40021000u && reset_held);
    assert(!option_keys && !option_writes && !option_programs && !option_reloads && !erased);
    assert(option_nvm[0]==0xfffff8aau);
    reset_fake(); hsi_stuck=true;
    r=stm32_program(image,sizeof image,progress); assert(r.result==RESULT_TIMEOUT && !erased);
    reset_fake(); msi_stuck=true;
    r=stm32_program(image,sizeof image,progress); assert(r.result==RESULT_TIMEOUT && !erased);
    reset_fake(); regulator_stuck=true;
    r=stm32_program(image,sizeof image,progress); assert(r.result==RESULT_TIMEOUT && !erased);
    reset_fake(); clock_switch_stuck=true;
    r=stm32_program(image,sizeof image,progress); assert(r.result==RESULT_TIMEOUT && !erased);
    reset_fake(); pwr_cr1=0;
    r=stm32_program(image,sizeof image,progress); assert(r.result==RESULT_CLOCK && !erased);
    reset_fake(); tamper_address=0x0807e800;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_VERIFY && r.address==tamper_address && reset_held);
    const uint32_t mismatch_offsets[]={0,1023,1024,16383,16384,0x7ffff};
    for (unsigned i=0;i<sizeof(mismatch_offsets)/sizeof(mismatch_offsets[0]);++i) {
        reset_fake(); tamper_address=TARGET_FLASH_BASE+mismatch_offsets[i];
        r=stm32_program(image,sizeof image,progress);
        assert(r.result==RESULT_VERIFY && r.address==tamper_address && reset_held);
    }
    reset_fake(); incomplete_erase=true;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_FLASH && r.stage==STAGE_ERASE && r.address==0x0807e800 && writes==0);
    reset_fake(); corrupt_upload=true;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_SWD && writes==0 && reset_held);
    reset_fake(); fail_block=true;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_SWD && uploaded_blocks==1 && writes==0);
    reset_fake(); loader_flash_error=true;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_FLASH && reset_held);
    assert(erase_count==(STM32_USE_FAST_PROGRAM ? 2u : 1u)); /* At most one retry. */
    assert(r.loader_status==LOADER_FLASH_ERROR);
#if STM32_USE_FAST_PROGRAM
    reset_fake(); fast_only_error=true;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_OK && r.standard_retry==1 && erase_count==2 && progress_restarts==1);
    assert(r.fast_rows==0);
    assert(r.fast_error_status==LOADER_FLASH_ERROR && r.fast_error_detail==0xa0);
    assert(r.fast_error_address==TARGET_FLASH_BASE+8 && !memcmp(image,flash,sizeof image));
    assert(verify_bytes==sizeof image && !reset_held);
    reset_fake(); fast_no_eop=true;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_OK && r.standard_retry==1 && erase_count==2);
    assert(r.fast_error_status==LOADER_NO_EOP && r.fast_error_detail==0);
    reset_fake(); fast_only_error=true; retry_erase_failure=true;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_FLASH && r.stage==STAGE_ERASE && reset_held && writes==0);
    assert(r.fast_error_status==LOADER_FLASH_ERROR && erase_count==2);
    reset_fake(); fast_only_error=true; retry_verify_failure=true;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_VERIFY && r.standard_retry==1 && reset_held && erase_count==2);
#endif
    reset_fake(); loader_timeout=true;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_TIMEOUT && reset_held);
    assert(r.standard_retry==0 && erase_count==1);
    reset_fake(); lose_halt=true;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_RESET && verify_bytes==0 && reset_held);
#if STM32_HALT_AFTER_VERIFY
    reset_fake(); final_halt_lost=true;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_RESET && r.target_halted==0 && reset_held);
    assert(verify_bytes==sizeof image && post_verify_resumes==0);
#endif
    reset_fake(); protected=true;
    r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_PROTECTED && !erased);
    reset_fake(); bad_size=true;
    r=stm32_program(image,sizeof image,progress); assert(r.result==RESULT_TARGET && !erased);
    reset_fake(); link_fail=true;
    r=stm32_program(image,sizeof image,progress); assert(r.result==RESULT_SWD && !erased);
    reset_fake(); stuck_busy=true;
    r=stm32_program(image,sizeof image,progress); assert(r.result==RESULT_TIMEOUT && !erased);
    reset_fake(); r=stm32_program(image,sizeof image-1,progress); assert(r.result==RESULT_IMAGE && !erased);
    memset(image+0x7e800,0xff,2048);
    reset_fake(); r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_OK && !memcmp(image,flash,sizeof image) && verify_bytes==sizeof image);
    /* Fully populated image uses 32 transfers, not 256. Includes bank and last
     * flash row; the compiled loader tests check actual FSTPG store sequences.
     */
    memset(image,0x5a,sizeof image); memcpy(image,&sp,4); memcpy(image+4,&rv,4);
    reset_fake(); r=stm32_program(image,sizeof image,progress);
    assert(r.result==RESULT_OK && uploaded_blocks==32 && largest_upload==16384);
    assert(r.fast_rows==(STM32_USE_FAST_PROGRAM ? 2048u : 0u));
    assert(verify_bytes==sizeof image && !memcmp(image,flash,sizeof image));
    /* A partial final transfer must neither read beyond the image nor write
     * padding. The rest of main flash remains erased by the mass erase.
     */
    size_t partial=RAM_BLOCK_BYTES+264u;
    reset_fake(); r=stm32_program(image,partial,progress);
    assert(r.result==RESULT_OK && uploaded_blocks==(STM32_USE_FAST_PROGRAM ? 3u : 2u) && verify_bytes==partial);
    assert(!memcmp(image,flash,partial));
    assert(uploaded_bytes==partial+(STM32_USE_FAST_PROGRAM ? 8u : 0u));
    for (size_t i=partial;i<sizeof flash;++i) assert(flash[i]==0xff);
    if (argc==2) {
        FILE *f=fopen(argv[1],"rb"); assert(f);
        assert(fread(image,1,sizeof image,f)==sizeof image && fgetc(f)==EOF); fclose(f);
        reset_fake(); r=stm32_program(image,sizeof image,progress);
        assert(r.result==RESULT_OK && !memcmp(image,flash,sizeof image));
        assert(verify_bytes==sizeof image && verify_calls==512 && r.standard_retry==0);
        assert(r.target_halted==STM32_HALT_AFTER_VERIFY && line_releases==1);
        assert(post_verify_resets==(STM32_HALT_AFTER_VERIFY ? 0u : 1u));
        assert(post_verify_resumes==(STM32_HALT_AFTER_VERIFY ? 0u : 1u));
        assert(r.fast_rows==(STM32_USE_FAST_PROGRAM ? 1650u : 0u));
        printf("Production host path: %u uploads, %u uploaded bytes, %u verify reads, %u fast rows\n",
               uploaded_blocks,uploaded_bytes,verify_calls,(unsigned)r.fast_rows);
    }
    puts("Clock normalization, 16 KiB transfers, verified standard recovery, bounded retry and failure tests passed");
}
