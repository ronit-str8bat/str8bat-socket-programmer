/* Local-only log storage. Same reserved LittleFS partition as network-v1/v2;
 * old CSVs are archived, never silently discarded by a schema change.
 * All mutations execute on core 0 between SWD jobs, with core-1 flash lockout.
 */
#include "services/storage.h"
#include "services/local_api.h"
#include "firmware_image.h"
#include "firmware_version.h"
#include "programmer_config.h"
#include "wifi_config.h"
#include "crc32.h"
#include "lfs.h"
#include "pico/flash.h"
#include "pico/rand.h"
#include "pico/unique_id.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stddef.h>

#define LOG_NAME "programming_logs.csv"
#define JOB_NAME "local-inflight.job"
#define CLEAR_NAME "logs-clear.tmp"
#define LOG_LIMIT (1024u * 1024u) /* Maximum CSV size: 1 MiB, including header. */
#define JOB_MAGIC 0x4c4f4732u
#define FACTORY_BIN_MAGIC 0x4642494eu /* FBIN */
#define FACTORY_PAIR_MAGIC 0x46504149u /* FPAI */
/* A successful job reports PASS in every error column. A failed job reports
 * ERROR only for the failure that stopped it; checks that did not run are
 * explicitly NOT_RUN. The checksum is the CRC-32 of the embedded STM32
 * image, displayed as eight hexadecimal digits. */
static const char header[]="Target UID,Date&Time(IST),Elapsed Time(sec),firmware_version,Binary Size(Bytes),status,Image CRC32,pico_ssid,bin_received_date_time,checksum_error,swd_error,target_error,protection_error,reset_error,timeout_error,flash_error,verify_error,clock_error,log_storage_error,time_not_set_error\n";
static const char utc_header[]="Target UID,date_time_utc,Elapsed Time(sec),firmware_version,Binary Size(Bytes),status,Image CRC32,pico_ssid,bin_received_date_time,checksum_error,swd_error,target_error,protection_error,reset_error,timeout_error,flash_error,verify_error,clock_error,log_storage_error,time_not_set_error\n";
static const char *const export_names[]={LOG_NAME,"programming_logs.v1.csv","upload.v1.csv","programming_logs.utc.csv","programming_logs.v2.csv"};
typedef struct { uint32_t magic;uint32_t size;uint64_t id;uint32_t timestamp;char version[32];uint32_t crc; } local_job_t;
typedef struct {
    uint32_t magic, generation, size, crc, received_epoch;
    char label[32];
    uint32_t header_crc;
} factory_bin_header_t;
typedef struct {
    uint32_t magic;
    char ids[3][17];
    uint32_t crc;
} factory_pair_record_t;
static lfs_t fs;
static bool mounted,job_open,snapshot_open;
static bool logs_full;
static const char *storage_state="STORAGE_NOT_STARTED";
static int refresh_log_capacity(void);
static local_job_t job;
static unsigned snapshot_file;
static uint32_t snapshot_size;
static char device_id[2*PICO_UNIQUE_BOARD_ID_SIZE_BYTES+1];
static factory_bin_header_t factory_bin;
static factory_pair_record_t factory_pairs;
static bool factory_upload_open;
static const char *factory_upload_error="No active factory upload.";
static uint32_t factory_expected_crc,factory_expected_size,factory_written,factory_received_epoch,factory_upload_generation;
static char factory_upload_label[32];
static uint8_t factory_page[256];
static uint32_t factory_page_used;
typedef struct { uint32_t offset,size;const void *data; } flash_op_t;
static const uint8_t *xip(uint32_t offset) { return (const uint8_t *)(XIP_BASE+offset); }
static void flash_op(void *arg) {
    flash_op_t *op=arg;
    if(op->data)flash_range_program(op->offset,op->data,op->size);
    else flash_range_erase(op->offset,op->size);
}
static bool mutate(uint32_t off,uint32_t n,const void *data) {
    uint32_t alignment=data?256u:4096u;
    bool log_range=off>=LOG_FS_BASE && off<=CREDENTIAL_BASE && n<=CREDENTIAL_BASE-off;
    bool bin_range=off>=FACTORY_BIN_HEADER_BASE && off<=FACTORY_BIN_HEADER_BASE+FACTORY_BIN_ERASE_BYTES &&
                   n<=FACTORY_BIN_HEADER_BASE+FACTORY_BIN_ERASE_BYTES-off;
    bool pair_range=off>=FACTORY_PAIR_BASE && off<=LOG_FS_BASE && n<=LOG_FS_BASE-off;
    if(snapshot_open || (!log_range && !bin_range && !pair_range) || off%alignment || n%alignment)return false;
    flash_op_t op={off,n,data};
    return flash_safe_execute(flash_op,&op,1000)==PICO_OK && (!data || !memcmp(xip(off),data,n));
}
static bool factory_bin_valid(factory_bin_header_t *header) {
    memcpy(header,xip(FACTORY_BIN_HEADER_BASE),sizeof(*header));
    if(header->magic!=FACTORY_BIN_MAGIC || header->size<8u || header->size>FACTORY_BIN_MAX_SIZE || (header->size&7u) ||
       header->header_crc!=image_crc32((const uint8_t *)header,offsetof(factory_bin_header_t,header_crc))) return false;
    return image_crc32(xip(FACTORY_BIN_DATA_BASE),header->size)==header->crc;
}
static bool valid_id(const char *id) {
    if(!id || strlen(id)!=16)return false;
    for(unsigned i=0;i<16;i++)if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='A'&&id[i]<='F')||(id[i]>='a'&&id[i]<='f')))return false;
    return true;
}
static bool valid_bin_label(const char *label) {
    if(!label)return false;
    size_t n=strlen(label);if(!n || n>=sizeof(factory_upload_label))return false;
    for(size_t i=0;i<n;i++)if(!((label[i]>='0'&&label[i]<='9') || (label[i]>='A'&&label[i]<='Z') ||
        (label[i]>='a'&&label[i]<='z') || label[i]=='.' || label[i]=='_' || label[i]=='-'))return false;
    return true;
}
static bool factory_pair_load(void) {
    memcpy(&factory_pairs,xip(FACTORY_PAIR_BASE),sizeof(factory_pairs));
    if(factory_pairs.magic!=FACTORY_PAIR_MAGIC ||
       factory_pairs.crc!=image_crc32((const uint8_t *)&factory_pairs,offsetof(factory_pair_record_t,crc))) {
        memset(&factory_pairs,0,sizeof(factory_pairs));return true;
    }
    for(unsigned i=0;i<3;i++)if(!valid_id(factory_pairs.ids[i])){memset(&factory_pairs,0,sizeof(factory_pairs));return false;}
    return true;
}
/* The production Master always gets its authorized slaves from the source
 * configuration, never from a web request or an old pairing-flash record. */
#if FACTORY_ROLE == FACTORY_ROLE_MASTER
static bool factory_pair_load_fixed(void) {
    static const char *const configured_ids[3]={
        FACTORY_SLAVE_ID_1, FACTORY_SLAVE_ID_2, FACTORY_SLAVE_ID_3
    };
    memset(&factory_pairs,0,sizeof(factory_pairs));
    factory_pairs.magic=FACTORY_PAIR_MAGIC;
    for(unsigned i=0;i<3;i++) {
        const char *id=configured_ids[i];
        if(!id[0])continue;
        if(!valid_id(id))return false;
        for(unsigned j=0;j<i;j++)if(!strcmp(id,factory_pairs.ids[j]))return false;
        memcpy(factory_pairs.ids[i],id,17);
    }
    factory_pairs.crc=image_crc32((const uint8_t *)&factory_pairs,offsetof(factory_pair_record_t,crc));
    return true;
}
#endif
static int read_block(const struct lfs_config *c,lfs_block_t b,lfs_off_t off,void *data,lfs_size_t n) {
    (void)c;if(b>=LOG_FS_SIZE/4096 || off>4096 || n>4096-off)return LFS_ERR_IO;
    memcpy(data,xip(LOG_FS_BASE+b*4096+off),n);return 0;
}
static int write_block(const struct lfs_config *c,lfs_block_t b,lfs_off_t off,const void *data,lfs_size_t n) {
    (void)c;if(b>=LOG_FS_SIZE/4096 || off>4096 || n>4096-off)return LFS_ERR_IO;
    return mutate(LOG_FS_BASE+b*4096+off,n,data)?0:LFS_ERR_IO;
}
static int erase_block(const struct lfs_config *c,lfs_block_t b) {
    (void)c;if(b>=LOG_FS_SIZE/4096)return LFS_ERR_IO;
    return mutate(LOG_FS_BASE+b*4096,4096,NULL)?0:LFS_ERR_IO;
}
static int sync_blocks(const struct lfs_config *c) { (void)c;return 0; }
static const struct lfs_config config={.read=read_block,.prog=write_block,.erase=erase_block,.sync=sync_blocks,
    .read_size=16,.prog_size=256,.block_size=4096,.block_count=LOG_FS_SIZE/4096,.block_cycles=500,.cache_size=256,.lookahead_size=64};
static int file_size(const char *name) {
    struct lfs_info info;int r=lfs_stat(&fs,name,&info);
    return r==LFS_ERR_NOENT?0:r<0?-1:(int)info.size;
}
static int read_file(const char *name,uint32_t offset,void *data,size_t n) {
    lfs_file_t f;if(lfs_file_open(&fs,&f,name,LFS_O_RDONLY)<0)return -1;
    int r=-1;if(lfs_file_seek(&fs,&f,offset,LFS_SEEK_SET)==(lfs_soff_t)offset)r=lfs_file_read(&fs,&f,data,n);
    if(lfs_file_close(&fs,&f)<0)r=-1;
    return r;
}
static bool write_file(const char *name,const void *data,size_t n) {
    lfs_file_t f;if(lfs_file_open(&fs,&f,name,LFS_O_WRONLY|LFS_O_CREAT|LFS_O_TRUNC)<0)return false;
    bool ok=lfs_file_write(&fs,&f,data,n)==(lfs_ssize_t)n;
    if(lfs_file_close(&fs,&f)<0)ok=false;
    return ok;
}
static bool archive(const char *from,const char *to) {
    int n=file_size(from);if(n<0)return false;if(!n)return true;
    /* Do not overwrite an existing archive from an earlier migration. */
    struct lfs_info info;
    if(lfs_stat(&fs,to,&info)!=LFS_ERR_NOENT)return false;
    return lfs_rename(&fs,from,to)==0;
}
static bool archive_first_free(const char *from,const char *const *names,unsigned count) {
    int n=file_size(from);if(n<0)return false;if(!n)return true;
    struct lfs_info info;
    for(unsigned i=0;i<count;i++)if(lfs_stat(&fs,names[i],&info)==LFS_ERR_NOENT)return archive(from,names[i]);
    return false;
}
/* New CSV layouts must not overwrite an older downloadable archive. */
static const char *next_log_archive(void) {
    static const char *const names[]={"programming_logs.v1.csv","programming_logs.v2.csv",
        "programming_logs.pre_factory.csv","programming_logs.v3.csv"};
    struct lfs_info info;
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++)
        if(lfs_stat(&fs,names[i],&info)==LFS_ERR_NOENT)return names[i];
    return NULL;
}
static const char *error_state(unsigned actual, unsigned expected) {
    if(actual==RESULT_OK)return "PASS";
    if(actual==expected)return "ERROR";
    /* The XIP image CRC is checked before every SWD session. */
    if(expected==RESULT_IMAGE && actual!=RESULT_IMAGE)return "PASS";
    return "NOT_RUN";
}
static bool append_result(const programmer_report_t *r) {
    uint64_t last=0;int attr=lfs_getattr(&fs,LOG_NAME,1,&last,sizeof(last));
    if(attr==(int)sizeof(last) && last==job.id)return true;
    if(attr<0 && attr!=LFS_ERR_NOATTR)return false;
    char uid[25]="UNAVAILABLE",date[20]="UNSET",received[20]="UNAVAILABLE",ssid[33],row[448];
    if(r->uid_valid)snprintf(uid,sizeof(uid),"%08" PRIx32 "%08" PRIx32 "%08" PRIx32,r->uid[0],r->uid[1],r->uid[2]);
    if(job.timestamp && !local_format_ist(job.timestamp,date))return false;
    if(factory_bin.magic==FACTORY_BIN_MAGIC && factory_bin.received_epoch &&
       !local_format_ist(factory_bin.received_epoch,received))return false;
    local_device_ssid(ssid);
    unsigned error=(unsigned)r->result;
    int n=snprintf(row,sizeof(row),
        "%s,%s,%" PRIu32 ".%03" PRIu32 ",%s,%" PRIu32 ",%s,%08" PRIx32 ",%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
        uid,date,r->elapsed_ms/1000u,r->elapsed_ms%1000u,job.version,job.size,
        r->result==RESULT_OK?"SUCCESS":"FAIL",factory_bin.magic==FACTORY_BIN_MAGIC?factory_bin.crc:firmware_image_crc32,ssid,received,
        error_state(error,RESULT_IMAGE),error_state(error,RESULT_SWD),
        error_state(error,RESULT_TARGET),error_state(error,RESULT_PROTECTED),
        error_state(error,RESULT_RESET),error_state(error,RESULT_TIMEOUT),
        error_state(error,RESULT_FLASH),error_state(error,RESULT_VERIFY),
        error_state(error,RESULT_CLOCK),error_state(error,10u),error_state(error,11u));
    if(n<0 || n>=(int)sizeof(row))return false;
    /* Commit the deduplication ID atomically with the row, without adding a
     * hidden/nonrequested column to the CSV. Recoverable across power loss. */
    struct lfs_attr id_attr={.type=1,.buffer=&job.id,.size=sizeof(job.id)};
    struct lfs_file_config fc={.attrs=&id_attr,.attr_count=1};
    lfs_file_t f;if(lfs_file_opencfg(&fs,&f,LOG_NAME,LFS_O_WRONLY|LFS_O_APPEND,&fc)<0)return false;
    bool ok=lfs_file_write(&fs,&f,row,(lfs_size_t)n)==n;
    if(lfs_file_close(&fs,&f)<0)ok=false;
    return ok;
}
stored_image_t storage_image(void) { return (stored_image_t){firmware_image,firmware_image_size,firmware_image_crc32,0}; }
const char *storage_device_id(void) { return device_id; }
const char *storage_init_status(void) { return storage_state; }
bool storage_init(void) {
    storage_state="STORAGE_INITIALIZING";
    logs_full=false;
    pico_get_unique_board_id_string(device_id,sizeof(device_id));
    if(!factory_pair_load())memset(&factory_pairs,0,sizeof(factory_pairs));
#if FACTORY_ROLE == FACTORY_ROLE_MASTER
    if(!factory_pair_load_fixed()){storage_state="FACTORY_SLAVE_ID_CONFIGURATION_INVALID";return false;}
#endif
    if(!factory_bin_valid(&factory_bin))memset(&factory_bin,0,sizeof(factory_bin));
    if(lfs_mount(&fs,&config)<0) {
        for(uint32_t i=0;i<LOG_FS_SIZE;i++)if(xip(LOG_FS_BASE)[i]!=0xff){storage_state="STORAGE_LOG_MOUNT_ERROR";return false;}
        if(lfs_format(&fs,&config)<0 || lfs_mount(&fs,&config)<0){storage_state="STORAGE_FORMAT_ERROR";return false;}
    }
    mounted=true;
    int n=file_size(LOG_NAME);if(n<0){storage_state="STORAGE_LOG_READ_ERROR";return false;}
    if(n) {
        char existing[sizeof(header)-1];
        int got=read_file(LOG_NAME,0,existing,sizeof(existing));
        if(got!=(int)sizeof(existing) || memcmp(existing,header,sizeof(existing))) {
            /* Keep old UTC rows explicitly labelled UTC, separate from new
             * IST rows. Existing older-schema archives are not overwritten. */
            const char *archive_name=got==(int)sizeof(existing) && !memcmp(existing,utc_header,sizeof(existing))
                ? export_names[3] : next_log_archive();
            if(!archive_name || !archive(LOG_NAME,archive_name)){storage_state="STORAGE_LOG_MIGRATION_ERROR";return false;}
            n=0;
        }
    }
    static const char *const upload_archives[]={"upload.v1.csv","upload.pre_factory.csv","upload.v2.csv"};
    if(!archive_first_free("upload.csv",upload_archives,sizeof(upload_archives)/sizeof(upload_archives[0]))) {storage_state="STORAGE_UPLOAD_MIGRATION_ERROR";return false;}
    /* Preserve a legacy interrupted-job marker for inspection. */
    static const char *const job_archives[]={"inflight.v1.job","inflight.pre_factory.job","inflight.v2.job"};
    if(!archive_first_free("inflight.job",job_archives,sizeof(job_archives)/sizeof(job_archives[0]))) {storage_state="STORAGE_JOB_MIGRATION_ERROR";return false;}
    if(!n && !write_file(LOG_NAME,header,sizeof(header)-1)){storage_state="STORAGE_LOG_CREATE_ERROR";return false;}
    n=file_size(JOB_NAME);if(n<0){storage_state="STORAGE_JOB_READ_ERROR";return false;}
    if(n) {
        if(n!=(int)sizeof(job) || read_file(JOB_NAME,0,&job,sizeof(job))!=n || job.magic!=JOB_MAGIC ||
           job.version[sizeof(job.version)-1] || job.crc!=image_crc32((const uint8_t *)&job,offsetof(local_job_t,crc))){storage_state="STORAGE_JOB_INVALID";return false;}
        programmer_report_t r={.result=RESULT_RESET};
        uint64_t archived_id=0;
        bool already_logged=lfs_getattr(&fs,export_names[3],1,&archived_id,sizeof(archived_id))==(int)sizeof(archived_id) && archived_id==job.id;
        if((!already_logged && !append_result(&r)) || lfs_remove(&fs,JOB_NAME)<0){storage_state="STORAGE_JOB_RECOVERY_ERROR";return false;}
    }
    if(refresh_log_capacity()<0){storage_state="STORAGE_CAPACITY_ERROR";return false;}
    storage_state="STORAGE_OK";return true;
}
/* -1=unreadable, 0=capacity available, 1=byte limit reached. */
static int log_capacity_state(void) {
    int size = file_size(LOG_NAME);
    /* Use the saved size across restart; reserve space for the next result.
     * Missing/unreadable storage blocks START before any target operation. */
    if (size < (int)(sizeof(header) - 1u)) return -1;
    return (uint32_t)size + 512u > LOG_LIMIT ? 1 : 0;
}
static int refresh_log_capacity(void) {
    int state=log_capacity_state();
    logs_full=state>0;
    return state;
}
bool local_logs_full(void) { return logs_full; }

bool storage_job_begin(uint64_t timestamp,bool utc,uint32_t size) 
{
    if(!mounted || job_open || snapshot_open || timestamp>UINT32_MAX)
        return false;
    if(refresh_log_capacity()!=0)return false;
    memset(&job,0,sizeof(job));job.magic=JOB_MAGIC;job.id=get_rand_64();
    job.size=size;job.timestamp=utc?(uint32_t)timestamp:0;
    snprintf(job.version,sizeof(job.version),"%s",TARGET_FIRMWARE_VERSION);
    job.crc=image_crc32((const uint8_t *)&job,offsetof(local_job_t,crc));
    job_open=write_file(JOB_NAME,&job,sizeof(job));return job_open;
}
bool storage_job_finish(const programmer_report_t *r) {
    if(!job_open || snapshot_open || !append_result(r))return false;
    if(lfs_remove(&fs,JOB_NAME)<0)return false;
    job_open=false;return refresh_log_capacity()>=0;
}
static bool hash_file(const char *name,uint32_t size,char etag[65]) {
    mbedtls_sha256_context ctx;mbedtls_sha256_init(&ctx);bool ok=mbedtls_sha256_starts(&ctx,0)==0;
    uint8_t bytes[1024],digest[32];
    for(uint32_t off=0;ok && off<size;) {
        uint32_t n=size-off;if(n>sizeof(bytes))n=sizeof(bytes);
        if(read_file(name,off,bytes,n)!=(int)n){ok=false;break;}
        ok=mbedtls_sha256_update(&ctx,bytes,n)==0;off+=n;
    }
    if(ok)ok=mbedtls_sha256_finish(&ctx,digest)==0;
    mbedtls_sha256_free(&ctx);if(!ok)return false;
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;i++){etag[2*i]=hex[digest[i]>>4];etag[2*i+1]=hex[digest[i]&15];}etag[64]=0;return true;
}
bool local_snapshot_begin(unsigned file,uint32_t *size,char etag[65]) {
    if(!mounted || job_open || snapshot_open || file>=sizeof(export_names)/sizeof(export_names[0]))return false;
    int n=file_size(export_names[file]);if(n<=0 || (uint32_t)n>LOG_FS_SIZE)return false;
    snapshot_open=true;snapshot_size=(uint32_t)n;snapshot_file=file;
    if(!hash_file(export_names[file],snapshot_size,etag)){snapshot_open=false;return false;}
    *size=snapshot_size;return true;
}
int local_snapshot_read(uint32_t offset,void *data,size_t n) {
    if(!snapshot_open || offset>snapshot_size)return -1;
    if(n>snapshot_size-offset)n=snapshot_size-offset;
    return read_file(export_names[snapshot_file],offset,data,n);
}
void local_snapshot_end(void) { snapshot_open=false; }
int local_logs_clear(const char *etag) {
    if(!mounted || job_open || snapshot_open || strlen(etag)!=64)return -1;
    uint32_t n;char current[65];
    if(!local_snapshot_begin(0,&n,current))return -1;
    local_snapshot_end();
    if(strcmp(etag,current))return 1;
    /* Atomic replacement: a power cut leaves either the complete old file or
     * the header-only new file. Never format LittleFS or touch the image. */
    if(!write_file(CLEAR_NAME,header,sizeof(header)-1) || lfs_rename(&fs,CLEAR_NAME,LOG_NAME)<0)return -1;
    return refresh_log_capacity()<0?-1:0;
}

void local_device_ssid(char out[33]) {
    const char *base = FACTORY_ROLE==FACTORY_ROLE_MASTER ? "Str8bat_Master" :
                       FACTORY_ROLE==FACTORY_ROLE_SLAVE ? "Str8bat_Slave" : WIFI_LOCAL_SSID;
    snprintf(out,33,"%s_%s",base,device_id);
}
bool local_factory_bin_available(void) { return factory_bin.magic==FACTORY_BIN_MAGIC; }
stored_image_t local_factory_bin_image(void) {
    return (stored_image_t){xip(FACTORY_BIN_DATA_BASE),factory_bin.size,factory_bin.crc,factory_bin.generation};
}
uint32_t local_factory_bin_received_epoch(void) { return factory_bin.received_epoch; }
bool factory_bin_available(void) { return local_factory_bin_available(); }
stored_image_t factory_bin_image(void) { return local_factory_bin_image(); }
uint32_t factory_bin_generation(void) { return factory_bin.generation; }
uint32_t factory_bin_received_epoch(void) { return factory_bin.received_epoch; }
const char *factory_bin_label(void) { return factory_bin_available()?factory_bin.label:TARGET_FIRMWARE_VERSION; }
int factory_bin_read(uint32_t offset,void *data,size_t size) {
    if(!factory_bin_available() || offset>factory_bin.size)return -1;
    if(size>factory_bin.size-offset)size=factory_bin.size-offset;
    memcpy(data,xip(FACTORY_BIN_DATA_BASE+offset),size);return (int)size;
}
void factory_bin_abort(void) { factory_upload_open=false;factory_page_used=0; }
bool factory_bin_upload_active(void) { return factory_upload_open; }
const char *factory_bin_last_error(void) { return factory_upload_error; }
bool factory_bin_begin(uint32_t size,uint32_t expected_crc,uint32_t received_epoch,const char *label) {
    if(factory_upload_open || job_open || snapshot_open || size<8u || size>FACTORY_BIN_MAX_SIZE || (size&7u) || !valid_bin_label(label)) {
        factory_upload_error="BIN upload cannot start while the programmer is busy.";return false;
    }
    /* A full 516 KiB erase can exceed the SDK flash-lockout timeout. Erase
     * 4 KiB sectors separately, exactly as LittleFS does. Invalidate the RAM
     * header first so an interrupted erase can never serve the old image. */
    factory_upload_generation=factory_bin.generation+1u;
    if(!factory_upload_generation)factory_upload_generation=1;
    memset(&factory_bin,0,sizeof(factory_bin));
    for(uint32_t off=FACTORY_BIN_HEADER_BASE;off<FACTORY_BIN_HEADER_BASE+FACTORY_BIN_ERASE_BYTES;off+=4096u)
        if(!mutate(off,4096u,NULL)){factory_upload_error="Could not erase factory BIN storage.";return false;}
    factory_upload_open=true;factory_expected_crc=expected_crc;factory_expected_size=size;
    factory_written=0;factory_received_epoch=received_epoch;factory_page_used=0;memcpy(factory_upload_label,label,sizeof(factory_upload_label));factory_upload_error="Receiving BIN.";
    memset(factory_page,0xff,sizeof(factory_page));return true;
}
static bool factory_bin_flush(void) {
    if(!factory_page_used)return true;
    /* Flash programming is performed in 256-byte pages, while a valid STM32
     * image only needs to be aligned to eight bytes.  Pad the unused tail of
     * the final page with erased-flash bytes, without changing the logical
     * image length used for its CRC or for later programming. */
    if(factory_page_used<sizeof(factory_page))
        memset(factory_page+factory_page_used,0xff,sizeof(factory_page)-factory_page_used);
    uint32_t offset=FACTORY_BIN_DATA_BASE+factory_written-factory_page_used;
    bool ok=mutate(offset,sizeof(factory_page),factory_page);
    memset(factory_page,0xff,sizeof(factory_page));factory_page_used=0;return ok;
}
bool factory_bin_write(uint32_t offset,const void *data,size_t size) {
    if(!factory_upload_open || !size)return false;
    /* A phone may lose the HTTP reply after this Pico has already saved a
     * chunk. Accept an exact retry, but reject a changed or out-of-order one. */
    if(offset!=factory_written) {
        if(offset>factory_written || size>factory_written-offset)return false;
        const uint8_t *retry=data;
        uint32_t programmed=factory_written-factory_page_used;
        size_t from_flash=offset<programmed?programmed-offset:0;
        if(from_flash>size)from_flash=size;
        if(from_flash && memcmp(xip(FACTORY_BIN_DATA_BASE+offset),retry,from_flash))return false;
        if(from_flash<size) {
            uint32_t page_offset=offset+(uint32_t)from_flash-programmed;
            if(page_offset>factory_page_used || size-from_flash>factory_page_used-page_offset ||
               memcmp(factory_page+page_offset,retry+from_flash,size-from_flash))return false;
        }
        return true;
    }
    if(size>FACTORY_BIN_MAX_SIZE-factory_written)return false;
    const uint8_t *p=data;
    while(size) {
        size_t n=sizeof(factory_page)-factory_page_used;if(n>size)n=size;
        memcpy(factory_page+factory_page_used,p,n);factory_page_used+=(uint32_t)n;factory_written+=(uint32_t)n;p+=n;size-=n;
        if(factory_page_used==sizeof(factory_page) && !factory_bin_flush()){factory_bin_abort();return false;}
    }
    return true;
}
bool factory_bin_commit(void) {
    if(!factory_upload_open) {
        if(factory_bin_valid(&factory_bin)){factory_upload_error="BIN was already stored and CRC verified.";return true;}
        factory_upload_error="No active BIN upload.";return false;
    }
    if(!factory_written || factory_written!=factory_expected_size) {
        factory_upload_error="Final received BIN size does not match the selected file.";factory_bin_abort();return false;
    }
    if(factory_page_used && !factory_bin_flush()){factory_upload_error="Could not write the final BIN flash page.";factory_bin_abort();return false;}
    const uint8_t *data=xip(FACTORY_BIN_DATA_BASE);
    uint32_t stored_crc=image_crc32(data,factory_written);
#if FACTORY_ROLE != FACTORY_ROLE_MASTER
    /* A slave receives the Master-provided CRC and must match it exactly. */
    if(stored_crc!=factory_expected_crc){factory_upload_error="Slave BIN CRC does not match the Master CRC.";factory_bin_abort();return false;}
#endif
    /* The Master computes the CRC from the bytes held in its own flash.  This
     * avoids depending on JavaScript's integer implementation in a phone
     * browser while still providing a stored-image integrity check. */
    factory_expected_crc=stored_crc;
    factory_bin_header_t h={.magic=FACTORY_BIN_MAGIC,.generation=factory_upload_generation,
        .size=factory_written,.crc=factory_expected_crc,.received_epoch=factory_received_epoch};
    memcpy(h.label,factory_upload_label,sizeof(h.label));
    h.header_crc=image_crc32((const uint8_t *)&h,offsetof(factory_bin_header_t,header_crc));
    uint8_t page[256];memset(page,0xff,sizeof(page));memcpy(page,&h,sizeof(h));
    if(!mutate(FACTORY_BIN_HEADER_BASE,sizeof(page),page) || !factory_bin_valid(&h)){
        factory_upload_error="Stored BIN header verification failed.";factory_bin_abort();return false;
    }
    factory_bin=h;factory_upload_error="BIN stored and Pico CRC verified.";factory_bin_abort();return true;
}
bool factory_peers_set(const char *csv_ids) {
    if(FACTORY_ROLE!=FACTORY_ROLE_MASTER || !csv_ids || job_open || snapshot_open)return false;
    factory_pair_record_t next={.magic=FACTORY_PAIR_MAGIC};
    const char *p=csv_ids;
    for(unsigned i=0;i<3;i++) {
        for(unsigned j=0;j<16;j++) { if(!p[j])return false;next.ids[i][j]=p[j]; }
        next.ids[i][16]=0;if(!valid_id(next.ids[i]))return false;p+=16;
        if(i<2) { if(*p!=',')return false;p++; } else if(*p)return false;
    }
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<i;j++)if(!strcmp(next.ids[i],next.ids[j]))return false;
    next.crc=image_crc32((const uint8_t *)&next,offsetof(factory_pair_record_t,crc));
    uint8_t page[256];memset(page,0xff,sizeof(page));memcpy(page,&next,sizeof(next));
    if(!mutate(FACTORY_PAIR_BASE,4096,NULL) || !mutate(FACTORY_PAIR_BASE,sizeof(page),page))return false;
    factory_pairs=next;return true;
}
bool factory_peer_allowed(const char *id) {
    if(FACTORY_ROLE!=FACTORY_ROLE_MASTER || !valid_id(id))return false;
    for(unsigned i=0;i<3;i++)if(!strcmp(id,factory_pairs.ids[i]))return true;
    return false;
}
unsigned factory_peer_count(void) {
    unsigned n=0;for(unsigned i=0;i<3;i++)if(valid_id(factory_pairs.ids[i]))n++;return n;
}
const char *factory_peer_id(unsigned index) { return index<3?factory_pairs.ids[index]:""; }
