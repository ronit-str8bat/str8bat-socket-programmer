/* Always-on WPA2 AP. No station connection, DNS client, NTP, Internet OTA or
 * cloud upload is started in this build. The phone seeds the hardware clock
 * with Unix seconds over /sync-time; Wi-Fi loss never clears the clock. */
#include "services/network.h"
#include "services/local_api.h"
#include "services/portal.h"
#include "wifi_config.h"
#include "programmer_config.h"
#if FACTORY_ROLE == FACTORY_ROLE_SLAVE
#include "services/factory_client.h"
#endif
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>

static bool storage_ok;
static bool client_connected;
static uint64_t next_client_check;
static unsigned start_error=10;
static const char *status="STARTING";
#if FACTORY_ROLE == FACTORY_ROLE_MASTER
static bool factory_id_valid(const char *id) {
    if(!id || strlen(id)!=16)return false;
    for(unsigned i=0;i<16;i++)if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='A'&&id[i]<='F')||(id[i]>='a'&&id[i]<='f')))return false;
    return true;
}
#endif
bool services_init(void) {
    storage_ok=storage_init();if(!storage_ok){status=storage_init_status();return false;}
    /* Use the Pico's complete unique ID, so every board carrying the same
     * UF2 has a different visible Wi-Fi name. SSIDs are limited to 32 bytes. */
    const char *id=storage_device_id();
    char name[33];local_device_ssid(name);
    if(strlen(WIFI_LOCAL_PASSWORD)<8 || strlen(WIFI_LOCAL_PASSWORD)>63 || strlen(name)>32 || strlen(id)!=16 ||
       (FACTORY_ROLE!=FACTORY_ROLE_NONE && !strcmp(FACTORY_GROUP_KEY,"CHANGE-THIS-FACTORY-KEY"))) {
        status="AP_CONFIGURATION_INVALID";return false;
    }
#if FACTORY_ROLE == FACTORY_ROLE_MASTER
    /* Do not accidentally use a Master UF2 on an unapproved Pico. */
    if(!factory_id_valid(FACTORY_MASTER_ID) || strcmp(id,FACTORY_MASTER_ID)) {
        status="MASTER_ID_MISMATCH";return false;
    }
#endif
    cyw43_arch_enable_ap_mode(name,WIFI_LOCAL_PASSWORD,CYW43_AUTH_WPA2_AES_PSK);
    if(!portal_start()){status="AP_START_FAILED";return false;}
#if FACTORY_ROLE == FACTORY_ROLE_SLAVE
    factory_client_init(id);
#endif
    status="AP_READY_TIME_UNSET";return true;
}
void services_poll(bool busy) {
    cyw43_arch_poll();portal_poll(busy);
#if FACTORY_ROLE == FACTORY_ROLE_SLAVE
    factory_client_poll(busy);
    /* A received Factory BIN is stored only. It is deliberately not started
       until the operator connects the STM32 and presses the GP18 button. */
    if(!busy)(void)factory_client_take_program_job();
#endif
    /* Query only while idle: no extra radio-control transactions during SWD.
     * One MAC suffices to detect a client; this SDK bounds writes by count.
     * Clearing the buffer also handles an early driver failure gracefully. */
    uint64_t now=time_us_64();
    if(!busy && now>=next_client_check) {
        uint8_t mac[6]={0};int count=1;
        cyw43_wifi_ap_get_stas(&cyw43_state,&count,mac);
        bool nonzero=false;for(unsigned i=0;i<sizeof(mac);i++)nonzero|=mac[i]!=0;
        client_connected=count>0 && nonzero;
        next_client_check=now+500000u;
    }
}
bool local_wifi_connected(void) {
#if FACTORY_ROLE == FACTORY_ROLE_SLAVE
    return client_connected || factory_client_master_connected();
#else
    return client_connected;
#endif
}
bool services_job_begin(void) {
    start_error=10;
    if(!storage_ok)return false;
#if LOCAL_REQUIRE_TIME
    if(!local_time_valid()){status="SET_TIME_BEFORE_START";start_error=11;return false;}
#endif
    portal_cancel_download();
    stored_image_t image=services_program_image();
    bool ok=storage_job_begin(local_time_now(),local_time_valid(),image.size);
    status=ok?"PROGRAMMING":"LOG_STORAGE_BLOCKS_START";return ok;
}
bool services_job_finish(const programmer_report_t *report) {
    bool ok=storage_job_finish(report);
    if(!ok){storage_ok=false;status="LOG_SAVE_FAILED";}
    else status="AP_READY";
    return ok;
}
const char *services_status(void) {
    if(!strcmp(status,"AP_READY") || !strcmp(status,"AP_READY_TIME_UNSET"))
#if FACTORY_ROLE == FACTORY_ROLE_SLAVE
        return factory_client_master_connected() ? (local_time_valid()?"AP_READY_MASTER_CONNECTED":"AP_READY_TIME_UNSET_MASTER_CONNECTED") :
                                                   (local_time_valid()?"AP_READY_MASTER_SEARCHING":"AP_READY_TIME_UNSET_MASTER_SEARCHING");
#else
        return local_time_valid()?"AP_READY":"AP_READY_TIME_UNSET";
#endif
    return status;
}
unsigned local_start_error(void) { return start_error; }
bool services_factory_led_active(void) {
#if FACTORY_ROLE == FACTORY_ROLE_MASTER
    return portal_factory_transfer_active();
#elif FACTORY_ROLE == FACTORY_ROLE_SLAVE
    return factory_client_receiving();
#else
    return false;
#endif
}
bool services_factory_take_program_job(void) {
    /* Factory transfer never automatically starts SWD programming. */
    return false;
}
stored_image_t services_program_image(void) {
#if FACTORY_ROLE != FACTORY_ROLE_NONE
    /* The GP18 button uses the most recently stored factory BIN. */
    if(local_factory_bin_available())return local_factory_bin_image();
#endif
    return storage_image();
}
