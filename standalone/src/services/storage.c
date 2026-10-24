/* Persistent state is written only between target jobs, from core 0.
 * OTA uses two independent slots, with the commit header written LAST.
 * LittleFS provides atomic metadata commits and wear levelling for logs.
 */
#include "services/storage.h"
#include "wifi_config.h"
#include "firmware_image.h"
#include "crc32.h"
#include "device_key.h"
#include "lfs.h"
#include "pico/flash.h"
#include "pico/rand.h"
#include "pico/unique_id.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stddef.h>

#define SLOT_MAGIC 0x494d4731u
#define CRED_MAGIC 0x57494631u
#define LOG_LIMIT (256u*1024u)
#define ACTIVE_LOG "programming_logs.csv"
#define BATCH_LOG "upload.csv"
#define JOB_FILE "inflight.job"
static const char csv_header[]="job_id,pico_id,stm32_uid,time_kind,timestamp,elapsed_ms,status,image_bytes,result,stage,address,detail\n";
typedef struct { uint32_t magic,revision,size,crc; uint8_t sha[32]; uint32_t header_crc; } image_header_t;
typedef struct { uint32_t magic; uint8_t nonce[12],cipher[sizeof(wifi_credentials_t)],tag[16]; } credential_record_t;
typedef struct { uint64_t id,timestamp; uint32_t utc,size,crc; } job_record_t;
static lfs_t fs;
static bool mounted,job_open,ota_open;
static bool export_open;
static uint32_t export_lengths[2],export_size;
static job_record_t job;
static stored_image_t active;
static uint32_t active_slot,ota_slot,ota_written,ota_revision,ota_size;
static uint8_t ota_sha[32],ota_page[256];
static uint32_t ota_page_used;
static char device_id[2*PICO_UNIQUE_BOARD_ID_SIZE_BYTES+1];
static uint8_t credential_key[32];
static bool last_flash_ok;

typedef struct { uint32_t offset,size; const uint8_t *data; bool erase; } flash_operation_t;
static void flash_operation(void *p) {
    flash_operation_t *op=p;
    if(op->erase)flash_range_erase(op->offset,op->size);
    else flash_range_program(op->offset,op->data,op->size);
}
static const uint8_t *xip(uint32_t offset) { return (const uint8_t *)(XIP_BASE+offset); }
static bool mutate(uint32_t offset,uint32_t size,const void *data,bool erase) {
    if(export_open)return false;
    /* No operation may address the executable or embedded factory image. */
    if(offset<APP_FLASH_LIMIT || offset>0x400000u || size>0x400000u-offset ||
       (offset%(erase?4096u:256u)) || (size%(erase?4096u:256u)))return false;
    flash_operation_t op={offset,size,data,erase};
    last_flash_ok=flash_safe_execute(flash_operation,&op,1000)==PICO_OK;
    if(last_flash_ok && !erase)last_flash_ok=!memcmp(xip(offset),data,size);
    return last_flash_ok;
}
static bool sha256(const void *data,size_t n,uint8_t out[32]) {
    return mbedtls_sha256(data,n,out,0)==0;
}
void storage_sha_hex(const uint8_t hash[32],char out[65]) {
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;i++){out[i*2]=hex[hash[i]>>4];out[i*2+1]=hex[hash[i]&15];}out[64]=0;
}
static bool vectors_valid(const uint8_t *data,uint32_t size) {
    uint32_t sp,pc;memcpy(&sp,data,4);memcpy(&pc,data+4,4);
    return size>=8 && size<=IMAGE_MAX_SIZE && !(size&7) && !(sp&7) &&
        ((sp>0x20000000u && sp<=0x20018000u) || (sp>0x10000000u && sp<=0x10008000u)) &&
        (pc&1) && (pc&~1u)>=0x08000000u && (pc&~1u)<0x08000000u+size;
}
static bool slot_valid(uint32_t slot,image_header_t *header) {
    memcpy(header,xip(slot),sizeof(*header));
    if(header->magic!=SLOT_MAGIC || header->size<8 || header->size>IMAGE_MAX_SIZE ||
       header->header_crc!=image_crc32((const uint8_t *)header,offsetof(image_header_t,header_crc)))return false;
    const uint8_t *data=xip(slot+4096);uint8_t hash[32];
    return vectors_valid(data,header->size) && image_crc32(data,header->size)==header->crc &&
        sha256(data,header->size,hash) && !memcmp(hash,header->sha,32);
}
stored_image_t storage_image(void) { return active; }
const char *storage_device_id(void) { return device_id; }

static int fs_read(const struct lfs_config *c,lfs_block_t block,lfs_off_t off,void *buffer,lfs_size_t n) {
    (void)c;if(block>=LOG_FS_SIZE/4096 || off>4096 || n>4096-off)return LFS_ERR_IO;
    memcpy(buffer,xip(LOG_FS_BASE+block*4096+off),n);return 0;
}
static int fs_prog(const struct lfs_config *c,lfs_block_t block,lfs_off_t off,const void *buffer,lfs_size_t n) {
    (void)c;if(block>=LOG_FS_SIZE/4096 || off>4096 || n>4096-off)return LFS_ERR_IO;
    return mutate(LOG_FS_BASE+block*4096+off,n,buffer,false)?0:LFS_ERR_IO;
}
static int fs_erase(const struct lfs_config *c,lfs_block_t block) {
    (void)c;if(block>=LOG_FS_SIZE/4096)return LFS_ERR_IO;
    return mutate(LOG_FS_BASE+block*4096,4096,NULL,true)?0:LFS_ERR_IO;
}
static int fs_sync(const struct lfs_config *c) { (void)c;return 0; }
static const struct lfs_config cfg={.read=fs_read,.prog=fs_prog,.erase=fs_erase,.sync=fs_sync,
    .read_size=16,.prog_size=256,.block_size=4096,.block_count=LOG_FS_SIZE/4096,
    .block_cycles=500,.cache_size=256,.lookahead_size=64};
static bool file_write(const char *path,const void *data,size_t size,int flags) {
    lfs_file_t file;
    if(lfs_file_open(&fs,&file,path,flags)<0)return false;
    bool ok=lfs_file_write(&fs,&file,data,size)==(lfs_ssize_t)size;
    if(lfs_file_close(&fs,&file)<0)ok=false;
    return ok;
}
static int file_read(const char *path,uint32_t offset,void *data,size_t size) {
    lfs_file_t file;if(lfs_file_open(&fs,&file,path,LFS_O_RDONLY)<0)return -1;
    int n=-1;
    if(lfs_file_seek(&fs,&file,offset,LFS_SEEK_SET)==(lfs_soff_t)offset)n=lfs_file_read(&fs,&file,data,size);
    if(lfs_file_close(&fs,&file)<0)n=-1;
    return n;
}
static int file_size(const char *path) {
    struct lfs_info info;int r=lfs_stat(&fs,path,&info);
    return r==LFS_ERR_NOENT?0:r<0?-1:(int)info.size;
}
static bool create_log(void) {
    if(file_size(ACTIVE_LOG)>0)return true;
    return file_write(ACTIVE_LOG,csv_header,strlen(csv_header),LFS_O_WRONLY|LFS_O_CREAT|LFS_O_TRUNC);
}
static bool append_result(const programmer_report_t *r,const char *status) {
    char uid[25]="UNAVAILABLE",line[320];
    if(r->uid_valid)snprintf(uid,sizeof(uid),"%08" PRIx32 "%08" PRIx32 "%08" PRIx32,r->uid[0],r->uid[1],r->uid[2]);
    int n=snprintf(line,sizeof(line),"%016" PRIx64 ",%s,%s,%s,%" PRIu64 ",%" PRIu32 ",%s,%" PRIu32 ",%u,%u,%08" PRIx32 ",%08" PRIx32 "\n",
        job.id,device_id,uid,job.utc?"UTC_UNIX":"UPTIME_MS",job.timestamp,r->elapsed_ms,status,job.size,
        (unsigned)r->result,(unsigned)r->stage,r->address,r->detail);
    return n>0 && n<(int)sizeof(line) && create_log() && file_write(ACTIVE_LOG,line,(size_t)n,LFS_O_WRONLY|LFS_O_APPEND);
}
/* A completed CSV append may survive power loss before removal of its marker.
 * Search both retained files to avoid recording that job as interrupted too. */
static bool job_recorded(const char *path,uint64_t id) {
    char needle[18];snprintf(needle,sizeof(needle),"%016" PRIx64 ",",id);
    lfs_file_t file;if(lfs_file_open(&fs,&file,path,LFS_O_RDONLY)<0)return false;
    char block[512];unsigned matched=0;bool found=false,candidate=true;
    for(;;){int n=lfs_file_read(&fs,&file,block,sizeof(block));if(n<=0)break;
        for(int i=0;i<n;i++) {
            if(block[i]=='\n'){matched=0;candidate=true;}
            else if(candidate) {
                if(block[i]!=needle[matched])candidate=false;
                else if(++matched==17){found=true;break;}
            }
        }
        if(found)break;
    }
    lfs_file_close(&fs,&file);return found;
}
bool storage_init(void) {
    pico_get_unique_board_id_string(device_id,sizeof(device_id));
    /* Bind this build's private key to the Pico identity. Key remains readable
     * by an attacker able to extract firmware; this is not a secure element. */
    uint8_t material[32+sizeof(device_id)];
    memcpy(material,device_storage_key,32);memcpy(material+32,device_id,sizeof(device_id));
    if(!sha256(material,sizeof(material),credential_key))return false;
    memset(material,0,sizeof(material));
    active=(stored_image_t){firmware_image,firmware_image_size,firmware_image_crc32,FACTORY_IMAGE_REVISION};
    image_header_t h;
    const uint32_t slots[]={IMAGE_SLOT_A,IMAGE_SLOT_B};
    for(unsigned i=0;i<2;i++)if(slot_valid(slots[i],&h) && h.revision>active.revision) {
        active=(stored_image_t){xip(slots[i]+4096),h.size,h.crc,h.revision};active_slot=slots[i];
    }
    if(lfs_mount(&fs,&cfg)<0) {
        /* Only a factory-erased partition can be formatted automatically. */
        for(uint32_t i=0;i<LOG_FS_SIZE;i++)if(xip(LOG_FS_BASE)[i]!=0xff)return false;
        if(lfs_format(&fs,&cfg)<0 || lfs_mount(&fs,&cfg)<0)return false;
    }
    mounted=true;
    int n=file_size(JOB_FILE);
    if(n<0)return false;
    if(n) {
        if(n!=(int)sizeof(job) || file_read(JOB_FILE,0,&job,sizeof(job))!=n || job.crc!=image_crc32((const uint8_t *)&job,offsetof(job_record_t,crc)))return false;
        programmer_report_t r={.result=RESULT_RESET};
        if(!job_recorded(ACTIVE_LOG,job.id) && !job_recorded(BATCH_LOG,job.id) && !append_result(&r,"INTERRUPTED"))return false;
        if(lfs_remove(&fs,JOB_FILE)<0)return false;
    }
    return create_log();
}
bool storage_job_begin(uint64_t timestamp,bool utc,uint32_t size) {
    if(!mounted || job_open || ota_open || export_open)return false;
    int a=file_size(ACTIVE_LOG),b=file_size(BATCH_LOG);
    if(a<0 || b<0 || (unsigned)(a+b)+1024>LOG_LIMIT)return false;
    memset(&job,0,sizeof(job));job.id=get_rand_64();job.timestamp=timestamp;job.utc=utc;job.size=size;
    job.crc=image_crc32((const uint8_t *)&job,offsetof(job_record_t,crc));
    job_open=file_write(JOB_FILE,&job,sizeof(job),LFS_O_WRONLY|LFS_O_CREAT|LFS_O_TRUNC);
    return job_open;
}
bool storage_job_finish(const programmer_report_t *r) {
    if(!job_open)return false;
    const char *status=r->result==RESULT_OK?"SUCCESS":r->result==RESULT_IMAGE?"IMAGE_FAIL":
        r->result==RESULT_VERIFY?"VERIFY_MISMATCH":r->stage==STAGE_ERASE?"ERASE_FAIL":
        r->stage==STAGE_PROGRAM?"WRITE_FAIL":r->stage==STAGE_RESTART?"RESTART_FAIL":"CONNECT_FAIL";
    if(!job_recorded(ACTIVE_LOG,job.id) && !append_result(r,status))return false;
    if(lfs_remove(&fs,JOB_FILE)<0)return false;
    job_open=false;return true;
}
bool storage_batch_prepare(uint32_t *size,char id[65]) {
    if(!mounted || job_open || ota_open || export_open)return false;
    int n=file_size(BATCH_LOG);
    if(n<0)return false;
    if(!n) {
        n=file_size(ACTIVE_LOG);
        if(n<=(int)strlen(csv_header))return false;
        if(lfs_rename(&fs,ACTIVE_LOG,BATCH_LOG)<0)return false;
    }
    mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);
    bool ok=mbedtls_sha256_starts(&sha,0)==0;
    uint8_t buffer[512],hash[32];
    for(int off=0;ok && off<n;) {
        int len=file_read(BATCH_LOG,(uint32_t)off,buffer,sizeof(buffer));
        if(len<=0){ok=false;break;}ok=mbedtls_sha256_update(&sha,buffer,(size_t)len)==0;off+=len;
    }
    if(ok)ok=mbedtls_sha256_finish(&sha,hash)==0;
    mbedtls_sha256_free(&sha);if(!ok)return false;
    storage_sha_hex(hash,id);*size=(uint32_t)n;return true;
}
int storage_batch_read(uint32_t offset,void *data,size_t size) { return file_read(BATCH_LOG,offset,data,size); }
bool storage_batch_ack(const char *id) {
    char current[65];uint32_t size;
    return storage_batch_prepare(&size,current) && !strcmp(current,id) && lfs_remove(&fs,BATCH_LOG)==0;
}
bool storage_credentials_read(wifi_credentials_t *out) {
    credential_record_t r;memcpy(&r,xip(CREDENTIAL_BASE),sizeof(r));
    if(r.magic!=CRED_MAGIC)return false;
    mbedtls_gcm_context gcm;mbedtls_gcm_init(&gcm);
    int rc=mbedtls_gcm_setkey(&gcm,MBEDTLS_CIPHER_ID_AES,credential_key,256);
    if(!rc)rc=mbedtls_gcm_auth_decrypt(&gcm,sizeof(*out),r.nonce,sizeof(r.nonce),
        (const uint8_t *)&r.magic,sizeof(r.magic),r.tag,sizeof(r.tag),r.cipher,(uint8_t *)out);
    mbedtls_gcm_free(&gcm);
    if(rc || out->ssid[32] || out->password[63] || !out->ssid[0] || strlen(out->password)<8){memset(out,0,sizeof(*out));return false;}
    return true;
}
bool storage_credentials_save(const wifi_credentials_t *c) {
    if(job_open || ota_open || export_open || c->ssid[32] || c->password[63] || !c->ssid[0] || strlen(c->password)<8)return false;
    uint8_t page[256];memset(page,0xff,sizeof(page));credential_record_t r={.magic=CRED_MAGIC};
    for(unsigned i=0;i<sizeof(r.nonce);i+=4){uint32_t n=get_rand_32();memcpy(r.nonce+i,&n,4);}
    mbedtls_gcm_context gcm;mbedtls_gcm_init(&gcm);
    int rc=mbedtls_gcm_setkey(&gcm,MBEDTLS_CIPHER_ID_AES,credential_key,256);
    if(!rc)rc=mbedtls_gcm_crypt_and_tag(&gcm,MBEDTLS_GCM_ENCRYPT,sizeof(*c),r.nonce,sizeof(r.nonce),
        (const uint8_t *)&r.magic,sizeof(r.magic),(const uint8_t *)c,r.cipher,sizeof(r.tag),r.tag);
    mbedtls_gcm_free(&gcm);if(rc)return false;
    memcpy(page,&r,sizeof(r));
    if(!mutate(CREDENTIAL_BASE,4096,NULL,true) || !mutate(CREDENTIAL_BASE,256,page,false))return false;
    wifi_credentials_t check;bool ok=storage_credentials_read(&check) && !memcmp(c,&check,sizeof(check));
    memset(&check,0,sizeof(check));return ok;
}
bool storage_ota_begin(uint32_t revision,uint32_t size,const uint8_t sha[32]) {
    if(job_open || ota_open || export_open || revision<=active.revision || size<8 || size>IMAGE_MAX_SIZE || (size&7))return false;
    ota_slot=active_slot==IMAGE_SLOT_A?IMAGE_SLOT_B:IMAGE_SLOT_A;
    if(!mutate(ota_slot,4096,NULL,true))return false;
    ota_open=true;ota_revision=revision;ota_size=size;ota_written=0;ota_page_used=0;memcpy(ota_sha,sha,32);
    memset(ota_page,0xff,256);return true;
}
static bool ota_flush(void) {
    uint32_t offset=ota_slot+4096+ota_written-ota_page_used;
    if(!(offset%4096) && !mutate(offset,4096,NULL,true))return false;
    if(!mutate(offset,256,ota_page,false))return false;
    memset(ota_page,0xff,256);ota_page_used=0;return true;
}
bool storage_ota_write(const void *data,size_t n) {
    if(!ota_open || n>ota_size-ota_written)return false;
    const uint8_t *p=data;
    while(n){size_t part=256-ota_page_used;if(part>n)part=n;memcpy(ota_page+ota_page_used,p,part);
        ota_page_used+=(uint32_t)part;ota_written+=(uint32_t)part;p+=part;n-=part;
        if(ota_page_used==256 && !ota_flush()){storage_ota_abort();return false;}
    }return true;
}
bool storage_ota_commit(void) {
    if(!ota_open || ota_written!=ota_size)return false;
    if(ota_page_used && !ota_flush())return false;
    const uint8_t *data=xip(ota_slot+4096);uint8_t hash[32];
    if(!vectors_valid(data,ota_size) || !sha256(data,ota_size,hash) || memcmp(hash,ota_sha,32))return false;
    image_header_t h={.magic=SLOT_MAGIC,.revision=ota_revision,.size=ota_size,.crc=image_crc32(data,ota_size)};
    memcpy(h.sha,hash,32);h.header_crc=image_crc32((const uint8_t *)&h,offsetof(image_header_t,header_crc));
    uint8_t page[256];memset(page,0xff,256);memcpy(page,&h,sizeof(h));
    if(!mutate(ota_slot,256,page,false) || !slot_valid(ota_slot,&h))return false;
    active=(stored_image_t){data,h.size,h.crc,h.revision};active_slot=ota_slot;storage_ota_abort();return true;
}
void storage_ota_abort(void) { ota_open=false;ota_page_used=0; }

bool storage_export_begin(uint32_t *size) {
    if(!mounted || job_open || ota_open || export_open)return false;
    const char *paths[]={BATCH_LOG,ACTIVE_LOG};
    const uint32_t header_size=sizeof(csv_header)-1;
    export_size=header_size;
    for(unsigned i=0;i<2;i++) {
        int n=file_size(paths[i]);
        if(n<0 || (uint32_t)n>LOG_FS_SIZE)return false;
        export_lengths[i]=0;
        if(n) {
            char header[sizeof(csv_header)-1];
            if((uint32_t)n<header_size || file_read(paths[i],0,header,sizeof(header))!=(int)sizeof(header) ||
               memcmp(header,csv_header,sizeof(header)))return false;
            export_lengths[i]=(uint32_t)n-header_size;
            export_size+=export_lengths[i];
        }
    }
    if(export_size>LOG_FS_SIZE)return false;
    export_open=true;*size=export_size;return true;
}
int storage_export_read(uint32_t offset,void *data,size_t size) {
    if(!export_open || offset>export_size)return -1;
    if(size>export_size-offset)size=export_size-offset;
    const char *paths[]={BATCH_LOG,ACTIVE_LOG};
    const uint32_t header_size=sizeof(csv_header)-1;
    uint8_t *out=data;size_t done=0;
    while(done<size) {
        uint32_t position=offset+(uint32_t)done;
        size_t count=size-done;
        if(position<header_size) {
            if(count>header_size-position)count=header_size-position;
            memcpy(out+done,csv_header+position,count);
        } else {
            position-=header_size;
            unsigned file=0;
            if(position>=export_lengths[0]){position-=export_lengths[0];file=1;}
            if(position>=export_lengths[file])return -1;
            if(count>export_lengths[file]-position)count=export_lengths[file]-position;
            if(file_read(paths[file],position+header_size,out+done,count)!=(int)count)return -1;
        }
        done+=count;
    }
    return (int)done;
}
void storage_export_end(void) { export_open=false; }
