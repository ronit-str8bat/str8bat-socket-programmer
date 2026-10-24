#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "stm32_programmer.h"

/* Offsets from the START of the Pico's 4 MiB flash, never target addresses. */
#define APP_FLASH_LIMIT 0x180000u
#define IMAGE_MAX_SIZE  0x80000u
#define FACTORY_BIN_MAX_SIZE IMAGE_MAX_SIZE
#define IMAGE_SLOT_A    0x180000u
#define IMAGE_SLOT_B    0x201000u
#define FACTORY_BIN_HEADER_BASE IMAGE_SLOT_A
#define FACTORY_BIN_DATA_BASE   (IMAGE_SLOT_A + 0x1000u)
#define FACTORY_BIN_ERASE_BYTES (0x1000u + FACTORY_BIN_MAX_SIZE)
/* Factory pairing record. This dedicated 4 KiB sector is separate from the
 * program BIN, CSV LittleFS and credential sector. */
#define FACTORY_PAIR_BASE 0x281000u
#define LOG_FS_BASE     0x282000u
#define CREDENTIAL_BASE 0x3ff000u
#define LOG_FS_SIZE     (CREDENTIAL_BASE - LOG_FS_BASE)
typedef struct { char ssid[33]; char password[64]; } wifi_credentials_t;
typedef struct {
    const uint8_t *data;
    uint32_t size, crc, revision;
} stored_image_t;
/* All functions below are core-0-only; call only while SWD worker is idle.
 * Flash mutations use flash_safe_execute; core1 must initialize its victim. */
bool storage_init(void);
const char *storage_init_status(void);
stored_image_t storage_image(void);
bool storage_credentials_read(wifi_credentials_t *out);
bool storage_credentials_save(const wifi_credentials_t *credentials);
bool storage_ota_begin(uint32_t revision, uint32_t size, const uint8_t sha[32]);
bool storage_ota_write(const void *data, size_t size);
bool storage_ota_commit(void);
void storage_ota_abort(void);
const char *storage_device_id(void);
/* Reserve a durable job marker BEFORE altering target flash. Exhausted or
 * damaged storage blocks START, rather than silently losing production logs. */
bool storage_job_begin(uint64_t timestamp, bool utc, uint32_t image_size);
bool storage_job_finish(const programmer_report_t *report);
bool storage_batch_prepare(uint32_t *size, char id[65]);
int storage_batch_read(uint32_t offset, void *data, size_t size);
bool storage_batch_ack(const char *id);
void storage_sha_hex(const uint8_t hash[32], char out[65]);
/* Read-only CSV view: one header, pending-upload rows, then current rows.
 * Holds a mutation lock until end(); does not rename, acknowledge or delete.
 * Core 0 only. Cancel an export before starting a new programming job. */
bool storage_export_begin(uint32_t *size);
int storage_export_read(uint32_t offset, void *data, size_t size);
void storage_export_end(void);

/* Local factory distributor image store. The upload header is committed only
 * after the entire binary CRC is verified, so a power cut cannot launch a
 * partial image. These calls are core-0-only while no SWD job is active. */
bool factory_bin_begin(uint32_t size, uint32_t expected_crc, uint32_t received_epoch,const char *label);
bool factory_bin_write(uint32_t offset, const void *data, size_t size);
bool factory_bin_commit(void);
void factory_bin_abort(void);
bool factory_bin_upload_active(void);
const char *factory_bin_last_error(void);
bool factory_bin_available(void);
stored_image_t factory_bin_image(void);
int factory_bin_read(uint32_t offset, void *data, size_t size);
uint32_t factory_bin_generation(void);
uint32_t factory_bin_received_epoch(void);
const char *factory_bin_label(void);

/* Master-only list of up to three authorized slave Pico IDs. The production
 * list is fixed in wifi_config.h; empty slots are allowed for future slaves. */
bool factory_peers_set(const char *csv_ids);
bool factory_peer_allowed(const char *id);
unsigned factory_peer_count(void);
const char *factory_peer_id(unsigned index);
