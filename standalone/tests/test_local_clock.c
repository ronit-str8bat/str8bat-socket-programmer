#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/services/local_clock.c"
static uint64_t ticks;
uint64_t time_us_64(void){return ticks;}
int main(void){
    assert(!local_time_valid() && !local_time_now());
    char date[20];uint32_t epoch;
    assert(local_parse_time(" { \"unix_seconds\": 1790640000 } ",&epoch));
    assert(local_time_set(epoch));assert(local_format_utc(local_time_now(),date));
    assert(!strcmp(date,"2026-09-29 00:00:00"));
    assert(local_format_ist(epoch,date) && !strcmp(date,"2026-09-29 05:30:00"));
    assert(local_format_ist(epoch+19u*3600u,date) && !strcmp(date,"2026-09-30 00:30:00"));
    assert(local_format_ist(1835395200u-3600u,date) && !strcmp(date,"2028-02-29 04:30:00"));
    assert(local_format_ist(4102444799u,date) && !strcmp(date,"2100-01-01 05:29:59"));
    assert(!local_format_ist(0,date));
    ticks=123000000;assert(local_time_now()==epoch); /* CPU uptime is not the RTC. */
    test_powman_advance_ms(123000);assert(local_format_utc(local_time_now(),date));assert(!strcmp(date,"2026-09-29 00:02:03"));
    test_powman_advance_ms(999);assert(local_time_now()==epoch+123);
    test_powman_advance_ms(1);assert(local_time_now()==epoch+124);
    powman_timer_stop();test_powman_advance_ms(9999);assert(!local_time_valid() && !local_time_now());
    assert(local_time_set(epoch)); /* Explicit resynchronization restarts hardware. */
    unsigned sets=test_powman_sets;assert(!local_time_set(0) && test_powman_sets==sets && local_time_now()==epoch);
    test_powman_fail_start=true;assert(!local_time_set(epoch) && !local_time_valid());test_powman_fail_start=false;
    test_powman_bad_readback=true;assert(!local_time_set(epoch) && !local_time_valid());test_powman_bad_readback=false;
    assert(local_time_set(2147483648u) && local_time_now()==2147483648u); /* Beyond signed 2038. */
    assert(local_format_utc(1835395200,date) && !strcmp(date,"2028-02-29 00:00:00"));
    assert(local_format_utc(2147483648u,date) && !strcmp(date,"2038-01-19 03:14:08"));
    assert(local_time_set(4102444799u));assert(local_format_utc(local_time_now(),date) && !strcmp(date,"2099-12-31 23:59:59"));
    test_powman_advance_ms(1000);assert(!local_time_valid());
    const char *bad[]={"", "{", "{}", "{\"unix_seconds\":0}", "{\"unix_seconds\":4102444800}",
        "{\"unix_seconds\":42949672960}","{\"unix_seconds\":-1790640000}","{\"unix_seconds\":1790640000.1}",
        "{\"unix_seconds\":1790640000,\"unix_seconds\":1790640001}","{\"unix_seconds\":1790640000}garbage"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(!local_parse_time(bad[i],&epoch));
    puts("Hardware clock: independent advancement, XOSC, stopped/start/readback failures, resync, IST rollovers and 2038 bounds passed");return 0;
}
