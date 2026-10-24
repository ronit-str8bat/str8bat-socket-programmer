/* Exercise AP identity and live association state, including failed queries;
 * a stale DHCP lease must never imply that a phone is still connected. */
#include <assert.h>
#include "hardware/powman.h"
#include "../src/services/local_network.c"
struct test_cyw43 cyw43_state;
static uint64_t now;
static unsigned queries;
static int associated;
static bool early_driver_error,job_called,job_utc;
static uint64_t job_date;
uint64_t time_us_64(void){return now;}
bool storage_init(void){return true;}
void cyw43_arch_enable_ap_mode(const char *ssid,const char *password,unsigned auth){
    assert(!strcmp(ssid,"Str8bat_Pico_0011223344556677") && !strcmp(password,"str8bat@pico") && auth==CYW43_AUTH_WPA2_AES_PSK);
}
void cyw43_arch_poll(void){}
void cyw43_wifi_ap_get_stas(struct test_cyw43 *self,int *count,uint8_t *mac){
    assert(self==&cyw43_state && *count==1);queries++;
    if(early_driver_error)return;
    *count=associated?1:0;if(*count)memcpy(mac,"ABCDEF",6);
}
bool portal_start(void){return true;}
void portal_poll(bool busy){(void)busy;}
void portal_cancel_download(void){}
stored_image_t storage_image(void){return (stored_image_t){.size=524288};}
const char *storage_device_id(void){return "0011223344556677";}
bool storage_job_begin(uint64_t timestamp,bool utc,uint32_t size){
    assert(size==524288);job_called=true;job_date=timestamp;job_utc=utc;return true;
}
bool storage_job_finish(const programmer_report_t *report){return report->result==RESULT_OK;}
int main(void){
    assert(services_init());services_poll(false);assert(!local_wifi_connected() && queries==1);
    associated=1;now=499999;services_poll(false);assert(queries==1);
    now=500000;services_poll(false);assert(local_wifi_connected() && queries==2);
    now=1000000;associated=0;services_poll(false);assert(!local_wifi_connected());
    early_driver_error=true;now=1500000;services_poll(false);assert(!local_wifi_connected());
    early_driver_error=false;associated=1;now=2000000;services_poll(false);assert(local_wifi_connected());
    unsigned before=queries;now=2500000;services_poll(true);assert(queries==before);
    assert(services_job_begin() && job_called && !job_utc && job_date==0);
    assert(local_time_set(1790640000));now+=2000000;test_powman_advance_ms(2000);
    assert(services_job_begin() && job_utc && job_date==1790640002);
    programmer_report_t report={.result=RESULT_OK};assert(services_job_finish(&report));
    assert(!strcmp(services_status(),"AP_READY"));
    unsigned sets=test_powman_sets;associated=0;now+=500000;services_poll(false);
    assert(!local_wifi_connected());
    /* No network traffic or software clock polls for six hours. */
    test_powman_advance_ms(6ull*3600*1000);
    assert(services_job_begin() && job_utc && job_date==1790661602u);
    assert(services_job_finish(&report));
    associated=1;now+=500000;services_poll(false);
    assert(local_wifi_connected() && test_powman_sets==sets && local_time_now()==1790661602u);
    puts("Local AP: identity, association, six hours offline hardware time, reconnect without reset and job timestamps passed");return 0;
}
