#include "services/http_client.h"
#include "wifi_config.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/dns.h"
#include "mbedtls/ssl.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

static struct altcp_pcb *pcb;
static struct altcp_tls_config *tls;
static https_url_t endpoint;
static char request[1536],header[2048];
static uint8_t rx[32768],tx[2048];
static uint32_t rx_size,header_size,body_received,body_length,response_limit;
static uint32_t request_size,request_sent,upload_size,upload_sent;
static http_sink_fn sink;
static http_source_fn source;
static int result=-1;
static bool connected,headers_done,eof;
static uint64_t deadline;
static uintptr_t generation;

static void close_connection(void) {
    if(pcb){altcp_arg(pcb,NULL);altcp_recv(pcb,NULL);altcp_err(pcb,NULL);altcp_abort(pcb);pcb=NULL;}
}
void http_cancel(void) { ++generation;close_connection();result=-1; }
static void failure(void) { close_connection();result=-1; }
static void error_cb(void *arg,err_t err) { (void)arg;(void)err;pcb=NULL;result=-1; }
static err_t receive_cb(void *arg,struct altcp_pcb *c,struct pbuf *p,err_t err) {
    (void)arg;
    if(!p){eof=true;return ERR_OK;}
    if(err!=ERR_OK){pbuf_free(p);result=-1;return ERR_OK;}
    if(p->tot_len>sizeof(rx)-rx_size)return ERR_MEM;
    pbuf_copy_partial(p,rx+rx_size,p->tot_len,0);rx_size+=p->tot_len;
    altcp_recved(c,p->tot_len);pbuf_free(p);return ERR_OK;
}
static err_t connected_cb(void *arg,struct altcp_pcb *c,err_t err) {
    (void)arg;(void)c;if(err!=ERR_OK){result=-1;return err;}connected=true;return ERR_OK;
}
static void dns_cb(const char *name,const ip_addr_t *ip,void *arg) {
    (void)name;if((uintptr_t)arg!=generation || result!=0)return;
    if(!ip){failure();return;}
    pcb=altcp_tls_new(tls,IPADDR_TYPE_V4);
    if(!pcb){failure();return;}
    mbedtls_ssl_context *ssl=altcp_tls_context(pcb);
    if(!ssl || mbedtls_ssl_set_hostname(ssl,endpoint.host)!=0){failure();return;}
    altcp_arg(pcb,NULL);altcp_recv(pcb,receive_cb);altcp_err(pcb,error_cb);
    if(altcp_connect(pcb,ip,443,connected_cb)!=ERR_OK)failure();
}
static bool safe_token(const char *p) {
    for(;*p;p++)if((unsigned char)*p<=32 || (unsigned char)*p>=127)return false;
    return true;
}
bool http_start(const https_url_t *url,const char *batch_id,uint32_t n,http_source_fn src,uint32_t limit,http_sink_fn snk) {
    http_cancel();
    if(!SERVER_CA_PEM[0] || !safe_token(SERVER_BEARER_TOKEN))return false;
    if(!tls)tls=altcp_tls_create_config_client((const uint8_t *)SERVER_CA_PEM,sizeof(SERVER_CA_PEM));
    if(!tls)return false;
    endpoint=*url;source=src;sink=snk;response_limit=limit;upload_size=n;
    rx_size=header_size=body_received=body_length=request_sent=upload_sent=0;
    connected=headers_done=eof=false;
    int len=snprintf(request,sizeof(request),"%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nAccept-Encoding: identity\r\n%s%s%s",
        src?"POST":"GET",url->path,url->host,SERVER_BEARER_TOKEN[0]?"Authorization: Bearer ":"",
        SERVER_BEARER_TOKEN,SERVER_BEARER_TOKEN[0]?"\r\n":"");
    if(len<0 || len>=(int)sizeof(request))return false;
    int tail;
    if(src)tail=snprintf(request+len,sizeof(request)-(size_t)len,
        "Content-Type: text/csv\r\nIdempotency-Key: %s\r\nContent-Length: %lu\r\n\r\n",batch_id,(unsigned long)n);
    else tail=snprintf(request+len,sizeof(request)-(size_t)len,"\r\n");
    if(tail<0 || tail>=(int)(sizeof(request)-(size_t)len))return false;
    request_size=(uint32_t)(len+tail);result=0;deadline=time_us_64()+180000000u;
    ip_addr_t ip;
    err_t err=dns_gethostbyname(url->host,&ip,dns_cb,(void *)generation);
    if(err==ERR_OK)dns_cb(url->host,&ip,(void *)generation);
    else if(err!=ERR_INPROGRESS)failure();
    return result==0;
}
void http_poll(void) {
    if(result!=0){close_connection();return;}
    if(time_us_64()>deadline){failure();return;}
    if(connected && pcb) {
        if(request_sent<request_size) {
            uint32_t n=request_size-request_sent;if(n>altcp_sndbuf(pcb))n=altcp_sndbuf(pcb);
            if(n && altcp_write(pcb,request+request_sent,(u16_t)n,TCP_WRITE_FLAG_COPY)==ERR_OK)request_sent+=n;
        } else if(upload_sent<upload_size) {
            uint32_t n=upload_size-upload_sent;if(n>sizeof(tx))n=sizeof(tx);if(n>altcp_sndbuf(pcb))n=altcp_sndbuf(pcb);
            if(n) {
                int got=source(upload_sent,tx,n);if(got!=(int)n){failure();return;}
                if(altcp_write(pcb,tx,(u16_t)n,TCP_WRITE_FLAG_COPY)==ERR_OK)upload_sent+=n;
            }
        }
        altcp_output(pcb);
    }
    uint32_t used=0;
    while(!headers_done && used<rx_size) {
        if(header_size+1>=sizeof(header)){failure();return;}
        header[header_size++]=(char)rx[used++];
        if(!header[header_size-1]){failure();return;}
        if(header_size>=4 && !memcmp(header+header_size-4,"\r\n\r\n",4)) {
            header[header_size]=0;
            if(!parse_http_response(header,response_limit,&body_length)){failure();return;}headers_done=true;
        }
    }
    if(headers_done && used<rx_size) {
        uint32_t n=rx_size-used;
        if(n>body_length-body_received || !sink(rx+used,n)){failure();return;}
        body_received+=n;used+=n;
    }
    if(used){memmove(rx,rx+used,rx_size-used);rx_size-=used;}
    if(headers_done && body_received==body_length) {
        if(request_sent!=request_size || upload_sent!=upload_size){failure();return;}
        result=1;close_connection();
    } else if(eof)failure();
}
int http_result(void) { return result; }
uint32_t http_body_size(void) { return body_received; }
