/* Exercise the actual HTTP portal with a fake TCP transport. Storage's real
 * implementation is independently tested by test_network_storage.c. */
#include <assert.h>
#include "hardware/powman.h"
#include "../src/services/local_portal.c"

struct test_cyw43 cyw43_state;
static struct tcp_pcb test_listener,test_client;
static uint8_t csv[1024u*1024u],wire[1024u*1024u+2000u];
static size_t wire_size;
static bool locked,fail_write,fail_close;
static unsigned reads,ends,clears;
static uint64_t now;
static int clear_result;
static const char test_etag[]="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static u16_t capacity=256;
uint64_t time_us_64(void){return now;}
uint32_t get_rand_32(void){static uint32_t n=10;return ++n;}
void watchdog_reboot(uint32_t a,uint32_t b,uint32_t c){(void)a;(void)b;(void)c;assert(false);}
void dhcp_server_init(dhcp_server_t *d,struct netif *n,ip_addr_t *ip,ip_addr_t *mask){(void)n;(void)ip;(void)mask;d->udp=(void *)1;}
void dns_server_init(dns_server_t *d,struct netif *n,ip_addr_t *ip){(void)n;(void)ip;d->udp=(void *)1;}
struct tcp_pcb *tcp_new_ip_type(int type){(void)type;return &test_listener;}
err_t tcp_bind(struct tcp_pcb *p,const ip_addr_t *ip,u16_t port){(void)p;assert(ip->addr==0xc0a80401 && port==80);return ERR_OK;}
struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *p,int backlog){assert(backlog==1);return p;}
void tcp_accept(struct tcp_pcb *p,err_t (*fn)(void *,struct tcp_pcb *,err_t)){(void)p;(void)fn;}
void tcp_arg(struct tcp_pcb *p,void *arg){(void)p;(void)arg;}
void tcp_recv(struct tcp_pcb *p,recv_fn fn){p->recv=fn;}
void tcp_err(struct tcp_pcb *p,error_fn fn){p->error=fn;}
void tcp_abort(struct tcp_pcb *p){(void)p;}
err_t tcp_close(struct tcp_pcb *p){(void)p;if(fail_close){fail_close=false;return ERR_MEM;}return ERR_OK;}
void tcp_recved(struct tcp_pcb *p,u16_t n){(void)p;(void)n;}
u16_t tcp_sndbuf(struct tcp_pcb *p){(void)p;return capacity;}
err_t tcp_write(struct tcp_pcb *p,const void *data,u16_t n,int flags){
    assert(flags==TCP_WRITE_FLAG_COPY && n<=capacity);
    if(fail_write){fail_write=false;return ERR_MEM;}
    assert(wire_size+n<sizeof(wire));memcpy(wire+wire_size,data,n);wire_size+=n;wire[wire_size]=0;p->unacked=(void *)1;return ERR_OK;
}
void tcp_output(struct tcp_pcb *p){(void)p;}
u16_t pbuf_copy_partial(const struct pbuf *p,void *data,u16_t n,u16_t offset){assert(n+offset<=p->tot_len);memcpy(data,(const uint8_t *)p->payload+offset,n);return n;}
void pbuf_free(struct pbuf *p){(void)p;}
bool local_snapshot_begin(unsigned file,uint32_t *size,char etag[65]){assert(!locked);if(file)return false;locked=true;*size=sizeof(csv);strcpy(etag,test_etag);return true;}
int local_snapshot_read(uint32_t off,void *data,size_t n){assert(locked && off+n<=sizeof(csv) && n<=1024);memcpy(data,csv+off,n);reads++;return (int)n;}
void local_snapshot_end(void){assert(locked);locked=false;ends++;}
int local_logs_clear(const char *etag){assert(!locked && !strcmp(etag,test_etag));if(!clear_result)clears++;return clear_result;}
const char *storage_device_id(void){return "0011223344556677";}
const char *services_status(void){return "AP_READY";}
static void send_request(const char *text){
    memset(&test_client,0,sizeof(test_client));wire_size=0;wire[0]=0;
    assert(accept_cb(NULL,&test_client,ERR_OK)==ERR_OK);
    /* Fragmented TCP request, including split CRLF and token. */
    for(size_t off=0;text[off];off++){
        struct pbuf p={1,text+off};assert(receive_cb(NULL,&test_client,&p,ERR_OK)==ERR_OK);
        portal_poll(false);
    }
}
static void finish(bool busy){
    for(unsigned i=0;client && i<10000;i++){
        test_client.unacked=NULL;now+=1000;portal_poll(busy);
    }
    assert(!client);
}
static void deletion(char req[512],const char *etag){
    snprintf(req,512,"DELETE /logs HTTP/1.1\r\nHost: 192.168.4.1\r\nX-Programmer-Token: %s\r\nIf-Match: \"%s\"\r\n\r\n",token,etag);
}
int main(void){
    memset(csv,'x',sizeof(csv));csv[sizeof(csv)-1]='\n';assert(portal_start());
    send_request("GET / HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n");finish(false);
    assert(strstr((char *)wire,"Download CSV") && strstr((char *)wire,"Clear downloaded logs"));
    assert(strstr((char *)wire,"Clock set to IST (UTC+05:30)") && strstr((char *)wire,"/logs/utc-archive") && strstr((char *)wire,"/logs/legacy-current-2"));
    assert(strstr((char *)wire,"req('/sync-time'") && strstr((char *)wire,"if(!j.clock_set)await sync()"));
    assert(strstr((char *)wire,"</script>")); /* Bounded page buffer must not truncate JS. */
    assert(strstr((char *)wire,"method:'HEAD'") && strstr((char *)wire,"window.location.assign('/logs?etag='"));
    assert(!strstr((char *)wire,"createObjectURL") && !strstr((char *)wire,"r.blob()"));
    /* OS captive probes must redirect to the actual page, never return 204 or
     * expose the token under a foreign origin. API Host checks stay intact. */
    const char *probes[]={
        "GET /generate_204 HTTP/1.1\r\nHost: connectivitycheck.gstatic.com\r\n\r\n",
        "GET /hotspot-detect.html HTTP/1.0\r\nHost: captive.apple.com\r\n\r\n",
        "GET /connecttest.txt HTTP/1.1\r\nHost: www.msftconnecttest.com\r\n\r\n",
        "GET /gen_204?test=1 HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n"};
    for(unsigned i=0;i<sizeof(probes)/sizeof(probes[0]);i++) {
        send_request(probes[i]);finish(false);
        assert(strstr((char *)wire,"302 Found") && strstr((char *)wire,"Location: http://192.168.4.1/\r\n"));
        assert(!strstr((char *)wire,token) && !locked);
    }
    send_request("GET /hotspot-detect.html HTTP/1.1\r\nHost: captive.apple.com\r\nHost: other\r\n\r\n");finish(false);
    assert(strstr((char *)wire,"400 Bad Request"));
    send_request("GET /status HTTP/1.1\r\nHost: captive.apple.com\r\n\r\n");finish(false);
    assert(strstr((char *)wire,"403 Forbidden") && !strstr((char *)wire,token));
    unsigned reads_before=reads;
    send_request("HEAD /logs HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n");finish(false);
    char *head_end=strstr((char *)wire,"\r\n\r\n");assert(head_end);
    assert((size_t)(head_end+4-(char *)wire)==wire_size && reads==reads_before && !receipt_etag[0] && !locked);
    assert(strstr((char *)wire,"Content-Length: 1048576\r\n") && strstr((char *)wire,"filename=\"programming_logs.csv\""));
    char req[512];deletion(req,test_etag);send_request(req);finish(false);
    assert(strstr((char *)wire,"409 Conflict") && !clears);
    char get[256];snprintf(get,sizeof(get),"GET /logs?etag=%s HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n",test_etag);
    snprintf(req,sizeof(req),"GET /logs?etag=%064u HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n",0u);
    send_request(req);finish(false);assert(strstr((char *)wire,"412 Precondition Failed") && !locked && !receipt_etag[0]);
    send_request("GET /logs?etag=bad HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n");finish(false);
    assert(strstr((char *)wire,"400 Bad Request"));
    send_request(get);assert(locked);
    assert(receive_cb(NULL,&test_client,NULL,ERR_OK)==ERR_ABRT && !client && !locked && !receipt_etag[0]);
    send_request(get);assert(locked);portal_cancel_download();assert(!locked && !receipt_etag[0]);
    deletion(req,test_etag);send_request(req);finish(false);assert(strstr((char *)wire,"409 Conflict"));
    send_request(get);assert(locked);capacity=0;portal_poll(false);unsigned before=reads;portal_poll(false);assert(before==reads);
    capacity=251;fail_write=true;fail_close=true;finish(false);
    char *body=strstr((char *)wire,"\r\n\r\n");assert(body);body+=4;
    assert(strstr((char *)wire,"Content-Length: 1048576\r\n") && strstr((char *)wire,"ETag: \"012345"));
    assert(wire_size-(size_t)(body-(char *)wire)==sizeof(csv) && !memcmp(body,csv,sizeof(csv)));
    assert(!locked && !strcmp(receipt_etag,test_etag));
    clear_result=1;deletion(req,test_etag);send_request(req);finish(false);assert(strstr((char *)wire,"412 Precondition Failed") && !clears);
    clear_result=0;send_request(req);finish(false);assert(strstr((char *)wire,"204 No Content") && clears==1 && !receipt_etag[0]);
    send_request(req);finish(false);assert(strstr((char *)wire,"409 Conflict") && clears==1);
    send_request("DELETE /logs HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n");finish(false);assert(strstr((char *)wire,"403 Forbidden"));
    send_request("GET /logs HTTP/1.1\r\nHost: attacker.example\r\n\r\n");finish(false);assert(strstr((char *)wire,"403 Forbidden"));
    send_request("GET /logs HTTP/1.1\r\nHost: 192.168.4.1\r\nHost: attacker.example\r\n\r\n");finish(false);assert(strstr((char *)wire,"403 Forbidden"));
    send_request("GET /status HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n");finish(false);assert(strstr((char *)wire,"\"clock_set\":false"));
    const char *json="{\"unix_seconds\":1790640000}";
    snprintf(req,sizeof(req),"POST /sync-time HTTP/1.1\r\nHost: 192.168.4.1\r\nX-Programmer-Token: %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\n\r\n%s",token,(unsigned)strlen(json),json);
    test_powman_fail_start=true;send_request(req);finish(false);
    assert(strstr((char *)wire,"503 Service Unavailable") && !local_time_valid());test_powman_fail_start=false;
    send_request(req);finish(false);assert(strstr((char *)wire,"204 No Content") && local_time_valid());
    send_request("GET /status HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n");finish(false);
    assert(strstr((char *)wire,"\"ist\":\"2026-09-29 05:30:") && strstr((char *)wire,"\"timezone\":\"IST\""));
    assert(strstr((char *)wire,"RP2350_POWMAN"));
    test_powman_advance_ms(3600000); /* Advances with no HTTP request/poll. */
    send_request("GET /status HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n");finish(false);
    assert(strstr((char *)wire,"\"ist\":\"2026-09-29 06:30:00\""));
    unsigned sets=test_powman_sets;
    send_request("POST /sync-time HTTP/1.1\r\nHost: 192.168.4.1\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}");finish(false);
    assert(strstr((char *)wire,"403 Forbidden") && test_powman_sets==sets);
    /* Busy synchronization is rejected without changing the running clock. */
    memset(&test_client,0,sizeof(test_client));wire_size=0;assert(accept_cb(NULL,&test_client,ERR_OK)==ERR_OK);
    struct pbuf sync_request={(u16_t)strlen(req),req};receive_cb(NULL,&test_client,&sync_request,ERR_OK);finish(true);
    assert(strstr((char *)wire,"409 Conflict") && test_powman_sets==sets);
    snprintf(req,sizeof(req),"POST /time HTTP/1.1\r\nHost: 192.168.4.1\r\nX-Programmer-Token: %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\n\r\n%s",token,(unsigned)strlen(json),json);
    send_request(req);finish(false);assert(strstr((char *)wire,"204 No Content")); /* Legacy alias. */
    send_request(get);assert(locked);error_cb(NULL,ERR_ABRT);assert(!locked && !client);
    send_request(get);assert(locked);now+=120000001;portal_poll(false);assert(!locked && !client);
    memset(&test_client,0,sizeof(test_client));wire_size=0;assert(accept_cb(NULL,&test_client,ERR_OK)==ERR_OK);
    struct pbuf p={(u16_t)strlen(get),get};receive_cb(NULL,&test_client,&p,ERR_OK);finish(true);
    assert(strstr((char *)wire,"409 Conflict") && !locked);
    /* Download service may ACK the last bytes and close before the next poll.
     * That is a completed download, unlike the early handoff above. */
    receipt_etag[0]=0;send_request(get);
    for(unsigned i=0;i<10000 && (download_offset<download_size || response_sent<response_size);i++) {
        test_client.unacked=NULL;portal_poll(false);
    }
    assert(client && downloading && download_offset==download_size && response_sent==response_size);
    test_client.unacked=NULL;
    assert(receive_cb(NULL,&test_client,NULL,ERR_OK)==ERR_ABRT);
    assert(!locked && !client && !strcmp(receipt_etag,test_etag));
    puts("Local portal: exact 1 MiB stream, backpressure, completed-download/ETag deletion, stale protection, UTC, Host/token, busy/cancel/timeout passed");return 0;
}
