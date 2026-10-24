#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "services/storage.h"

/* Core-0-only hardware timekeeping: phone seeds the RP2350 POWMAN timer after
 * boot; subsequent time is read from hardware, independent of Wi-Fi. */
bool local_time_set(uint32_t unix_seconds);
bool local_time_valid(void);
uint32_t local_time_now(void);
bool local_format_utc(uint32_t seconds,char out[20]);
bool local_format_ist(uint32_t seconds,char out[20]);
bool local_parse_time(const char *json,uint32_t *seconds);
unsigned local_start_error(void);
/* At least one device is associated with the AP (not a stale DHCP lease). */
bool local_wifi_connected(void);
/* Cached CSV capacity indication; refreshed at mount, append, START and clear.
 * Reading this state performs no filesystem I/O during SWD programming. */
bool local_logs_full(void);

/* A read-only file snapshot holds a mutation lock, consumes bounded memory,
 * and supplies a SHA-256 ETag. 0=current CSV; 1/2=legacy; 3=prior UTC CSV. */
bool local_snapshot_begin(unsigned file,uint32_t *size,char etag[65]);
int local_snapshot_read(uint32_t offset,void *data,size_t size);
void local_snapshot_end(void);
/* 0=cleared, 1=stale ETag, -1=busy/storage failure. Only current CSV clears. */
int local_logs_clear(const char *etag);

/* Device network identity and received factory image metadata. */
void local_device_ssid(char out[33]);
bool local_factory_bin_available(void);
stored_image_t local_factory_bin_image(void);
uint32_t local_factory_bin_received_epoch(void);
bool local_factory_transfer_active(void);
