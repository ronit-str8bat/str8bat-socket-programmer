#include "services/usb_export.h"
#if STANDALONE_USB_DIAGNOSTICS
#include "services/network.h"
#include "services/storage.h"
#include "crc32.h"
#include "crc32_nibble.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

/* Request/response framing lets the laptop retry a dropped USB line without
 * duplicating bytes. One read-only snapshot is held until CLOSE, disconnect,
 * timeout or physical START. No CSV data or credentials are accepted as input. */
static bool active,overflow;
static char token[33],line[96];
static unsigned line_size;
static uint32_t total,next_offset,last_offset,crc;
static uint64_t idle_deadline,session_deadline,input_deadline;

bool usb_export_active(void) { return active; }
void usb_export_cancel(void) {
    if(active)storage_export_end();
    active=false;
}
static void error(const char *id,const char *reason) {
    printf("\nPICOCSV %s ERROR %s\n",id,reason);
}
static bool valid_token(const char *id) {
    if(strlen(id)!=32)return false;
    for(unsigned i=0;i<32;i++)if(!((id[i]>='0'&&id[i]<='9') || (id[i]>='a'&&id[i]<='f')))return false;
    return true;
}
static bool number(const char *text,uint32_t *out) {
    if(!*text)return false;
    uint32_t value=0;
    for(;*text;text++) {
        if(*text<'0'||*text>'9')return false;
        unsigned digit=(unsigned)(*text-'0');
        if(value>(UINT32_MAX-digit)/10u)return false;
        value=value*10u+digit;
    }
    *out=value;return true;
}
static void command(char *text,bool busy) {
    char *parts[5];unsigned count=0;
    char *part=strtok(text," ");
    while(part && count<5){parts[count++]=part;part=strtok(NULL," ");}
    if(part || count<3 || strcmp(parts[0],"PICOCSV") || !valid_token(parts[2]))return;
    const char *op=parts[1],*id=parts[2];
    if(!strcmp(op,"OPEN") && count==3) {
        if(busy){error(id,"BUSY");return;}
        if(active && strcmp(id,token)){error(id,"BUSY");return;}
        if(!active) {
            if(!services_export_begin(&total)){error(id,"UNAVAILABLE");return;}
            strcpy(token,id);active=true;next_offset=0;last_offset=UINT32_MAX;crc=0xffffffffu;
            session_deadline=time_us_64()+300000000u;
        }
        idle_deadline=time_us_64()+10000000u;
        printf("\nPICOCSV %s BEGIN %" PRIu32 "\n",token,total);return;
    }
    if(!active || strcmp(token,id)){error(id,"CLOSED");return;}
    if(!strcmp(op,"CLOSE") && count==3) {
        usb_export_cancel();printf("\nPICOCSV %s CLOSED\n",id);return;
    }
    if(busy){error(id,"BUSY");usb_export_cancel();return;}
    if(!strcmp(op,"END") && count==3) {
        if(next_offset!=total){error(id,"INCOMPLETE");return;}
        idle_deadline=time_us_64()+10000000u;
        printf("\nPICOCSV %s END %" PRIu32 " %08" PRIx32 "\n",token,total,~crc);return;
    }
    if(!strcmp(op,"READ") && count==4) {
        uint32_t offset;
        if(!number(parts[3],&offset) || offset>=total || (offset!=next_offset && offset!=last_offset)) {
            error(id,"RANGE");return;
        }
        uint8_t data[256];uint32_t n=total-offset;if(n>sizeof(data))n=sizeof(data);
        if(storage_export_read(offset,data,n)!=(int)n){error(id,"STORAGE");usb_export_cancel();return;}
        if(offset==next_offset) {
            for(uint32_t i=0;i<n;i++)crc=crc32_update_byte(crc,data[i]);
            next_offset+=n;last_offset=offset;
        }
        char hex[513];static const char digits[]="0123456789abcdef";
        for(uint32_t i=0;i<n;i++){hex[i*2]=digits[data[i]>>4];hex[i*2+1]=digits[data[i]&15];}hex[n*2]=0;
        idle_deadline=time_us_64()+10000000u;
        printf("\nPICOCSV %s DATA %" PRIu32 " %s %08" PRIx32 "\n",token,offset,hex,image_crc32(data,n));
    }
}
void usb_export_poll(bool busy) {
    uint64_t now=time_us_64();
    if(!stdio_usb_connected()) {
        usb_export_cancel();line_size=0;overflow=false;return;
    }
    if(active && (now>=idle_deadline || now>=session_deadline)) {
        error(token,"TIMEOUT");usb_export_cancel();
    }
    if(line_size && now>=input_deadline){line_size=0;overflow=false;}
    /* Bound work per UI iteration, even when a host floods the serial port. */
    for(unsigned i=0;i<64;i++) {
        int c=getchar_timeout_us(0);if(c<0)break;
        input_deadline=now+2000000u;
        if(c=='\n') {
            if(!overflow){line[line_size]=0;command(line,busy);}
            line_size=0;overflow=false;
        } else if(c!='\r') {
            if(c<32 || c>126 || line_size+1>=sizeof(line))overflow=true;
            else if(!overflow)line[line_size++]=(char)c;
        }
    }
}
#else
bool usb_export_active(void) { return false; }
void usb_export_cancel(void) { }
void usb_export_poll(bool busy) { (void)busy; }
#endif
