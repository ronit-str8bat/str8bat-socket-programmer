/* Local HTTP API on the WPA2 AP. One bounded TCP session, no whole-file CSV
 * allocation. Callbacks collect bytes only; core-0 poll performs filesystem IO.
 * Deletion requires an acknowledged download, matching ETag and boot token.
 * The application must save the response successfully before sending DELETE.
 */
#include "services/portal.h"
#include "services/local_api.h"
#include "services/network.h"
#include "programmer_config.h"
#include "firmware_version.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
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
/* Phone browsers add different headers. Keep enough room for those headers plus one small BIN packet. */
static char request[8192],response[4096],token[33],download_etag[65],receipt_etag[65];
static size_t received,response_size,response_sent;
static uint64_t expires;
static bool ready,responding,downloading,download_factory,overflow;
static uint32_t download_size,download_offset;
static unsigned download_file;
static bool factory_distribution;
static bool download_acknowledged(void) {
    return downloading && download_offset==download_size && response_sent==response_size &&
        client && !client->unsent && !client->unacked;
}
static void end_download(void) {
    if(downloading && !download_factory)local_snapshot_end();
    if(download_factory)factory_distribution=false;
    downloading=download_factory=false;
}
static void close_client(void) {
    if(client){tcp_arg(client,NULL);tcp_recv(client,NULL);tcp_err(client,NULL);tcp_abort(client);client=NULL;}
    received=response_size=response_sent=0;ready=responding=false;end_download();
}
static void error_cb(void *arg,err_t err) { (void)arg;(void)err;client=NULL;ready=responding=false;end_download(); }
static err_t receive_cb(void *arg,struct tcp_pcb *pcb,struct pbuf *p,err_t err) {
    (void)arg;(void)err;
    /* WebView closes its response connection when handing an attachment to
     * Android's download service. Release that reader for the service's GET.
     * A half-close before we start responding still permits a response. */
    if(!p){
        if(responding){
            /* ACK+FIN can arrive before poll observes completion. Preserve a
             * fully delivered receipt, but never one for a WebView handoff. */
            if(download_file==0 && download_acknowledged())memcpy(receipt_etag,download_etag,sizeof(receipt_etag));
            close_client();return ERR_ABRT;
        }
        return ERR_OK;
    }
    if(responding || p->tot_len>=sizeof(request)-received)overflow=true;
    else {
        pbuf_copy_partial(p,request+received,p->tot_len,0);received+=p->tot_len;
        if(memchr(request,0,received))overflow=true;
        request[received]=0;
    }
    tcp_recved(pcb,p->tot_len);pbuf_free(p);ready=true;return ERR_OK;
}
static err_t accept_cb(void *arg,struct tcp_pcb *pcb,err_t err) {
    (void)arg;if(err!=ERR_OK || client){tcp_abort(pcb);return ERR_ABRT;}
    client=pcb;received=response_size=response_sent=0;ready=responding=overflow=false;request[0]=0;
    expires=time_us_64()+10000000u;tcp_recv(pcb,receive_cb);tcp_err(pcb,error_cb);return ERR_OK;
}
/* Return -1 for malformed/duplicate headers, 0 absent, 1 present. */
static int field(const char *name,char *out,size_t cap) {
    bool found=false;size_t keylen=strlen(name);
    for(const char *p=strstr(request,"\r\n");p && p[2] && p[2]!='\r';) {
        p+=2;const char *end=strstr(p,"\r\n");if(!end)return -1;
        const char *colon=memchr(p,':',(size_t)(end-p));
        if(!colon || p[0]==' ' || p[0]=='\t')return -1;
        if((size_t)(colon-p)==keylen && !strncasecmp(p,name,keylen)) {
            if(found)return -1;
            found=true;const char *v=colon+1;while(v<end && (*v==' ' || *v=='\t'))v++;
            while(end>v && (end[-1]==' ' || end[-1]=='\t'))end--;
            size_t n=(size_t)(end-v);if(n>=cap)return -1;memcpy(out,v,n);out[n]=0;
        }
        p=strstr(p,"\r\n");
    }
    return found?1:0;
}
static bool local_host(void) {
    char h[32];const char *ip=FACTORY_ROLE==FACTORY_ROLE_SLAVE?"192.168.5.1":"192.168.4.1";
    char with_port[20];snprintf(with_port,sizeof(with_port),"%s:80",ip);
    return field("Host",h,sizeof(h))==1 && (!strcmp(h,ip) || !strcmp(h,with_port));
}
static bool authorized(void) {
    char h[64];return field("X-Programmer-Token",h,sizeof(h))==1 && !strcmp(h,token);
}
static bool factory_authorized(void) {
    char key[96];return field("X-Factory-Key",key,sizeof(key))==1 && !strcmp(key,FACTORY_GROUP_KEY);
}
static void respond(const char *status,const char *type,const char *body) {
    int n=snprintf(response,sizeof(response),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\n"
        "Connection: close\r\nX-Content-Type-Options: nosniff\r\n"
        "Content-Security-Policy: default-src 'none'; script-src 'nonce-%s'; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'\r\n\r\n%s",
        status,type,(unsigned)strlen(body),!strncmp(type,"text/html",9)?token:"disabled",body);
    if(n<0 || n>=(int)sizeof(response)){close_client();return;}
    response_size=(size_t)n;response_sent=0;responding=true;
}
static void message(const char *status,const char *body) { respond(status,"text/plain; charset=utf-8",body); }
static bool captive_probe(const char *path) {
    /* HTTP connectivity checks used by Android, Apple and Windows. Redirect
     * only these public discovery paths; foreign-host /logs stays forbidden. */
    static const char *const paths[]={"/generate_204","/gen_204","/hotspot-detect.html",
        "/library/test/success.html","/connecttest.txt","/ncsi.txt","/redirect","/canonical.html","/success.txt","/fwlink"};
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++) {
        size_t n=strlen(paths[i]);
        if(!strncmp(path,paths[i],n) && (path[n]==0 || path[n]=='?'))return true;
    }
    return false;
}
static void portal_redirect(void) {
    const char *ip=FACTORY_ROLE==FACTORY_ROLE_SLAVE?"192.168.5.1":"192.168.4.1";
    int n=snprintf(response,sizeof(response),"HTTP/1.1 302 Found\r\nLocation: http://%s/\r\n"
        "Content-Length: 0\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",ip);
    if(n<0 || n>=(int)sizeof(response)){close_client();return;}
    response_size=(size_t)n;response_sent=0;responding=true;
}
static void page(void) {
    char body[4096];
#if FACTORY_ROLE == FACTORY_ROLE_MASTER
    snprintf(body,sizeof(body),
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>Factory master</title>"
        "<h1>Factory master programmer</h1><p id=s>Connecting...</p><p>Keep this Wi-Fi connection despite the no-Internet notice.</p>"
        "<button id=t>Set clock from this phone</button> <input id=f type=file accept='.bin,application/octet-stream'> <button id=u>Upload BIN to slaves</button>"
        "<p>Before the first upload, enter the three 16-character slave Pico IDs, separated by commas.</p>"
        "<input id=p size=58 placeholder='0011223344556677,0011223344556678,0011223344556679'> <button id=ps>Save slave IDs</button>"
        "<p><button id=d>Download this Pico CSV</button> <button id=c disabled>Clear downloaded logs</button></p>"
        "<script nonce='%s'>let tok='',tag='';const s=document.getElementById('s'),c=document.getElementById('c');"
        "async function req(u,o){let r=await fetch(u,o);if(!r.ok)throw Error(await r.text());return r}"
        "async function sync(){await req('/sync-time',{method:'POST',headers:{'Content-Type':'application/json','X-Programmer-Token':tok},body:JSON.stringify({unix_seconds:Math.floor(Date.now()/1000)})})}"
        "document.getElementById('t').onclick=()=>sync().then(()=>s.textContent='Clock set.').catch(e=>s.textContent=e.message);"
        "document.getElementById('ps').onclick=()=>req('/factory/pair',{method:'POST',headers:{'Content-Type':'text/plain','X-Programmer-Token':tok},body:document.getElementById('p').value.trim()}).then(()=>s.textContent='Three slave IDs saved.').catch(e=>s.textContent=e.message);"
        "document.getElementById('u').onclick=async()=>{try{let f=document.getElementById('f').files[0];if(!f)throw Error('Select an STM32 BIN file.');if(f.size<8||f.size>524288||f.size%%8)throw Error('BIN must be 8 bytes to 512 KiB and a multiple of 8 bytes.');let a=new Uint8Array(await f.arrayBuffer());await req('/bin/begin',{method:'POST',headers:{'Content-Type':'application/json','X-Programmer-Token':tok},body:JSON.stringify({size:f.size,crc32:'00000000'})});for(let o=0;o<a.length;o+=256){let e=Math.min(o+256,a.length),h='';for(let n=o;n<e;n++){let v=a[n];h+='0123456789abcdef'[v>>4]+'0123456789abcdef'[v&15]}s.textContent='Uploading '+Math.min(100,Math.round(e*100/a.length))+'%%';await req('/bin/chunk-get?offset='+o+'&data='+h,{headers:{'X-Programmer-Token':tok}})}await req('/bin/commit?token='+tok);s.textContent='BIN verified. Sending to the paired slaves.'}catch(e){s.textContent=e.message}};"
        "document.getElementById('d').onclick=async()=>{try{c.disabled=true;let r=await req('/logs',{method:'HEAD'});tag=r.headers.get('ETag');window.location.assign('/logs?etag='+tag.slice(1,-1));c.disabled=false}catch(e){s.textContent=e.message}};"
        "c.onclick=async()=>{try{await req('/logs',{method:'DELETE',headers:{'If-Match':tag,'X-Programmer-Token':tok}});c.disabled=true}catch(e){s.textContent=e.message}};"
        "(async()=>{try{let j=await(await req('/status')).json();tok=j.token;document.getElementById('p').value=j.peers||'';s.textContent=j.ssid+' — '+j.status+' ('+j.build+')'}catch(e){s.textContent=e.message}})();</script>",token);
#else
    snprintf(body,sizeof(body),
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>Pico programmer</title>"
        "<h1>STM32 programming logs</h1><p id=s>Connecting...</p><p>Keep this Wi-Fi connection despite the no-Internet notice.</p>"
        "<button id=t>Set clock from this phone</button> <button id=d>Download CSV</button> "
        "<button id=c disabled>Clear downloaded logs</button><p>Clear only after confirming the CSV was saved. New records are protected.</p>"
        "<p>Archives, if present: <a href=/logs/utc-archive>previous UTC logs</a> / <a href=/logs/legacy-current>old logs</a> / <a href=/logs/legacy-current-2>older logs</a> / <a href=/logs/legacy-pending>old pending upload</a>.</p>"
        "<script nonce='%s'>let tok='',tag='';const s=document.getElementById('s'),c=document.getElementById('c');"
        "async function req(u,o){let r=await fetch(u,o);if(!r.ok)throw Error(await r.text());return r}"
        "async function sync(){await req('/sync-time',{method:'POST',headers:{'Content-Type':'application/json','X-Programmer-Token':tok},"
        "body:JSON.stringify({unix_seconds:Math.floor(Date.now()/1000)})});s.textContent='Clock set to IST (UTC+05:30). Ready.'}"
        "document.getElementById('t').onclick=()=>sync().catch(e=>s.textContent=e.message);"
        "document.getElementById('d').onclick=async()=>{try{c.disabled=true;let r=await req('/logs',{method:'HEAD'});"
        "tag=r.headers.get('ETag');if(!tag||!/^\"[0-9a-f]{64}\"$/.test(tag))throw Error('Invalid download response');"
        "s.textContent='Save programming_logs.csv. Clear only after the phone confirms it was saved.';c.disabled=false;"
        "window.location.assign('/logs?etag='+tag.slice(1,-1))}catch(e){s.textContent=e.message}};"
        "c.onclick=async()=>{if(!confirm('Have you saved the downloaded CSV? Clear those records?'))return;try{await req('/logs',{method:'DELETE',"
        "headers:{'If-Match':tag,'X-Programmer-Token':tok}});c.disabled=true;s.textContent='Downloaded logs cleared.'}catch(e){s.textContent=e.message}};"
        "(async()=>{try{let j=await(await req('/status')).json();tok=j.token;"
        "if(!j.clock_set)await sync();else s.textContent='Hardware clock running in IST. Ready.'}catch(e){s.textContent=e.message}})();</script>",token);
#endif
    respond("200 OK","text/html; charset=utf-8",body);
}
static void start_download(unsigned file,bool busy,bool head,const char *expected) {
    if(busy){message("409 Conflict","Programming in progress. Retry when finished.");return;}
    if(!local_snapshot_begin(file,&download_size,download_etag)){message("404 Not Found","CSV unavailable or archive absent.");return;}
    if(expected && strcmp(expected,download_etag)) {
        local_snapshot_end();message("412 Precondition Failed","Logs changed. Return to the page and tap Download CSV again.");return;
    }
    downloading=true;download_file=file;download_offset=0;
    const char *name=file==0?"programming_logs.csv":file==1?"programming_logs.v1.csv":file==2?"upload.v1.csv":"programming_logs.utc.csv";
    int n=snprintf(response,sizeof(response),
        "HTTP/1.1 200 OK\r\nContent-Type: text/csv; charset=utf-8\r\nContent-Disposition: attachment; filename=\"%s\"\r\n"
        "Content-Length: %lu\r\nETag: \"%s\"\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
        name,(unsigned long)download_size,download_etag);
    if(n<0 || n>=(int)sizeof(response)){close_client();return;}
    response_size=(size_t)n;response_sent=0;responding=true;expires=time_us_64()+120000000u;
    /* HEAD supplies metadata only; it must never authorize deletion. The
     * following real HTTP GET is bound to this ETag and streams the bytes. */
    if(head)end_download();
}
static bool query_value(const char *path,const char *name,char *out,size_t cap) {
    const char *q=strchr(path,'?');size_t want=strlen(name);if(!q)return false;q++;
    while(*q) {
        const char *end=strchr(q,'&');if(!end)end=q+strlen(q);
        const char *eq=memchr(q,'=',(size_t)(end-q));
        if(eq && (size_t)(eq-q)==want && !memcmp(q,name,want)) {
            size_t n=(size_t)(end-eq-1);if(!n || n>=cap)return false;memcpy(out,eq+1,n);out[n]=0;return true;
        }
        q=*end?end+1:end;
    }
    return false;
}
/* The Android captive WebView can coalesce POST upload bodies into a request
 * that exceeds its advertised small chunk.  Factory phone uploads therefore
 * use a bounded hexadecimal query value instead. */
static bool query_hex_value(const char *path,const char *name,uint8_t *out,size_t cap,size_t *written) {
    const char *q=strchr(path,'?');size_t want=strlen(name);if(!q)return false;q++;
    while(*q) {
        const char *end=strchr(q,'&');if(!end)end=q+strlen(q);
        const char *eq=memchr(q,'=',(size_t)(end-q));
        if(eq && (size_t)(eq-q)==want && !memcmp(q,name,want)) {
            size_t n=(size_t)(end-eq-1);if(!n || (n&1u) || n/2u>cap)return false;
            for(size_t i=0;i<n/2u;i++) {
                unsigned char a=(unsigned char)eq[1+i*2u],b=(unsigned char)eq[2+i*2u];
                int hi=isdigit(a)?a-'0':isxdigit(a)?(tolower(a)-'a'+10):-1;
                int lo=isdigit(b)?b-'0':isxdigit(b)?(tolower(b)-'a'+10):-1;
                if(hi<0 || lo<0)return false;
                out[i]=(uint8_t)((hi<<4)|lo);
            }
            *written=n/2u;return true;
        }
        q=*end?end+1:end;
    }
    return false;
}
static bool parse_upload_begin(const char *body,uint32_t *size,uint32_t *crc) {
    unsigned long s=0,c=0;char hex[9]={0};
    if(sscanf(body,"{\"size\":%lu,\"crc32\":\"%8[0-9a-fA-F]\"}",&s,hex)!=2 || strlen(hex)!=8)return false;
    if(sscanf(hex,"%lx",&c)!=1 || s<8u || s>FACTORY_BIN_MAX_SIZE || (s&7u))return false;
    *size=(uint32_t)s;*crc=(uint32_t)c;return true;
}
#if FACTORY_ROLE == FACTORY_ROLE_MASTER
static stored_image_t factory_master_image(void) {
    /* A newly installed Master can authenticate the embedded production BIN
     * immediately. Once a factory upload is committed, that verified BIN
     * becomes the authoritative image instead. */
    return factory_bin_available()?factory_bin_image():storage_image();
}
static bool factory_id_from_path(const char *path,char id[17]) {
    return query_value(path,"id",id,17) && factory_peer_allowed(id);
}
static void start_factory_download(const char *id) {
    (void)id;
    download_factory=true;downloading=true;download_file=99;download_offset=0;
    stored_image_t image=factory_master_image();download_size=image.size;
    int n=snprintf(response,sizeof(response),
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %lu\r\n"
        "X-Bin-CRC32: %08lx\r\nX-Bin-Generation: %lu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        (unsigned long)image.size,(unsigned long)image.crc,(unsigned long)image.revision);
    if(n<0 || n>=(int)sizeof(response)){close_client();return;}
    response_size=(size_t)n;response_sent=0;responding=true;factory_distribution=true;expires=time_us_64()+300000000u;
}
#endif
static void handle(bool busy) {
    char *end=strstr(request,"\r\n\r\n");
    if(overflow){message("413 Payload Too Large","Request too large.");return;}
    if(!end)return;
    char method[8],path[768],version[16],extra;
    char *line=strstr(request,"\r\n");if(!line)return;
    *line=0;int parsed=sscanf(request,"%7s %767s %15s %c",method,path,version,&extra);*line='\r';
    if(parsed!=3 || (strcmp(version,"HTTP/1.1") && strcmp(version,"HTTP/1.0"))){message("400 Bad Request","Unsupported HTTP version.");return;}
    char value[80];size_t length=0;
    int cl=field("Content-Length",value,sizeof(value));
    if(cl<0 || field("Transfer-Encoding",value,sizeof(value))!=0){message("400 Bad Request","Invalid framing.");return;}
    if(cl==1) {
        field("Content-Length",value,sizeof(value));
        if(!value[0]){message("400 Bad Request","Invalid length.");return;}
        for(const char *v=value;*v;v++) {
            if(!isdigit((unsigned char)*v) || length>128){message("400 Bad Request","Invalid length.");return;}
            length=length*10+(unsigned)(*v-'0');
        }
        if(length>1024){message("413 Payload Too Large","Body too large.");return;}
    }
    size_t offset=(size_t)(end+4-request);
    if(received<offset+length)return;
    if(received!=offset+length || (strcmp(method,"POST") && length)){message("400 Bad Request","Unexpected body.");return;}
    if(!strcmp(method,"GET") && captive_probe(path)) {
        char host[128];
        if(field("Host",host,sizeof(host))!=1 || !host[0]){message("400 Bad Request","Invalid Host.");return;}
        portal_redirect();return;
    }
    if(!local_host()){message("403 Forbidden","Open http://192.168.4.1/ directly.");return;}
    if(!strcmp(method,"GET") && !strcmp(path,"/")){page();return;}
    if(strcmp(version,"HTTP/1.1")){message("400 Bad Request","Use HTTP/1.1 for the API.");return;}
    if(!strcmp(method,"GET") && !strcmp(path,"/status")) {
        char body[768],date[20]="UNSET",ssid[33],peers[55]="";if(local_time_valid())local_format_ist(local_time_now(),date);
        local_device_ssid(ssid);
#if FACTORY_ROLE == FACTORY_ROLE_MASTER
        snprintf(peers,sizeof(peers),"%s,%s,%s",factory_peer_id(0),factory_peer_id(1),factory_peer_id(2));
#endif
        snprintf(body,sizeof(body),"{\"build\":\"%s\",\"device_id\":\"%s\",\"ssid\":\"%s\",\"firmware_version\":\"%s\",\"clock_set\":%s,\"clock_source\":\"RP2350_POWMAN\",\"ist\":\"%s\",\"timezone\":\"IST\",\"status\":\"%s\",\"busy\":%s,\"factory_bin\":%s,\"factory_transfer\":%s,\"peers\":\"%s\",\"token\":\"%s\"}",
            PROGRAMMER_BUILD_ID,storage_device_id(),ssid,TARGET_FIRMWARE_VERSION,local_time_valid()?"true":"false",date,services_status(),busy?"true":"false",
            factory_bin_available()?"true":"false",factory_distribution?"true":"false",peers,token);
        respond("200 OK","application/json",body);return;
    }
    if(!strcmp(method,"HEAD") && !strcmp(path,"/logs")){start_download(0,busy,true,NULL);return;}
    if(!strcmp(method,"GET") && !strncmp(path,"/logs?etag=",11)) {
        const char *etag=path+11;
        if(strlen(etag)!=64 || strspn(etag,"0123456789abcdef")!=64){message("400 Bad Request","Invalid download ETag.");return;}
        start_download(0,busy,false,etag);return;
    }
    if(!strcmp(method,"GET") && !strcmp(path,"/logs")){start_download(0,busy,false,NULL);return;}
    if(!strcmp(method,"GET") && !strcmp(path,"/logs/legacy-current")){start_download(1,busy,false,NULL);return;}
    if(!strcmp(method,"GET") && !strcmp(path,"/logs/legacy-current-2")){start_download(4,busy,false,NULL);return;}
    if(!strcmp(method,"GET") && !strcmp(path,"/logs/legacy-pending")){start_download(2,busy,false,NULL);return;}
    if(!strcmp(method,"GET") && !strcmp(path,"/logs/utc-archive")){start_download(3,busy,false,NULL);return;}
    if(!strcmp(method,"POST") && !strcmp(path,"/factory/pair")) {
#if FACTORY_ROLE == FACTORY_ROLE_MASTER
        if(!authorized()){message("403 Forbidden","Missing programmer token.");return;}
        if(busy || cl!=1 || !factory_peers_set(end+4)){message("400 Bad Request","Enter exactly three unique 16-character Pico IDs.");return;}
        message("204 No Content","");return;
#else
        message("404 Not Found","Factory pairing is available only on the master.");return;
#endif
    }
    if(!strcmp(method,"POST") && (!strcmp(path,"/bin/begin") || !strncmp(path,"/bin/begin?",11))) {
        bool auth=FACTORY_ROLE==FACTORY_ROLE_SLAVE?factory_authorized():authorized();
        uint32_t size,crc,received=local_time_valid()?local_time_now():0;
        char value2[16];
        if(FACTORY_ROLE==FACTORY_ROLE_SLAVE && query_value(path,"received",value2,sizeof(value2))) {
            unsigned long epoch=0;if(sscanf(value2,"%lu",&epoch)!=1){message("400 Bad Request","Invalid receive time.");return;}received=(uint32_t)epoch;
        }
        if(!auth){message("403 Forbidden","Upload is not authorized.");return;}
        /* An interrupted phone upload must not prevent a clean retry. */
        if(FACTORY_ROLE==FACTORY_ROLE_MASTER)factory_bin_abort();
        if(busy || cl!=1 || !parse_upload_begin(end+4,&size,&crc) || !factory_bin_begin(size,crc,received)) {
            message("409 Conflict","Cannot begin BIN upload.");return;
        }
        message("204 No Content","");return;
    }
    if(!strcmp(method,"POST") && !strncmp(path,"/bin/chunk?",11)) {
        bool auth=FACTORY_ROLE==FACTORY_ROLE_SLAVE?factory_authorized():authorized();char off[16];unsigned long offset=0;
        if(!auth || !query_value(path,"offset",off,sizeof(off)) || sscanf(off,"%lu",&offset)!=1 || !length ||
           !factory_bin_write((uint32_t)offset,end+4,length)) {message("409 Conflict","BIN chunk rejected.");return;}
        message("204 No Content","");return;
    }
    if(!strcmp(method,"GET") && !strncmp(path,"/bin/chunk-get?",15)) {
        bool auth=FACTORY_ROLE==FACTORY_ROLE_SLAVE?factory_authorized():authorized();char off[16];unsigned long offset=0;
        uint8_t chunk[256];size_t chunk_size=0;
        if(!auth || !query_value(path,"offset",off,sizeof(off)) || sscanf(off,"%lu",&offset)!=1 ||
           !query_hex_value(path,"data",chunk,sizeof(chunk),&chunk_size) ||
           !factory_bin_write((uint32_t)offset,chunk,chunk_size)) {message("409 Conflict","BIN chunk rejected.");return;}
        message("204 No Content","");return;
    }
    if((!strcmp(method,"POST") && !strcmp(path,"/bin/commit")) ||
       (!strcmp(method,"GET") && !strncmp(path,"/bin/commit?",12))) {
        bool auth=FACTORY_ROLE==FACTORY_ROLE_SLAVE?factory_authorized():authorized();
        /* Some Android captive WebViews drop custom headers from an empty
         * POST. The Master page supplies its one-time local token in the
         * final GET query as a reliable fallback. */
        char commit_token[40];
        if(!auth && FACTORY_ROLE==FACTORY_ROLE_MASTER && query_value(path,"token",commit_token,sizeof(commit_token)) &&
           !strcmp(commit_token,token))auth=true;
        if(!auth || (strcmp(method,"GET") && cl!=0)){message("403 Forbidden","BIN commit is not authorized.");return;}
        if(!factory_bin_commit()){message("409 Conflict",factory_bin_last_error());return;}
#if FACTORY_ROLE == FACTORY_ROLE_MASTER
#endif
        message("204 No Content","");return;
    }
    if(!strcmp(method,"GET") && !strncmp(path,"/factory/job?",13)) {
#if FACTORY_ROLE == FACTORY_ROLE_MASTER
        char id[17];if(!factory_authorized() || !factory_id_from_path(path,id)){message("403 Forbidden","Unpaired slave.");return;}
        char crc[16];unsigned long current_crc=0;
        if(!query_value(path,"crc",crc,sizeof(crc)) || strlen(crc)!=8 || strspn(crc,"0123456789abcdefABCDEF")!=8 ||
           sscanf(crc,"%lx",&current_crc)!=1){message("400 Bad Request","Missing slave image CRC.");return;}
        stored_image_t image=factory_master_image();
        if((uint32_t)current_crc==image.crc){message("204 No Content","");return;}
        char body[192];
        snprintf(body,sizeof(body),"{\"generation\":%lu,\"size\":%lu,\"crc32\":\"%08lx\",\"received\":%lu}",
                 (unsigned long)image.revision,(unsigned long)image.size,(unsigned long)image.crc,(unsigned long)factory_bin_received_epoch());
        respond("200 OK","application/json",body);return;
#else
        message("404 Not Found","Slave does not distribute BIN files.");return;
#endif
    }
    if(!strcmp(method,"GET") && !strncmp(path,"/factory/bin?",13)) {
#if FACTORY_ROLE == FACTORY_ROLE_MASTER
        char id[17];if(!factory_authorized() || !factory_id_from_path(path,id)){message("403 Forbidden","Unpaired slave.");return;}
        start_factory_download(id);return;
#else
        message("404 Not Found","Slave does not distribute BIN files.");return;
#endif
    }
    if(!strcmp(method,"GET") && !strncmp(path,"/factory/report?",16)) {
#if FACTORY_ROLE == FACTORY_ROLE_MASTER
        char id[17],generation[16],result[16];
        if(!factory_authorized() || !factory_id_from_path(path,id) || !query_value(path,"generation",generation,sizeof(generation)) ||
           !query_value(path,"result",result,sizeof(result))){message("403 Forbidden","Unpaired slave.");return;}
        /* The report is retained for protocol acknowledgement. A missing
         * slave never blocks other slaves or a future upload. */
        (void)id;(void)generation;(void)result;message("204 No Content","");return;
#else
        message("404 Not Found","Slave does not receive reports.");return;
#endif
    }
    /* Keep /time as an alias for existing mobile clients. */
    if(!strcmp(method,"POST") && (!strcmp(path,"/sync-time") || !strcmp(path,"/time"))) {
        if(!authorized()){message("403 Forbidden","Missing or expired programmer token.");return;}
        if(busy){message("409 Conflict","Set time after programming finishes.");return;}
        uint32_t epoch;
        if(cl!=1 || field("Content-Type",value,sizeof(value))!=1 || strcmp(value,"application/json") ||
           !local_parse_time(end+4,&epoch)){message("400 Bad Request","Expected JSON unix_seconds, UTC, year 2025..2099.");return;}
        if(!local_time_set(epoch)){message("503 Service Unavailable","Hardware clock did not start. Retry synchronization.");return;}
        message("204 No Content","");return;
    }
    if(!strcmp(method,"DELETE") && !strcmp(path,"/logs")) {
        if(!authorized()){message("403 Forbidden","Missing or expired programmer token.");return;}
        if(busy){message("409 Conflict","Programming in progress.");return;}
        if(field("If-Match",value,sizeof(value))!=1 || strlen(value)!=66 || value[0]!='\"' || value[65]!='\"') {
            message("428 Precondition Required","Supply the quoted ETag from the completed GET /logs.");return;
        }
        value[65]=0;
        if(!receipt_etag[0] || strcmp(value+1,receipt_etag)){message("409 Conflict","Complete GET /logs before confirming deletion.");return;}
        int r=local_logs_clear(value+1);
        if(r==1){message("412 Precondition Failed","Logs changed. Download the new CSV before clearing.");return;}
        if(r<0){message("503 Service Unavailable","Logs could not be cleared.");return;}
        receipt_etag[0]=0;message("204 No Content","");return;
    }
    message("404 Not Found","Unknown programmer endpoint.");
}
bool portal_start(void) {
    for(unsigned i=0;i<4;i++)snprintf(token+i*8,9,"%08lx",(unsigned long)get_rand_32());
    uint8_t subnet=FACTORY_ROLE==FACTORY_ROLE_SLAVE?5:4;
    ip_addr_t ip,mask;IP4_ADDR(&ip,192,168,subnet,1);IP4_ADDR(&mask,255,255,255,0);
    struct netif *nif=&cyw43_state.netif[CYW43_ITF_AP];netif_set_addr(nif,ip_2_ip4(&ip),ip_2_ip4(&mask),ip_2_ip4(&ip));
    dhcp_server_init(&dhcp,nif,&ip,&mask);dns_server_init(&dns,nif,&ip);
    if(!dhcp.udp || !dns.udp)return false;
    listener=tcp_new_ip_type(IPADDR_TYPE_V4);if(!listener)return false;
    if(tcp_bind(listener,&ip,80)!=ERR_OK){tcp_abort(listener);listener=NULL;return false;}
    struct tcp_pcb *bound=listener;listener=tcp_listen_with_backlog(bound,1);
    if(!listener){tcp_abort(bound);return false;}tcp_accept(listener,accept_cb);return true;
}
void portal_poll(bool busy) {
    if(!client)return;
    if(time_us_64()>expires){close_client();return;}
    if(ready && !responding){ready=false;handle(busy);}
    /* CSV payload is streamed in 1 KiB blocks, independent of file length. */
    if(downloading && response_sent==response_size && download_offset<download_size) {
        size_t n=download_size-download_offset;if(n>1024)n=1024;
        int got;
#if FACTORY_ROLE == FACTORY_ROLE_MASTER
        if(download_factory) {
            stored_image_t image=factory_master_image();
            if(download_offset>image.size || n>image.size-download_offset){close_client();return;}
            memcpy(response,image.data+download_offset,n);got=(int)n;
        } else
#endif
        got=local_snapshot_read(download_offset,response,n);
        if(got!=(int)n){close_client();return;}
        download_offset+=(uint32_t)n;response_size=n;response_sent=0;
    }
    if(responding && response_sent<response_size) {
        size_t n=response_size-response_sent;if(n>tcp_sndbuf(client))n=tcp_sndbuf(client);
        if(n && tcp_write(client,response+response_sent,(u16_t)n,TCP_WRITE_FLAG_COPY)==ERR_OK)response_sent+=n;
        tcp_output(client);
    }
    /* API replies are deliberately connection-close replies.  Close as soon
     * as a short reply is queued: some Android WebViews otherwise issue the
     * next fetch while the old connection is still waiting for its ACK, and
     * that next upload packet was incorrectly reported as too large.  File
     * downloads still wait for full acknowledgement. */
    if(responding && response_sent==response_size && (!downloading || download_offset==download_size) &&
       (!downloading || (!client->unsent && !client->unacked))) {
        tcp_recv(client,NULL);tcp_err(client,NULL);
        if(tcp_close(client)==ERR_OK) {
            if(downloading && download_file==0)memcpy(receipt_etag,download_etag,sizeof(receipt_etag));
            client=NULL;end_download();
        } else {tcp_recv(client,receive_cb);tcp_err(client,error_cb);}
    }
}
bool portal_reboot_pending(void) { return false; }
void portal_cancel_download(void) { if(downloading)close_client(); }
bool portal_factory_transfer_active(void) { return factory_distribution || factory_bin_upload_active(); }
