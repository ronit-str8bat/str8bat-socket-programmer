#include "services/factory_client.h"
#include "services/storage.h"
#include "wifi_config.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "lwip/tcp.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

enum client_op { CLIENT_IDLE, CLIENT_JOB, CLIENT_BIN, CLIENT_REPORT };
static struct tcp_pcb *pcb;
static enum client_op op;
static char pico_id[17],master_ssid[33],request[256],header[640],body[256];
static size_t request_len,header_len,body_len;
static uint32_t expected_size,expected_crc,expected_generation,expected_received,bin_received;
static uint64_t next_action;
static bool scan_started,master_found,master_conflict,master_connected,receiving,program_ready;
static bool download_pending;
static bool header_done,request_sent,failed;
static int pending_report=-1;

static uint32_t local_image_crc(void) {
    return factory_bin_available()?factory_bin_image().crc:storage_image().crc;
}

static void reset_connection(void) {
    if(pcb){tcp_arg(pcb,NULL);tcp_recv(pcb,NULL);tcp_err(pcb,NULL);tcp_abort(pcb);pcb=NULL;}
    op=CLIENT_IDLE;header_len=body_len=0;header_done=request_sent=failed=false;
}
static void retry_later(void) {
    /* A broken BIN connection must never leave an incomplete image marked as
     * being uploaded. The next poll restarts from a freshly erased slot. */
    if(op==CLIENT_BIN){factory_bin_abort();receiving=false;}
    reset_connection();
    next_action=time_us_64()+2000000u;
}
static int scan_cb(void *arg,const cyw43_ev_scan_result_t *result) {
    (void)arg;if(!result)return 0;
    char name[33];size_t n=result->ssid_len;if(n>32)n=32;memcpy(name,result->ssid,n);name[n]=0;
    if(strncmp(name,"Str8bat_Master_",15))return 0;
    if(!master_found){memcpy(master_ssid,name,n+1);master_found=true;}
    else if(strcmp(master_ssid,name))master_conflict=true;
    return 0;
}
static void start_scan(void) {
    master_found=master_conflict=false;scan_started=true;
    cyw43_wifi_scan_options_t options={0};
    if(cyw43_wifi_scan(&cyw43_state,&options,NULL,scan_cb)!=0){scan_started=false;next_action=time_us_64()+3000000u;}
}
static bool header_number(const char *name,uint32_t *out) {
    const char *p=strstr(header,name);if(!p)return false;p+=strlen(name);
    unsigned long value=0;char *end=NULL;value=strtoul(p,&end,10);if(end==p || (*end!='\r'&&*end!='\n'))return false;
    *out=(uint32_t)value;return true;
}
static bool header_hex(const char *name,uint32_t *out) {
    const char *p=strstr(header,name);if(!p)return false;p+=strlen(name);
    char *end=NULL;unsigned long value=strtoul(p,&end,16);if(end==p || (*end!='\r'&&*end!='\n'))return false;
    *out=(uint32_t)value;return true;
}
static bool parse_header(void) {
    int status=0;if(sscanf(header,"HTTP/1.1 %d",&status)!=1)return false;
    uint32_t length=0;if(!header_number("Content-Length: ",&length))return false;
    if(op==CLIENT_JOB)return (status==200 && length<sizeof(body)) || (status==204 && !length);
    if(op==CLIENT_BIN) {
        uint32_t crc=0,generation=0;
        if(status!=200 || length<8u || length>FACTORY_BIN_MAX_SIZE || (length&7u) || !header_hex("X-Bin-CRC32: ",&crc) || !header_number("X-Bin-Generation: ",&generation))return false;
        return crc==expected_crc && generation==expected_generation;
    }
    return status==204 && !length;
}
static void finish_response(void) {
    if(failed){retry_later();return;}
    if(op==CLIENT_JOB) {
        body[body_len]=0;
        unsigned long generation=0,size=0,crc=0,received=0;
        if(!body_len){retry_later();return;}
        if(sscanf(body,"{\"generation\":%lu,\"size\":%lu,\"crc32\":\"%8lx\",\"received\":%lu}",&generation,&size,&crc,&received)!=4 ||
           size<8u || size>FACTORY_BIN_MAX_SIZE || (size&7u) || (uint32_t)crc==local_image_crc()) {retry_later();return;}
        expected_generation=(uint32_t)generation;expected_size=(uint32_t)size;expected_crc=(uint32_t)crc;expected_received=(uint32_t)received;
        if(!factory_bin_begin(expected_size,expected_crc,expected_received)){retry_later();return;}
        reset_connection();download_pending=true;next_action=time_us_64();return;
    }
    if(op==CLIENT_BIN) {
        if(bin_received==expected_size && factory_bin_commit()){program_ready=true;receiving=false;}
        else factory_bin_abort();
    }
    if(op==CLIENT_REPORT)pending_report=-1;
    retry_later();
}
static err_t receive_cb(void *arg,struct tcp_pcb *conn,struct pbuf *p,err_t err) {
    (void)arg;(void)err;
    if(!p){finish_response();return ERR_OK;}
    uint8_t temp[1024];u16_t offset=0;
    while(offset<p->tot_len) {
        u16_t n=(u16_t)(p->tot_len-offset);if(n>sizeof(temp))n=sizeof(temp);
        pbuf_copy_partial(p,temp,n,offset);offset+=(u16_t)n;
        for(u16_t i=0;i<n;i++) {
            uint8_t byte=temp[i];
            if(!header_done) {
                if(header_len+1>=sizeof(header)){failed=true;break;}
                header[header_len++]=(char)byte;header[header_len]=0;
                if(header_len>=4 && !memcmp(header+header_len-4,"\r\n\r\n",4)) {
                    header_done=true;if(!parse_header()){failed=true;break;}
                }
            } else if(op==CLIENT_BIN) {
                if(bin_received>=expected_size || !factory_bin_write(bin_received,&byte,1)){failed=true;break;}
                bin_received++;
            } else {
                if(body_len+1>=sizeof(body)){failed=true;break;}body[body_len++]=(char)byte;
            }
        }
        if(failed)break;
    }
    tcp_recved(conn,p->tot_len);pbuf_free(p);return ERR_OK;
}
static void error_cb(void *arg,err_t err) { (void)arg;(void)err;pcb=NULL;retry_later(); }
static err_t connected_cb(void *arg,struct tcp_pcb *conn,err_t err) {
    (void)arg;if(err!=ERR_OK){retry_later();return err;}
    tcp_arg(conn,NULL);tcp_recv(conn,receive_cb);tcp_err(conn,error_cb);
    request_sent=false;return ERR_OK;
}
static bool start_request(enum client_op next,const char *text) {
    if(pcb || !master_connected)return false;
    size_t n=strlen(text);if(n>=sizeof(request))return false;memcpy(request,text,n+1);request_len=n;
    op=next;header_len=body_len=0;header_done=request_sent=failed=false;
    pcb=tcp_new_ip_type(IPADDR_TYPE_V4);if(!pcb){retry_later();return false;}
    ip_addr_t ip;IP4_ADDR(&ip,192,168,4,1);
    if(tcp_connect(pcb,&ip,80,connected_cb)!=ERR_OK){retry_later();return false;}
    return true;
}
static void start_job(void) {
    char text[256];snprintf(text,sizeof(text),"GET /factory/job?id=%s&crc=%08lx HTTP/1.1\r\nHost: 192.168.4.1\r\nX-Factory-Key: %s\r\nConnection: close\r\n\r\n",pico_id,(unsigned long)local_image_crc(),FACTORY_GROUP_KEY);
    (void)start_request(CLIENT_JOB,text);
}
static void start_bin(void) {
    char text[256];snprintf(text,sizeof(text),"GET /factory/bin?id=%s HTTP/1.1\r\nHost: 192.168.4.1\r\nX-Factory-Key: %s\r\nConnection: close\r\n\r\n",pico_id,FACTORY_GROUP_KEY);
    bin_received=0;receiving=true;
    if(!start_request(CLIENT_BIN,text)){receiving=false;factory_bin_abort();}
}
static void start_report(unsigned result) {
    char text[256];snprintf(text,sizeof(text),"GET /factory/report?id=%s&generation=%lu&result=%u HTTP/1.1\r\nHost: 192.168.4.1\r\nX-Factory-Key: %s\r\nConnection: close\r\n\r\n",pico_id,(unsigned long)factory_bin_generation(),result,FACTORY_GROUP_KEY);
    (void)start_request(CLIENT_REPORT,text);
}
void factory_client_init(const char *id) {
    if(!id || strlen(id)!=16)return;
    memcpy(pico_id,id,17);
    cyw43_arch_enable_sta_mode();
    start_scan();
}
void factory_client_poll(bool programmer_busy) {
    uint64_t now=time_us_64();
    if(scan_started && !cyw43_wifi_scan_active(&cyw43_state)) {
        scan_started=false;
        if(master_found && !master_conflict && cyw43_arch_wifi_connect_async(master_ssid,WIFI_LOCAL_PASSWORD,CYW43_AUTH_WPA2_AES_PSK)==PICO_OK) next_action=now+1000000u;
        else {master_found=false;next_action=now+5000000u;}
    }
    int link=cyw43_tcpip_link_status(&cyw43_state,CYW43_ITF_STA);
    master_connected=link==CYW43_LINK_UP;
    if(!master_connected) {
        if(!scan_started && now>=next_action)start_scan();
        return;
    }
    if(pcb) {
        if(!request_sent) {
            u16_t n=(u16_t)(request_len>tcp_sndbuf(pcb)?tcp_sndbuf(pcb):request_len);
            if(n && tcp_write(pcb,request,n,TCP_WRITE_FLAG_COPY)==ERR_OK){request_sent=true;tcp_output(pcb);}
        }
        return;
    }
    if(programmer_busy || program_ready || now<next_action)return;
    if(pending_report>=0){start_report((unsigned)pending_report);return;}
    if(op==CLIENT_IDLE && download_pending){download_pending=false;start_bin();}
    else if(op==CLIENT_IDLE)start_job();
}
bool factory_client_master_connected(void) { return master_connected; }
bool factory_client_receiving(void) { return receiving || download_pending; }
bool factory_client_take_program_job(void) { if(!program_ready)return false;program_ready=false;return true; }
void factory_client_program_result(unsigned result) {
    pending_report=(int)result;
    if(master_connected && !pcb)start_report(result);
}
