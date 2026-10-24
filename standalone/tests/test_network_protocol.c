#include "services/protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void){
    https_url_t url;assert(parse_https_url("https://example.com/fw/image.bin",&url));
    assert(!parse_https_url("http://example.com/fw",&url));assert(!parse_https_url("https://a@b/fw",&url));
    assert(!parse_https_url("https://example.com/\r\nInjected: yes",&url));
    ota_manifest_t m;
    const char *valid="{\"revision\":40003,\"size\":524288,\"sha256\":\"3d656a56028eb7cda59d7ec13c2a07275e3ef281848754afe16ca96d6eca98b9\",\"url\":\"https://example.com/image.bin\",\"target\":\"STM32L476JE\",\"address\":\"0x08000000\"}";
    assert(parse_manifest(valid,&m));assert(m.size==524288 && m.revision==40003 && m.sha[0]==0x3d);
    char malformed[1024];strcpy(malformed,valid);char *p=strstr(malformed,"524288");memcpy(p,"524289",6);assert(!parse_manifest(malformed,&m));
    strcpy(malformed,valid);p=strstr(malformed,"STM32L476JE");p[0]='X';assert(!parse_manifest(malformed,&m));
    snprintf(malformed,sizeof(malformed),"%.*s,\"revision\":40004}",(int)strlen(valid)-1,valid);assert(!parse_manifest(malformed,&m));
    assert(!parse_manifest("{\"revision\":42949672960}",&m));assert(!parse_manifest("{}",&m));
    char ssid[33],password[64],token[33];
    assert(parse_form("ssid=My+WiFi&password=p%40ssword%26x&token=abcdef",ssid,password,token));assert(!strcmp(ssid,"My WiFi")&&!strcmp(password,"p@ssword&x"));
    assert(!parse_form("ssid=x&password=12345678&token=x&ssid=y",ssid,password,token));
    assert(!parse_form("ssid=x%00&password=12345678&token=x",ssid,password,token));
    assert(!parse_form("ssid=x&password=%xx&token=x",ssid,password,token));
    uint32_t length;
    strcpy(malformed,"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 524288\r\n\r\n");
    assert(parse_http_response(malformed,524288,&length) && length==524288);
    const char *bad_headers[]={
        "HTTP/1.1 302 Moved\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 8\r\nContent-Length: 8\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 524289\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 42949672960\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 8\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 8\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 8",
        "HTTP/1.1 200 OK\r\n Content-Length: 8\r\n\r\n",
        "HTTP/1.1 200 OK\r\n\r\n"};
    for(unsigned i=0;i<sizeof(bad_headers)/sizeof(bad_headers[0]);i++){
        strcpy(malformed,bad_headers[i]);assert(!parse_http_response(malformed,524288,&length));
    }
    for(unsigned i=0;i<20000;i++) {size_t n=(unsigned)rand()%sizeof(malformed);for(size_t j=0;j<n;j++)malformed[j]=(char)(1+rand()%127);malformed[n]=0;(void)parse_manifest(malformed,&m);(void)parse_https_url(malformed,&url);(void)parse_form(malformed,ssid,password,token);}
    puts("Protocol: manifest schema/target/length, duplicate rejection, HTTPS URL validation, credential form bounds and 20000 malformed inputs passed");return 0;
}
