#include "hardware/powman.h"
#include <assert.h>
static uint64_t hardware_ms;
static bool running,xosc_selected;
bool test_powman_fail_start,test_powman_bad_readback;
unsigned test_powman_sets;
void powman_timer_stop(void){running=false;}
void powman_timer_start(void){assert(xosc_selected);running=!test_powman_fail_start;}
bool powman_timer_is_running(void){return running;}
void powman_timer_set_1khz_tick_source_xosc(void){assert(!running);xosc_selected=true;}
void powman_timer_set_ms(uint64_t ms){assert(!running);hardware_ms=ms;test_powman_sets++;}
uint64_t powman_timer_get_ms(void){return test_powman_bad_readback?0:hardware_ms;}
void test_powman_advance_ms(uint64_t ms){if(running)hardware_ms+=ms;}
