#pragma once
#include <stdint.h>
#include <stdbool.h>
void powman_timer_stop(void);
void powman_timer_start(void);
bool powman_timer_is_running(void);
void powman_timer_set_1khz_tick_source_xosc(void);
void powman_timer_set_ms(uint64_t ms);
uint64_t powman_timer_get_ms(void);
/* Hardware model advances independently of mocked CPU uptime and Wi-Fi. */
void test_powman_advance_ms(uint64_t ms);
extern bool test_powman_fail_start,test_powman_bad_readback;
extern unsigned test_powman_sets;
