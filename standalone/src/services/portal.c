/* A single, bounded HTTP session on the password-protected provisioning AP.
 * TCP callbacks only collect requests. Flash writes/reboot happen in poll(). */
#include "services/portal.h"
#include "services/protocol.h"
#include "services/storage.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "lwip/tcp.h"
#include "dhcpserver.h"
#include "dnsserver.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

static struct tcp_pcb *listener,*client;
static dhcp_server_t dhcp;
static dns_server_t dns;
static char request[1536],response[1800],csrf[33];
static size_t received,response_size,response_sent;
static uint64_t expires,reboot_at;
static bool ready,responding;
static bool downloading;
static uint32_t download_size,download_offset;
static void end_download(void) {
    if(downloading)storage_export_end();
    downloading=false;
}
static void close_client(void) {
    if(client){tcp_arg(client,NULL);tcp_recv(client,NULL);tcp_err(client,NULL);tcp_abort(client);client=NULL;}
    received=response_size=response_sent=0;ready=responding=false;
    memset(request,0,sizeof(request));
    end_download();
}
static void error_cb(void *arg,err_t err) { (void)arg;(void)err;client=NULL;received=0;ready=responding=false;end_download(); }
static err_t receive_cb(void *arg,struct tcp_pcb *pcb,struct pbuf *p,err_t err) {
    (void)arg;(void)err;
    if(!p)return ERR_OK;
    if(responding || p->tot_len>=sizeof(request)-received){tcp_recved(pcb,p->tot_len);pbuf_free(p);ready=true;received=sizeof(request)-1;request[received]=0;return ERR_OK;}
    pbuf_copy_partial(p,request+received,p->tot_len,0);received+=p->tot_len;request[received]=0;
    tcp_recved(pcb,p->tot_len);pbuf_free(p);ready=true;return ERR_OK;
}
static err_t accept_cb(void *arg,struct tcp_pcb *pcb,err_t err) {
    (void)arg;
    if(err!=ERR_OK || client){tcp_abort(pcb);return ERR_ABRT;}
    client=pcb;received=response_size=response_sent=0;ready=responding=false;
    expires=time_us_64()+10000000;tcp_recv(pcb,receive_cb);tcp_err(pcb,error_cb);return ERR_OK;
}
static void respond(const char *status,const char *body) {
    int n=snprintf(response,sizeof(response),"HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Security-Policy: default-src 'none'; form-action 'self'\r\n\r\n%s",status,(unsigned)strlen(body),body);
    if(n<0 || n>=(int)sizeof(response)){close_client();return;}
    response_size=(size_t)n;response_sent=0;responding=true;
}
/* Only the AP's literal address may serve sensitive downloads. This also
 * prevents a DNS-rebinding origin from reading logs through captive DNS. */
static bool local_host(const char *headers) {
    bool seen=false;
    const char *p=strstr(headers,"\r\n");
    while(p && p[2] && p[2]!='\r') {
        p+=2;const char *end=strstr(p,"\r\n");if(!end)return false;
        if((size_t)(end-p)>=5 && !strncasecmp(p,"Host:",5)) {
            if(seen)return false;
            seen=true;const char *v=p+5;while(v<end && (*v==' ' || *v=='\t'))++v;
            size_t n=(size_t)(end-v);
            if(!((n==11&&!memcmp(v,"192.168.4.1",11)) ||
                 (n==14&&!memcmp(v,"192.168.4.1:80",14))))return false;
        }
        p=end;
    }
    return seen;
}
static void start_download(bool busy) {
    char expected[96];
    snprintf(expected,sizeof(expected),"GET /logs.csv?token=%s HTTP/1.1\r\n",csrf);
    if(strncmp(request,expected,strlen(expected)) || !local_host(request)) {
        respond("403 Forbidden","Open http://192.168.4.1 and use Download programming logs.");return;
    }
    if(busy || reboot_at) { respond("409 Conflict","Programmer busy. Download again after the job finishes.");return; }
    if(!storage_export_begin(&download_size)) {
        respond("503 Service Unavailable","Logs are unavailable. Try again when the programmer is idle.");return;
    }
    downloading=true;download_offset=0;
    int n=snprintf(response,sizeof(response),
        "HTTP/1.1 200 OK\r\nContent-Type: text/csv; charset=utf-8\r\n"
        "Content-Disposition: attachment; filename=\"programming_logs.csv\"\r\n"
        "Content-Length: %lu\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n\r\n",(unsigned long)download_size);
    if(n<0 || n>=(int)sizeof(response)){close_client();return;}
    response_size=(size_t)n;response_sent=0;responding=true;
    expires=time_us_64()+120000000u;
}
bool portal_start(void) {
    for(unsigned i=0;i<4;i++)snprintf(csrf+i*8,9,"%08lx",(unsigned long)get_rand_32());
    ip_addr_t ip,mask;IP4_ADDR(&ip,192,168,4,1);IP4_ADDR(&mask,255,255,255,0);
    struct netif *nif=&cyw43_state.netif[CYW43_ITF_AP];
    netif_set_addr(nif,ip_2_ip4(&ip),ip_2_ip4(&mask),ip_2_ip4(&ip));
    dhcp_server_init(&dhcp,nif,&ip,&mask);dns_server_init(&dns,nif,&ip);
    if(!dhcp.udp || !dns.udp)return false;
    listener=tcp_new_ip_type(IPADDR_TYPE_V4);if(!listener)return false;
    if(tcp_bind(listener,&ip,80)!=ERR_OK){tcp_abort(listener);listener=NULL;return false;}
    struct tcp_pcb *bound=listener;listener=tcp_listen_with_backlog(bound,1);
    if(!listener){tcp_abort(bound);return false;}tcp_accept(listener,accept_cb);return true;
}
/* Strict Content-Length, no transfer coding, no folded or duplicate headers. */
static bool content_length(char *headers,size_t *length) {
    bool found=false,form=false;*length=0;
    for(char *p=strstr(headers,"\r\n");p && p[2] && p[2]!='\r';) {
        p+=2;char *end=strstr(p,"\r\n");if(!end)return false;
        char *colon=memchr(p,':',(size_t)(end-p));if(!colon)return false;
        size_t key=(size_t)(colon-p);const char *v=colon+1;while(v<end && (*v==' ' || *v=='\t'))v++;
        if(key==14&&!strncasecmp(p,"Content-Length",14)) {
            if(found || v==end)return false;
            found=true;
            while(v<end){if(!isdigit((unsigned char)*v))return false;*length=*length*10+(unsigned)(*v++-'0');if(*length>512)return false;}
        } else if(key==17&&!strncasecmp(p,"Transfer-Encoding",17))return false;
        else if(key==12&&!strncasecmp(p,"Content-Type",12)) {
            const char *type="application/x-www-form-urlencoded";
            form=(size_t)(end-v)==strlen(type)&&!strncasecmp(v,type,strlen(type));
        }
        p=end;
    }
    return found&&form;
}
void portal_poll(bool busy) {
    if(reboot_at && !busy && time_us_64()>=reboot_at)watchdog_reboot(0,0,0);
    if(!client)return;
    if(time_us_64()>expires){close_client();return;}
    if(ready && !responding) {
        ready=false;
        char *end=strstr(request,"\r\n\r\n");
        if(!end){if(received==sizeof(request)-1)respond("413 Payload Too Large","Request too large.");return;}
        if(!strncmp(request,"GET /logs.csv",13)) {
            start_download(busy);
        } else if(!strncmp(request,"GET ",4)) {
            char page[1400];snprintf(page,sizeof(page),
                "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>Pico setup</title>"
                "<meta name=referrer content=no-referrer>"
                "<h1>Programming logs</h1><p><a href='http://192.168.4.1/logs.csv?token=%s' download>"
                "Download programming logs</a></p><p>Saves a CSV copy. Records stay on the Pico. Wait for programming to finish before downloading.</p>"
                "<h1>Pico Programmer Wi-Fi</h1><p>Enter a 2.4 GHz WPA2 network. Saving restarts the Pico.</p>"
                "<form method=post action=/save><input type=hidden name=token value='%s'>"
                "<p><label>Wi-Fi name <input name=ssid maxlength=32 required></label></p>"
                "<p><label>Password <input name=password type=password minlength=8 maxlength=63 required></label></p>"
                "<button>Save and restart</button></form>",csrf,csrf);
            respond("200 OK",page);
        } else if(!strncmp(request,"POST /save HTTP/1.1\r\n",21)) {
            size_t length;
            if(!content_length(request,&length)){respond("400 Bad Request","Invalid form request.");return;}
            size_t offset=(size_t)(end+4-request);
            if(received<offset+length)return;
            if(received!=offset+length){respond("400 Bad Request","Invalid length.");return;}
            if(busy || reboot_at){respond("409 Conflict","Programmer busy. Try again after the job.");return;}
            wifi_credentials_t credentials={0};char token[33];
            if(!parse_form(end+4,credentials.ssid,credentials.password,token) || strcmp(token,csrf)) {
                respond("400 Bad Request","Invalid credentials or expired form. Reload this page.");return;
            }
            bool ok=storage_credentials_save(&credentials);memset(&credentials,0,sizeof(credentials));memset(request,0,sizeof(request));
            if(ok){respond("200 OK","Saved. The Pico is restarting; reconnect your phone to your normal network.");reboot_at=time_us_64()+3000000;}
            else respond("503 Service Unavailable","Could not save settings. No reboot was requested.");
        } else respond("405 Method Not Allowed","Use the Wi-Fi setup form.");
    }
    /* Each tcp_write copies a bounded block. Do not load the complete CSV into
     * RAM; do not reuse the response buffer until its block has been queued. */
    if(downloading && response_sent==response_size && download_offset<download_size) {
        size_t n=download_size-download_offset;if(n>sizeof(response))n=sizeof(response);
        int got=storage_export_read(download_offset,response,n);
        if(got!=(int)n){close_client();return;}
        download_offset+=(uint32_t)n;response_size=n;response_sent=0;
    }
    if(responding && response_sent<response_size) {
        size_t n=response_size-response_sent;if(n>tcp_sndbuf(client))n=tcp_sndbuf(client);
        if(n && tcp_write(client,response+response_sent,(u16_t)n,TCP_WRITE_FLAG_COPY)==ERR_OK)response_sent+=n;
        tcp_output(client);
    }
    if(responding && response_sent==response_size && (!downloading || download_offset==download_size) && !client->unsent && !client->unacked) {
        tcp_recv(client,NULL);tcp_err(client,NULL);
        if(tcp_close(client)==ERR_OK){client=NULL;end_download();}
        else { tcp_recv(client,receive_cb);tcp_err(client,error_cb); }
    }
}
bool portal_reboot_pending(void) { return reboot_at!=0; }
void portal_cancel_download(void) { if(downloading)close_client(); }
