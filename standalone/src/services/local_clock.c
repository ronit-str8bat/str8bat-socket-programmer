#include "services/local_api.h"
#include "hardware/powman.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* Pico 2 W / RP2350 uses the POWMAN always-on timer instead of the RP2040
 * calendar RTC. Use its crystal-derived 1 kHz hardware counter directly,
 * avoiding time_t width/local-time conversions and the 2038 boundary.
 * Only core 0 accesses this clock. Wi-Fi and CPU uptime never advance it in
 * software. A fresh application boot requires an explicit phone sync. */
static bool valid;
bool local_time_set(uint32_t seconds) {
    if(seconds<1735689600u || seconds>=4102444800u)return false; /* 2025..2099 UTC */
    valid=false;
    powman_timer_stop();
    powman_timer_set_1khz_tick_source_xosc();
    powman_timer_set_ms((uint64_t)seconds*1000u);
    powman_timer_start();
    uint64_t readback=powman_timer_get_ms()/1000u;
    valid=powman_timer_is_running() && readback>=seconds && readback<=(uint64_t)seconds+1u && readback<4102444800ull;
    return valid;
}
uint32_t local_time_now(void) {
    if(!valid || !powman_timer_is_running())return 0;
    uint64_t seconds=powman_timer_get_ms()/1000u;
    return seconds>=1735689600ull && seconds<4102444800ull?(uint32_t)seconds:0;
}
bool local_time_valid(void) { return local_time_now()!=0; }
static bool leap(unsigned year) { return year%4==0 && (year%100!=0 || year%400==0); }
static bool format_calendar(uint32_t seconds,char out[20]) {
    uint32_t days=seconds/86400u,day_time=seconds%86400u;unsigned year=1970,month=0;
    while(days>=(leap(year)?366u:365u)){days-=leap(year)?366u:365u;year++;}
    static const unsigned lengths[]={31,28,31,30,31,30,31,31,30,31,30,31};
    while(month<11){unsigned n=lengths[month]+(month==1 && leap(year));if(days<n)break;days-=n;month++;}
    return snprintf(out,20,"%04u-%02u-%02lu %02lu:%02lu:%02lu",year,month+1,(unsigned long)days+1,
        (unsigned long)(day_time/3600),(unsigned long)(day_time/60%60),(unsigned long)(day_time%60))==19;
}
bool local_format_utc(uint32_t seconds,char out[20]) {
    if(seconds<1735689600u || seconds>=4102444800u)return false;
    return format_calendar(seconds,out);
}
bool local_format_ist(uint32_t seconds,char out[20]) {
    /* Phone sends standard Unix seconds. Offset only the human-readable
     * display, never the stored clock or elapsed flashing duration. */
    if(seconds<1735689600u || seconds>=4102444800u)return false;
    return format_calendar(seconds+19800u,out); /* India: UTC+05:30, no DST. */
}
static void space(const char **p) { while(**p && strchr(" \r\n\t",**p))++*p; }
bool local_parse_time(const char *p,uint32_t *seconds) {
    space(&p);if(*p++!='{')return false;space(&p);
    const char key[]="\"unix_seconds\"";
    if(strncmp(p,key,sizeof(key)-1))return false;
    p+=sizeof(key)-1;space(&p);if(*p++!=':')return false;space(&p);
    if(!isdigit((unsigned char)*p) || *p=='0')return false;
    uint32_t n=0;
    do {unsigned d=(unsigned)(*p++-'0');if(n>(UINT32_MAX-d)/10u)return false;n=n*10u+d;}while(isdigit((unsigned char)*p));
    space(&p);if(*p++!='}')return false;space(&p);
    if(*p || n<1735689600u || n>=4102444800u)return false;
    *seconds=n;return true;
}
