/* Runs the ACTUAL storage.c + LittleFS against a NOR-flash model.
 * OpenSSL substitutes for SDK crypto only, not for storage/OTA/logging code. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/services/storage.c"

uint8_t fake_flash[0x400000],firmware_image[512*1024];
size_t firmware_image_size=sizeof(firmware_image);
uint32_t firmware_image_crc32;
static uint8_t image[512*1024],snapshot[0x400000];
static int fail_after=-1,operations;
static unsigned fail_prefix;
void pico_get_unique_board_id_string(char *out,size_t n){snprintf(out,n,"0011223344556677");}
uint32_t get_rand_32(void){static uint32_t x=0x12345678;x^=x<<13;x^=x>>17;x^=x<<5;return x;}
uint64_t get_rand_64(void){return ((uint64_t)get_rand_32()<<32)|get_rand_32();}
void flash_range_erase(uint32_t off,size_t n){assert(off>=APP_FLASH_LIMIT && !(off%4096) && !(n%4096) && off+n<=sizeof(fake_flash));memset(fake_flash+off,0xff,n);}
void flash_range_program(uint32_t off,const uint8_t *data,size_t n){assert(off>=APP_FLASH_LIMIT && !(off%256) && !(n%256) && off+n<=sizeof(fake_flash));for(size_t i=0;i<n;i++){assert((fake_flash[off+i]&data[i])==data[i]);fake_flash[off+i]&=data[i];}}
int flash_safe_execute(void (*fn)(void *),void *arg,uint32_t timeout){
    (void)timeout;operations++;
    if(fail_after==0){flash_operation_t *op=arg;unsigned n=fail_prefix;if(n>op->size)n=op->size;
        if(op->erase)memset(fake_flash+op->offset,0xff,n);
        else for(unsigned i=0;i<n;i++)fake_flash[op->offset+i]&=op->data[i];
        return -1;
    }
    if(fail_after>0)fail_after--;
    fn(arg);return 0;
}
static void reset_ram(void){
    if(mounted)lfs_unmount(&fs);
    memset(&fs,0,sizeof(fs));mounted=job_open=ota_open=export_open=false;active_slot=0;
    memset(&job,0,sizeof(job));fail_after=-1;fail_prefix=0;
}
static void blank_boot(void){reset_ram();memset(fake_flash,0xff,sizeof(fake_flash));assert(storage_init());}
static void reboot(void){reset_ram();assert(storage_init());}
static bool install(uint32_t revision,const uint8_t *data,size_t n){
    uint8_t hash[32];assert(sha256(data,n,hash));
    if(!storage_ota_begin(revision,(uint32_t)n,hash))return false;
    for(size_t off=0;off<n;){size_t part=n-off;if(part>997)part=997;if(!storage_ota_write(data+off,part))return false;off+=part;}
    return storage_ota_commit();
}
static void logs(void){
    blank_boot();programmer_report_t r={.result=RESULT_OK,.stage=STAGE_RESTART,.uid={1,2,3},.uid_valid=true,.elapsed_ms=15588};
    assert(storage_job_begin(1234,false,524288));assert(storage_job_finish(&r));
    uint32_t size;char id[65],other[65];assert(storage_batch_prepare(&size,id));
    char csv[1024];int n=storage_batch_read(0,csv,sizeof(csv)-1);assert(n>0);csv[n]=0;
    assert(strstr(csv,"000000010000000200000003,UPTIME_MS,1234,15588,SUCCESS,524288"));
    reboot();assert(storage_batch_prepare(&size,other));assert(!strcmp(id,other));
    assert(!storage_batch_ack("wrong"));assert(storage_batch_ack(id));assert(!storage_batch_prepare(&size,id));
    /* A power loss after durable marker but before result becomes INTERRUPTED. */
    assert(storage_job_begin(99,false,32));reboot();assert(storage_batch_prepare(&size,id));
    n=storage_batch_read(0,csv,sizeof(csv)-1);csv[n]=0;assert(strstr(csv,"INTERRUPTED"));
    assert(storage_batch_ack(id));
    /* Completed append + power cut before marker deletion does not duplicate. */
    assert(storage_job_begin(101,false,32));assert(append_result(&r,"SUCCESS"));reboot();
    assert(storage_batch_prepare(&size,id));n=storage_batch_read(0,csv,sizeof(csv)-1);csv[n]=0;
    assert(!strstr(csv,"INTERRUPTED"));assert(storage_batch_ack(id));
    /* Tear every physical mutation in completion/marker cleanup. On reboot
     * exactly one outcome survives, never a silently missing job. */
    assert(storage_job_begin(555,false,524288));
    memcpy(snapshot,fake_flash,sizeof(snapshot));job_record_t saved_job=job;
    operations=0;assert(storage_job_finish(&r));int finish_ops=operations;
    for(int fail=0;fail<finish_ops;fail++) {
        reset_ram();memcpy(fake_flash,snapshot,sizeof(snapshot));
        assert(lfs_mount(&fs,&cfg)==0);mounted=true;job=saved_job;job_open=true;
        fail_after=fail;fail_prefix=16;(void)storage_job_finish(&r);reboot();
        assert(storage_batch_prepare(&size,id));n=storage_batch_read(0,csv,sizeof(csv)-1);assert(n>0);csv[n]=0;
        unsigned rows=0;for(int j=0;j<n;j++)if(csv[j]=='\n')rows++;
        assert(rows==2 && (strstr(csv,"SUCCESS") || strstr(csv,"INTERRUPTED")));
    }
    assert(storage_batch_ack(id));
    /* Saturation preserves pending data and refuses the next target erase. */
    assert(create_log());
    char block[1024];memset(block,'x',sizeof(block));
    for(unsigned i=0;i<256;i++)assert(file_write(ACTIVE_LOG,block,sizeof(block),LFS_O_WRONLY|LFS_O_APPEND));
    assert(!storage_job_begin(1,false,32));
    puts("Logging: UID, duration, offline timestamp, retained/acknowledged batches, reboot recovery, capacity guard passed");
}
static void exports(void){
    blank_boot();uint32_t size;char id[65];
    int before=operations;
    assert(storage_export_begin(&size) && size==strlen(csv_header));
    char small[512];int n=storage_export_read(0,small,sizeof(small));
    assert(n==(int)strlen(csv_header) && !memcmp(small,csv_header,(size_t)n));
    assert(storage_export_read(size,small,sizeof(small))==0);
    assert(storage_export_read(size+1,small,sizeof(small))==-1);
    assert(!storage_job_begin(1,false,524288));
    assert(!storage_batch_prepare(&size,id));
    wifi_credentials_t c={.ssid="test",.password="12345678"};
    assert(!storage_credentials_save(&c));
    assert(operations==before);storage_export_end();
    programmer_report_t r={.result=RESULT_OK,.uid={1,2,3},.uid_valid=true};
    assert(storage_job_begin(100,false,524288));assert(!storage_export_begin(&size));assert(storage_job_finish(&r));
    assert(storage_batch_prepare(&size,id));
    for(unsigned i=0;i<70;i++){assert(storage_job_begin(200+i,false,524288));assert(storage_job_finish(&r));}
    int pending=file_size(BATCH_LOG),current=file_size(ACTIVE_LOG);
    memcpy(snapshot,fake_flash,sizeof(snapshot));before=operations;
    assert(storage_export_begin(&size));
    assert(size==(unsigned)(pending+current)-(unsigned)strlen(csv_header));
    /* Odd-sized reads cross both header and file boundaries. */
    for(uint32_t off=0;off<size;){n=storage_export_read(off,image+off,17);assert(n>0 && n<=17);off+=(uint32_t)n;}
    assert(storage_export_read(size,small,sizeof(small))==0);image[size]=0;
    unsigned rows=0;for(uint32_t i=0;i<size;i++)if(image[i]=='\n')rows++;
    assert(rows==72 && !strstr((char *)image+strlen(csv_header),"job_id,pico_id"));
    assert(strstr((char *)image,",100,")<strstr((char *)image,",200,"));
    storage_export_end();
    assert(operations==before && !memcmp(snapshot,fake_flash,sizeof(snapshot)));
    assert(storage_export_read(0,small,sizeof(small))==-1);
    assert(storage_export_begin(&size));storage_export_end();
    assert(storage_job_begin(999,false,524288));assert(storage_job_finish(&r));
    assert(storage_batch_ack(id)); /* export did not acknowledge the upload */
    puts("CSV export: empty/header-only, pending+current merge, streamed boundaries, read-only flash, locking and cancellation passed");
}
int main(void){
    memset(firmware_image,0xff,sizeof(firmware_image));
    const uint32_t vectors[]={0x20000460,0x08000009};memcpy(firmware_image,vectors,8);
    firmware_image_crc32=image_crc32(firmware_image,sizeof(firmware_image));
    memcpy(image,firmware_image,sizeof(image));for(unsigned i=8;i<sizeof(image);i++)image[i]=(uint8_t)(i*37);
    blank_boot();assert(storage_image().data==firmware_image);
    assert(install(40003,image,sizeof(image)));assert(storage_image().size==sizeof(image));
    assert(!memcmp(storage_image().data,image,sizeof(image)));reboot();assert(storage_image().revision==40003);
    uint8_t hash[32];assert(sha256(image,sizeof(image),hash));
    assert(!storage_ota_begin(40003,sizeof(image),hash));
    assert(storage_ota_begin(40004,sizeof(image),hash));assert(storage_ota_write(image,2000));assert(!storage_ota_commit());storage_ota_abort();
    reboot();assert(storage_image().revision==40003);
    assert(storage_ota_begin(40004,sizeof(image),hash));image[1024]^=1;
    assert(storage_ota_write(image,sizeof(image)));assert(!storage_ota_commit());storage_ota_abort();image[1024]^=1;
    reboot();assert(storage_image().revision==40003);
    /* Power failure at every erase/program operation of a 4104-byte update,
     * including page/sector boundaries and every header commit prefix. */
    memcpy(snapshot,fake_flash,sizeof(snapshot));operations=0;assert(install(40004,image,4104));int count=operations;
    for(int fail=0;fail<count;fail++)for(unsigned prefix=0;prefix<3;prefix++){
        reset_ram();memcpy(fake_flash,snapshot,sizeof(snapshot));assert(storage_init());fail_after=fail;fail_prefix=prefix==0?0:prefix==1?16:64;
        (void)install(40004,image,4104);reboot();
        assert(storage_image().revision==40003 || storage_image().revision==40004);
        if(storage_image().revision==40004)assert(!memcmp(storage_image().data,image,4104));
        else assert(!memcmp(storage_image().data,image,sizeof(image)));
    }
    puts("OTA: full 512 KiB, fragmented download, digest rejection, rollback prevention, 8-byte tail and torn-write recovery passed");
    blank_boot();wifi_credentials_t c={.ssid="Factory WiFi",.password="private-password"},read;
    assert(storage_credentials_save(&c));assert(storage_credentials_read(&read));assert(!memcmp(&c,&read,sizeof(c)));
    fake_flash[CREDENTIAL_BASE+20]^=1;assert(!storage_credentials_read(&read));
    assert(storage_credentials_save(&c));reboot();assert(storage_credentials_read(&read));
    puts("Credentials: AES-GCM save/load, ciphertext tamper rejection and reboot passed");
    logs();
    exports();
    reset_ram();memset(fake_flash,0xff,sizeof(fake_flash));fake_flash[LOG_FS_BASE]=0;assert(!storage_init());assert(fake_flash[LOG_FS_BASE]==0);
    puts("Corrupt existing filesystem is preserved and never automatically formatted");return 0;
}
