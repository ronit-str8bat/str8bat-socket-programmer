#include <assert.h>
#include <stdio.h>
#include "ui_logic.h"
#include "crc32.h"
#include "programmer_config.h"

static uint32_t reference_crc(const uint8_t *data, size_t size) {
    uint32_t c=0xffffffffu;
    for (size_t i=0;i<size;++i) {
        c^=data[i];
        for (unsigned b=0;b<8;++b) c=(c>>1)^(0xedb88320u & (0u-(c&1u)));
    }
    return ~c;
}

int main(void) {
    assert(SWD_OE_PIN==4 && STATUS_LED_PIN==5 && START_PIN==18);
    /* Exercise every priority combination and each blink boundary. */
    for(unsigned full=0;full<2;full++)
    for(unsigned clock=0;clock<2;clock++)
    for(unsigned wifi=0;wifi<2;wifi++) {
        const uint32_t times[]={0,249,250,499,500,749,750,999,1000,UINT32_MAX};
        for(unsigned i=0;i<sizeof(times)/sizeof(times[0]);i++) {
            uint32_t t=times[i];
            bool expected=full?t%500u<250u:!clock?t%1000u<500u:wifi!=0;
            assert(status_led_on(full,clock,wifi,t)==expected);
        }
    }
    assert(image_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926u);
    assert(image_crc32((const uint8_t *)"", 0) == 0);
    uint8_t data[1027]; uint32_t random=1234567;
    for (size_t i=0;i<sizeof(data);++i) {
        random=random*1664525u+1013904223u; data[i]=(uint8_t)(random>>24);
    }
    for (size_t n=0;n<sizeof(data)-1;++n)
        assert(image_crc32(data+1,n)==reference_crc(data+1,n));
    start_button_t b;
    start_button_reset(&b, true, 0);
    assert(!start_button_update(&b, true, 500)); /* Held at power-on. */
    assert(!start_button_update(&b, false, 501));
    assert(!start_button_update(&b, false, 600));
    assert(!start_button_update(&b, false, 601));
    assert(!start_button_update(&b, true, 700));
    assert(!start_button_update(&b, true, 799));
    assert(start_button_update(&b, true, 800));
    assert(!start_button_update(&b, true, 1500)); /* No held-button repeat. */
    start_button_reset(&b, false, 0xfffffff0u);
    assert(!start_button_update(&b, false, 0x60u));
    assert(!start_button_update(&b, true, 0x70u));
    assert(!start_button_update(&b, false, 0x80u)); /* Chatter cancels press. */
    assert(!start_button_update(&b, true, 0x90u));
    assert(!start_button_update(&b, true, 0xf3u));
    assert(start_button_update(&b, true, 0xf4u));
    puts("LED 2 priority/timing, GP4 OE / GP5 status mapping, CRC and button debounce tests passed");
}
