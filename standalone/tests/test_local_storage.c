/* Actual local storage + LittleFS, NOR model with torn physical writes. */
#include <assert.h>
#include "../src/services/local_storage.c"
uint8_t fake_flash[0x400000],firmware_image[512*1024];
size_t firmware_image_size=sizeof(firmware_image);
uint32_t firmware_image_crc32;
static uint8_t backup[sizeof(fake_flash)];
static int fail_after=-1,operations;
uint64_t time_us_64(void){return 0;}
void pico_get_unique_board_id_string(char *out,size_t n){snprintf(out,n,"0011223344556677");}
uint32_t get_rand_32(void){static uint32_t n=8;return ++n;}
uint64_t get_rand_64(void){return get_rand_32();}
void flash_range_erase(uint32_t off,size_t n){assert(off>=LOG_FS_BASE && !(off%4096) && !(n%4096) && off+n<=CREDENTIAL_BASE);memset(fake_flash+off,0xff,n);}
void flash_range_program(uint32_t off,const uint8_t *data,size_t n){assert(off>=LOG_FS_BASE && !(off%256) && !(n%256) && off+n<=CREDENTIAL_BASE);for(size_t i=0;i<n;i++){assert((fake_flash[off+i]&data[i])==data[i]);fake_flash[off+i]&=data[i];}}
int flash_safe_execute(void (*fn)(void *),void *arg,uint32_t timeout){
    (void)timeout;operations++;
    if(fail_after==0){flash_op_t *op=arg;unsigned n=16;if(n>op->size)n=op->size;
        if(!op->data)memset(fake_flash+op->offset,0xff,n);
        else for(unsigned i=0;i<n;i++)fake_flash[op->offset+i]&=((const uint8_t *)op->data)[i];
        return -1;
    }
    if(fail_after>0)fail_after--;
    fn(arg);return 0;
}
static void reset_ram(void){
    if(mounted)lfs_unmount(&fs);
    memset(&fs,0,sizeof(fs));mounted=job_open=snapshot_open=false;
    memset(&job,0,sizeof(job));fail_after=-1;
}
static void boot(void){reset_ram();assert(storage_init());}
static void blank(void){reset_ram();memset(fake_flash,0xff,sizeof(fake_flash));assert(storage_init());}
static void csv(char out[4096],char etag[65]){
    uint32_t n;assert(local_snapshot_begin(0,&n,etag) && n<4096);
    for(uint32_t off=0;off<n;){int r=local_snapshot_read(off,out+off,17);assert(r>0 && r<=17);off+=(uint32_t)r;}
    out[n]=0;assert(local_snapshot_read(n,out,1)==0);local_snapshot_end();
}
int main(void){
    blank();assert(storage_image().data==firmware_image);
    assert(!local_logs_full());
    programmer_report_t r={.result=RESULT_OK,.uid={1,2,3},.uid_valid=true,.elapsed_ms=15588};
    /* More than five records are allowed; restart retains both rows and capacity. */
    assert(LOG_LIMIT==1024u*1024u);
    for(unsigned i=0;i<10;i++) {
        assert(storage_job_begin(i%2?0:1790640000,i%2==0,524288));
        r.result=i%2?RESULT_VERIFY:RESULT_OK;
        assert(storage_job_finish(&r));
        assert(!local_logs_full());
    }
    char retained[4096],retained_tag[65];csv(retained,retained_tag);
    unsigned retained_rows=0;
    for(char *p=retained;*p;p++)if(*p=='\n')retained_rows++;
    assert(retained_rows==11);
    boot();assert(!local_logs_full());
    assert(storage_job_begin(0,false,524288));boot();
    csv(retained,retained_tag);
    assert(strstr(retained,"UNAVAILABLE,UNSET,0.000"));
    assert(!local_logs_full());
    assert(local_logs_clear(retained_tag)==0);
    csv(retained,retained_tag);assert(!strcmp(retained,header));
    blank();assert(lfs_remove(&fs,LOG_NAME)==0);
    assert(!storage_job_begin(0,false,524288));
    assert(!local_logs_full()); /* Missing file is a storage fault, not full. */
    blank();r.result=RESULT_OK;
    assert(storage_job_begin(1790640000,true,524288));assert(storage_job_finish(&r));
    char text[4096],tag[65],old[65];csv(text,tag);
    assert(strstr(text,"Target UID,Date&Time(IST),Elapsed Time(sec),") && strstr(text,"checksum_error,swd_error,"));
    assert(strstr(text,"000000010000000200000003,2026-09-29 05:30:00,15.588,v4.0.2,524288,SUCCESS,00000000,PASS,PASS,PASS,PASS,PASS,PASS,PASS,PASS,PASS,PASS,PASS\n"));
    strcpy(old,tag);assert(storage_job_begin(0,false,524288));r.result=RESULT_VERIFY;assert(storage_job_finish(&r));
    csv(text,tag);assert(strstr(text,",UNSET,15.588,v4.0.2,524288,FAIL,00000000,PASS,NOT_RUN,NOT_RUN,NOT_RUN,NOT_RUN,NOT_RUN,NOT_RUN,ERROR,NOT_RUN,NOT_RUN,NOT_RUN\n"));
    assert(storage_job_begin(0,false,524288));r.result=RESULT_IMAGE;assert(storage_job_finish(&r));
    csv(text,tag);assert(strstr(text,",FAIL,00000000,ERROR,NOT_RUN,NOT_RUN,NOT_RUN,NOT_RUN,NOT_RUN,NOT_RUN,NOT_RUN,NOT_RUN,NOT_RUN,NOT_RUN\n"));
    assert(local_logs_clear(old)==1);uint32_t n;assert(local_snapshot_begin(0,&n,tag));
    int before=operations;assert(!storage_job_begin(0,false,524288));assert(local_logs_clear(tag)==-1);assert(operations==before);local_snapshot_end();
    /* Tear each physical mutation of atomic CSV replacement: old or header. */
    memcpy(backup,fake_flash,sizeof(backup));operations=0;assert(!local_logs_clear(tag));int count=operations;
    for(int fail=0;fail<count;fail++){
        reset_ram();memcpy(fake_flash,backup,sizeof(backup));assert(storage_init());fail_after=fail;(void)local_logs_clear(tag);boot();
        char current[4096],etag[65];csv(current,etag);assert(!strcmp(current,text) || !strcmp(current,header));
    }
    blank();assert(storage_job_begin(0,false,524288));boot();csv(text,tag);
    assert(strstr(text,"UNAVAILABLE,UNSET,0.000,v4.0.2,524288,FAIL,00000000,PASS,NOT_RUN,NOT_RUN,NOT_RUN,ERROR,NOT_RUN,NOT_RUN,NOT_RUN,NOT_RUN,NOT_RUN,NOT_RUN\n"));
    blank();assert(storage_job_begin(1790640000,true,524288));local_job_t saved=job;
    memcpy(backup,fake_flash,sizeof(backup));operations=0;r.result=RESULT_OK;assert(storage_job_finish(&r));count=operations;
    for(int fail=0;fail<count;fail++){
        reset_ram();memcpy(fake_flash,backup,sizeof(backup));assert(!lfs_mount(&fs,&config));mounted=true;job=saved;job_open=true;
        fail_after=fail;(void)storage_job_finish(&r);boot();csv(text,tag);
        unsigned rows=0;for(char *p=text;*p;p++)if(*p=='\n')rows++;
        assert(rows==2 && (strstr(text,",SUCCESS,") || strstr(text,",FAIL,")));
    }
    /* Old schema and pending uploads survive migration with readable archives. */
    blank();const char legacy[]="job_id,pico_id\n1,ABC\n";
    assert(write_file(LOG_NAME,legacy,sizeof(legacy)-1));assert(write_file("upload.csv",legacy,sizeof(legacy)-1));boot();
    csv(text,tag);assert(!strcmp(text,header));
    for(unsigned i=1;i<=2;i++){
        assert(local_snapshot_begin(i,&n,tag));assert(n==sizeof(legacy)-1);
        assert(local_snapshot_read(0,text,sizeof(text))==(int)n && !memcmp(text,legacy,n));local_snapshot_end();
    }
    /* A later schema update keeps the first archive and uses the v2 archive. */
    assert(write_file(LOG_NAME,legacy,sizeof(legacy)-1));boot();
    assert(local_snapshot_begin(4,&n,tag));assert(n==sizeof(legacy)-1);
    assert(local_snapshot_read(0,text,sizeof(text))==(int)n && !memcmp(text,legacy,n));local_snapshot_end();
    /* UTC-to-IST migration coexists with earlier archives and is restart-safe.
     * A completed UTC row's durable ID suppresses a duplicate recovery row. */
    assert(storage_job_begin(1790640000,true,524288));assert(append_result(&r));
    lfs_file_t utc_file;assert(!lfs_file_open(&fs,&utc_file,LOG_NAME,LFS_O_WRONLY));
    assert(lfs_file_write(&fs,&utc_file,utc_header,sizeof(utc_header)-1)==sizeof(utc_header)-1);
    assert(!lfs_file_close(&fs,&utc_file));boot();csv(text,tag);assert(!strcmp(text,header));
    assert(local_snapshot_begin(3,&n,tag));
    assert(local_snapshot_read(0,text,sizeof(text)-1)==(int)n);text[n]=0;
    assert(strstr(text,"date_time_utc") && strstr(text,"SUCCESS"));local_snapshot_end();
    boot();assert(storage_job_begin(1790640000,true,524288));assert(storage_job_finish(&r));
    csv(text,tag);assert(strstr(text,"2026-09-29 05:30:00"));
    /* Exercise the actual 1 MiB boundary, not the old 256 KiB or five-row cap. */
    blank();
    lfs_file_t f;assert(!lfs_file_open(&fs,&f,LOG_NAME,LFS_O_WRONLY|LFS_O_APPEND));
    char block[1024];memset(block,'x',sizeof(block));
    uint32_t remaining=LOG_LIMIT-512u-(sizeof(header)-1u);
    while(remaining) {
        uint32_t bytes=remaining>sizeof(block)?sizeof(block):remaining;
        assert(lfs_file_write(&fs,&f,block,bytes)==(int)bytes);
        remaining-=bytes;
    }
    assert(!lfs_file_close(&fs,&f));
    assert(refresh_log_capacity()==0 && !local_logs_full());
    assert(storage_job_begin(0,false,524288));assert(storage_job_finish(&r));
    assert(local_logs_full());
    int limit_operations=operations;
    assert(!storage_job_begin(0,false,524288) && !job_open);
    assert(operations==limit_operations);
    boot();assert(local_logs_full());assert(!storage_job_begin(0,false,524288));
    assert(local_snapshot_begin(0,&n,tag));
    assert(n>LOG_LIMIT-512u && n<=LOG_LIMIT);
    for(uint32_t off=0;off<n;) {
        int got=local_snapshot_read(off,block,sizeof(block));
        assert(got>0 && got<=(int)sizeof(block));off+=(uint32_t)got;
    }
    local_snapshot_end();
    assert(local_logs_clear(tag)==0 && !local_logs_full());
    csv(text,tag);assert(!strcmp(text,header));
    assert(storage_job_begin(0,false,524288));assert(storage_job_finish(&r));
    /* Corrupt FS must never be silently formatted. */
    reset_ram();memset(fake_flash,0xff,sizeof(fake_flash));fake_flash[LOG_FS_BASE]=0;assert(!storage_init());assert(fake_flash[LOG_FS_BASE]==0);
    puts("Local CSV: 1 MiB limit, checksum and eleven error columns, restart/clear recovery, UTC/UNSET, migration, ETag locking, full guard and torn-write append/clear recovery passed");return 0;
}
