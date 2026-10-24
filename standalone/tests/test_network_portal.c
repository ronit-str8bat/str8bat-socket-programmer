/* Exercise the actual HTTP portal with a fake TCP transport. Storage's real
 * implementation is independently tested by test_network_storage.c. */
#include <assert.h>
#include "../src/services/portal.c"

struct test_cyw43 cyw43_state;
static struct tcp_pcb test_listener,test_client;
static uint8_t csv[150000],wire[152000];
static size_t wire_size;
static bool locked,fail_write,fail_close;
static unsigned reads,ends,saves;
static uint64_t now;
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
bool storage_export_begin(uint32_t *size){assert(!locked);locked=true;*size=sizeof(csv);return true;}
int storage_export_read(uint32_t offset,void *data,size_t n){assert(locked && offset+n<=sizeof(csv));memcpy(data,csv+offset,n);reads++;return (int)n;}
void storage_export_end(void){assert(locked);locked=false;ends++;}
bool storage_credentials_save(const wifi_credentials_t *c){(void)c;saves++;return true;}
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
static void request_text(char out[256],const char *host){
    snprintf(out,256,"GET /logs.csv?token=%s HTTP/1.1\r\nHost: %s\r\n\r\n",csrf,host);
}
int main(void){
    memset(csv,'x',sizeof(csv));memcpy(csv,"job_id,pico_id\n",15);csv[sizeof(csv)-1]='\n';
    assert(portal_start());
    send_request("GET / HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n");finish(false);
    assert(strstr((char *)wire,"Download programming logs") && strstr((char *)wire,"/logs.csv?token="));
    char req[256];request_text(req,"192.168.4.1");send_request(req);assert(locked);
    capacity=0;unsigned before=reads;portal_poll(false);unsigned after=reads;portal_poll(false);assert(reads==after && after<=before+1);
    capacity=251;fail_write=true;fail_close=true;finish(false);
    char *body=strstr((char *)wire,"\r\n\r\n");assert(body);body+=4;
    assert(strstr((char *)wire,"Content-Disposition: attachment; filename=\"programming_logs.csv\""));
    assert(strstr((char *)wire,"Content-Length: 150000\r\n"));
    assert(wire_size-(size_t)(body-(char *)wire)==sizeof(csv) && !memcmp(body,csv,sizeof(csv)));
    assert(!locked && ends==1 && saves==0);
    request_text(req,"attacker.example");send_request(req);finish(false);assert(strstr((char *)wire,"403 Forbidden") && !locked);
    send_request("GET /logs.csv?token=bad HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n");finish(false);assert(strstr((char *)wire,"403 Forbidden"));
    request_text(req,"192.168.4.1");send_request(req);assert(locked);portal_cancel_download();assert(!client && !locked);
    send_request(req);assert(locked);error_cb(NULL,ERR_ABRT);assert(!client && !locked);
    send_request(req);assert(locked);now+=120000001;portal_poll(false);assert(!client && !locked);
    /* Simulate a complete request received while target programming is busy. */
    memset(&test_client,0,sizeof(test_client));wire_size=0;assert(accept_cb(NULL,&test_client,ERR_OK)==ERR_OK);
    struct pbuf p={(u16_t)strlen(req),req};receive_cb(NULL,&test_client,&p,ERR_OK);finish(true);
    assert(strstr((char *)wire,"409 Conflict") && !locked && !saves);
    puts("Portal: download link, fragmented requests, exact 150 KB stream, TCP backpressure, host/token checks, busy refusal, cancellation/disconnect/timeout passed");return 0;
}
