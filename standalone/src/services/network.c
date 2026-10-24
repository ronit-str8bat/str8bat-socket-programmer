#include "services/network.h"
#include "services/http_client.h"
#include "services/portal.h"
#include "services/usb_export.h"
#include "wifi_config.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico/rand.h"
#include "lwip/apps/sntp.h"
#include "mbedtls/platform_time.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static enum { NET_JOINING,NET_AP,NET_ONLINE } net_state;
static enum { REMOTE_IDLE,REMOTE_MANIFEST,REMOTE_IMAGE,REMOTE_LOG } remote;
static uint64_t join_deadline,next_ota,next_log,ntp_set_at;
static uint64_t epoch;
static bool clock_valid,storage_ok;
static ota_manifest_t manifest;
static char response[1536],batch_id[65];
static size_t response_size;
static const char *status="STARTING";
unsigned long services_ntp_nonce(void) { return get_rand_32(); }
static mbedtls_time_t tls_time(mbedtls_time_t *out) {
    mbedtls_time_t t=clock_valid?(mbedtls_time_t)(epoch+(time_us_64()-ntp_set_at)/1000000):0;
    if(out)*out=t;
    return t;
}
void services_set_ntp_time(unsigned long seconds) {
    /* NTP is not an authenticated time source. TLS also validates chain/name;
     * do not use this appliance where hostile NTP is in the threat model. */
    if(seconds<1735689600ul)return; /* reject clearly invalid/pre-2025 clocks */
    epoch=seconds;ntp_set_at=time_us_64();clock_valid=true;
}
static void start_ap(void) {
    cyw43_arch_disable_sta_mode();
    if(strlen(WIFI_SETUP_PASSWORD)<8 || strlen(WIFI_SETUP_PASSWORD)>63){status="AP_PASSWORD_INVALID";net_state=NET_AP;return;}
    char name[33];
    if(strlen(WIFI_SETUP_SSID)>27){status="AP_NAME_TOO_LONG";net_state=NET_AP;return;}
    const char *id=storage_device_id();
    snprintf(name,sizeof(name),"%s_%s",WIFI_SETUP_SSID,id+strlen(id)-4);
    cyw43_arch_enable_ap_mode(name,WIFI_SETUP_PASSWORD,CYW43_AUTH_WPA2_AES_PSK);
    net_state=NET_AP;status=portal_start()?"SETUP_AP_192.168.4.1":"AP_START_FAILED";
}
bool services_init(void) {
    storage_ok=storage_init();if(!storage_ok){status="STORAGE_ERROR";return false;}
    mbedtls_platform_set_time(tls_time);
    wifi_credentials_t credentials={0};
    if(!storage_credentials_read(&credentials)) {
        if(strlen(WIFI_FACTORY_SSID)>32 || strlen(WIFI_FACTORY_PASSWORD)>63){status="FACTORY_CREDENTIALS_INVALID";return false;}
        strcpy(credentials.ssid,WIFI_FACTORY_SSID);strcpy(credentials.password,WIFI_FACTORY_PASSWORD);
    }
    cyw43_arch_enable_sta_mode();net_state=NET_JOINING;status="CONNECTING";
    join_deadline=time_us_64()+(uint64_t)WIFI_CONNECT_TIMEOUT_MS*1000;
    if(credentials.ssid[0])cyw43_arch_wifi_connect_async(credentials.ssid,credentials.password,CYW43_AUTH_WPA2_AES_PSK);
    memset(&credentials,0,sizeof(credentials));return true;
}
static bool collect(const void *p,size_t n) {
    if(n>=sizeof(response)-response_size || memchr(p,0,n))return false;
    memcpy(response+response_size,p,n);response_size+=n;response[response_size]=0;return true;
}
static void remote_stop(void) { http_cancel();storage_ota_abort();remote=REMOTE_IDLE; }
static bool get_manifest(void) {
    https_url_t url;if(!parse_https_url(OTA_MANIFEST_URL,&url))return false;
    response_size=0;response[0]=0;
    if(!http_start(&url,NULL,0,NULL,sizeof(response)-1,collect))return false;
    remote=REMOTE_MANIFEST;status="OTA_CHECKING";return true;
}
static bool send_logs(void) {
    https_url_t url;uint32_t size;
    if(!parse_https_url(LOG_UPLOAD_URL,&url) || !storage_batch_prepare(&size,batch_id))return false;
    response_size=0;response[0]=0;
    if(!http_start(&url,batch_id,size,storage_batch_read,64,collect))return false;
    remote=REMOTE_LOG;status="LOG_UPLOADING";return true;
}
void services_poll(bool busy) {
    busy=busy || usb_export_active();
    /* CYW43 callbacks never touch flash, the SWD worker, or the active image. */
    cyw43_arch_poll();
    if(net_state==NET_AP){portal_poll(busy);return;}
    uint64_t now=time_us_64();
    bool linked=cyw43_tcpip_link_status(&cyw43_state,CYW43_ITF_STA)==CYW43_LINK_UP;
    if(net_state==NET_JOINING) {
        if(linked) {
            net_state=NET_ONLINE;status="ONLINE";
            sntp_setoperatingmode(SNTP_OPMODE_POLL);sntp_setservername(0,NTP_SERVER);sntp_init();
        } else if(now>=join_deadline && !busy)start_ap();
        return;
    }
    if(!linked) {
        if(!busy)remote_stop();
        sntp_stop();net_state=NET_JOINING;join_deadline=now+(uint64_t)WIFI_CONNECT_TIMEOUT_MS*1000;
        status="RECONNECTING";return;
    }
    if(busy || !storage_ok)return;
    if(remote!=REMOTE_IDLE) {
        http_poll();int result=http_result();if(!result)return;
        if(result<0){remote_stop();status="REMOTE_FAILED_RETRY_PENDING";return;}
        if(remote==REMOTE_MANIFEST) {
            https_url_t origin;
            if(!parse_manifest(response,&manifest) || !parse_https_url(OTA_MANIFEST_URL,&origin) || strcmp(origin.host,manifest.image.host)) {
                remote_stop();status="MANIFEST_REJECTED";return;
            }
            if(manifest.revision<=storage_image().revision){remote_stop();status="IMAGE_CURRENT";return;}
            if(!storage_ota_begin(manifest.revision,manifest.size,manifest.sha) ||
               !http_start(&manifest.image,NULL,0,NULL,manifest.size,storage_ota_write)) {
                remote_stop();status="OTA_START_FAILED";return;
            }
            remote=REMOTE_IMAGE;status="OTA_DOWNLOADING";
        } else if(remote==REMOTE_IMAGE) {
            bool ok=http_body_size()==manifest.size && storage_ota_commit();
            remote_stop();status=ok?"OTA_IMAGE_READY":"OTA_REJECTED";
        } else {
            /* Server MUST durably accept/deduplicate this batch and return
             * HTTP 200 with the exact 64-character Idempotency-Key as body. */
            bool ok=response_size==64 && !memcmp(response,batch_id,64) && storage_batch_ack(batch_id);
            remote_stop();status=ok?"LOG_SYNCED":"LOG_ACK_MISSING_RETAINED";
        }
        return;
    }
    if(!clock_valid || !SERVER_CA_PEM[0])return;
    if(now>=next_log){next_log=now+(uint64_t)LOG_RETRY_INTERVAL_MS*1000;if(send_logs())return;}
    if(now>=next_ota){next_ota=now+(uint64_t)OTA_CHECK_INTERVAL_MS*1000;(void)get_manifest();}
}
bool services_job_begin(void) {
    if(!storage_ok || portal_reboot_pending())return false;
    usb_export_cancel();
    portal_cancel_download();
    remote_stop(); /* START takes priority over a download/upload. */
    uint64_t timestamp=clock_valid?(uint64_t)tls_time(NULL):time_us_64()/1000;
    bool ok=storage_job_begin(timestamp,clock_valid,storage_image().size);
    if(!ok)status="LOG_STORAGE_BLOCKS_START";
    return ok;
}
bool services_job_finish(const programmer_report_t *report) {
    bool ok=storage_job_finish(report);
    if(!ok){status="LOG_SAVE_FAILED";storage_ok=false;}
    else next_log=0;
    return ok;
}
const char *services_status(void) { return status; }
bool services_export_begin(uint32_t *size) {
    if(!storage_ok || portal_reboot_pending())return false;
    remote_stop();
    return storage_export_begin(size);
}
