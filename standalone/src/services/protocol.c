#include "services/protocol.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

bool parse_https_url(const char *s, https_url_t *out) {
    if (strncmp(s, "https://", 8)) return false;
    s += 8;
    const char *slash = strchr(s, '/');
    if (!slash || slash == s || (size_t)(slash-s) >= sizeof(out->host) || strlen(slash) >= sizeof(out->path)) return false;
    for (const char *p=s; p<slash; ++p)
        if (!(isalnum((unsigned char)*p) || *p=='.' || *p=='-')) return false;
    for (const char *p=slash; *p; ++p)
        if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127 || *p=='#' || *p=='\\') return false;
    memcpy(out->host,s,(size_t)(slash-s)); out->host[slash-s]=0;
    strcpy(out->path,slash);
    return true;
}
static void ws(const char **s) { while (**s && strchr(" \r\n\t", **s)) ++*s; }
static bool string(const char **s, char *out, size_t cap) {
    ws(s); if (*(*s)++ != '"') return false;
    size_t n=0;
    while (**s && **s!='"') {
        unsigned char c=(unsigned char)*(*s)++;
        if (c<32 || c>=127 || c=='\\' || n+1>=cap) return false;
        out[n++]=(char)c;
    }
    if (**s!='"') return false;
    ++*s; out[n]=0; return true;
}
static int hex(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}
bool parse_manifest(const char *s, ota_manifest_t *out) {
    memset(out,0,sizeof(*out)); ws(&s); if (*s++!='{') return false;
    unsigned seen=0;
    for (;;) {
        char key[20],value[520];
        if (!string(&s,key,sizeof(key))) return false;
        ws(&s); if (*s++!=':') return false; ws(&s);
        unsigned bit=0;
        if (!strcmp(key,"revision") || !strcmp(key,"size")) {
            if (!isdigit((unsigned char)*s)) return false;
            uint32_t n=0;
            do { unsigned d=(unsigned)(*s++-'0'); if (n>(UINT32_MAX-d)/10u) return false; n=n*10+d; } while (isdigit((unsigned char)*s));
            if (!strcmp(key,"revision")) { bit=1; out->revision=n; } else { bit=2; out->size=n; }
        } else {
            if (!string(&s,value,sizeof(value))) return false;
            if (!strcmp(key,"sha256")) {
                bit=4; if (strlen(value)!=64) return false;
                for (unsigned i=0;i<32;i++) { int a=hex(value[i*2]),b=hex(value[i*2+1]); if(a<0||b<0)return false; out->sha[i]=(uint8_t)(a*16+b); }
            } else if (!strcmp(key,"url")) { bit=8; if(!parse_https_url(value,&out->image))return false; }
            else if (!strcmp(key,"target")) { bit=16; if(strcmp(value,"STM32L476JE"))return false; }
            else if (!strcmp(key,"address")) { bit=32; if(strcmp(value,"0x08000000"))return false; }
            else return false;
        }
        if (seen&bit) return false;
        seen|=bit; ws(&s);
        if (*s=='}') { ++s; break; }
        if (*s++!=',') return false;
    }
    ws(&s);
    return !*s && seen==63 && out->revision && out->size>=8 && out->size<=0x80000 && !(out->size&7);
}
static bool decode(const char *p,size_t n,char *out,size_t cap) {
    size_t j=0;
    for(size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)p[i];
        if(c=='+')c=' ';
        else if(c=='%') {
            if(i+2>=n)return false;
            int a=hex(p[i+1]),b=hex(p[i+2]);if(a<0||b<0)return false;c=(unsigned char)(a*16+b);i+=2;
        }
        if(c<32 || c==127 || j+1>=cap)return false;
        out[j++]=(char)c;
    }
    out[j]=0;return true;
}
bool parse_form(const char *p,char *ssid,char *password,char *token) {
    unsigned seen=0;
    while(*p) {
        const char *end=strchr(p,'&');if(!end)end=p+strlen(p);
        const char *eq=memchr(p,'=',(size_t)(end-p));if(!eq)return false;
        unsigned bit;char *dest;size_t cap;
        size_t n=(size_t)(eq-p);
        if(n==4&&!memcmp(p,"ssid",4)){bit=1;dest=ssid;cap=33;}
        else if(n==8&&!memcmp(p,"password",8)){bit=2;dest=password;cap=64;}
        else if(n==5&&!memcmp(p,"token",5)){bit=4;dest=token;cap=33;}
        else return false;
        if((seen&bit)||!decode(eq+1,(size_t)(end-eq-1),dest,cap))return false;
        seen|=bit;p=*end?end+1:end;
    }
    return seen==7 && *ssid && strlen(password)>=8;
}
bool parse_http_response(char *header,uint32_t limit,uint32_t *length) {
    if(strncmp(header,"HTTP/1.1 200 ",13) && strncmp(header,"HTTP/1.0 200 ",13))return false;
    char *p=strstr(header,"\r\n");if(!p)return false;p+=2;bool length_seen=false;
    while(*p && *p!='\r') {
        char *end=strstr(p,"\r\n");if(!end)return false;*end=0;
        char *colon=strchr(p,':');if(!colon || colon==p)return false;
        for(char *c=p;c<colon;c++) {
            if(!isalnum((unsigned char)*c) && *c!='-')return false;
            *c=(char)tolower((unsigned char)*c);
        }
        *colon++=0;while(*colon==' ' || *colon=='\t')++colon;
        if(!strcmp(p,"content-length")) {
            if(length_seen || !isdigit((unsigned char)*colon))return false;
            uint32_t value=0;
            while(isdigit((unsigned char)*colon)) { unsigned d=(unsigned)(*colon++-'0');if(value>(UINT32_MAX-d)/10)return false;value=value*10+d; }
            while(*colon==' ' || *colon=='\t')colon++;
            if(*colon || value>limit)return false;
            *length=value;length_seen=true;
        } else if(!strcmp(p,"transfer-encoding") || !strcmp(p,"content-encoding"))return false;
        p=end+2;
    }
    return length_seen && !strcmp(p,"\r\n");
}
