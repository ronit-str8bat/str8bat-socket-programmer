#pragma once
#include <stdbool.h>
#include "services/storage.h"
bool services_init(void);
void services_poll(bool programmer_busy);
bool services_job_begin(void);
bool services_job_finish(const programmer_report_t *report);
const char *services_status(void);
void services_set_ntp_time(unsigned long seconds);
bool services_export_begin(uint32_t *size);
/* Factory-role helpers. Standard/local builds return false or the embedded
 * image and therefore preserve the existing button-driven behaviour. */
bool services_factory_led_active(void);
bool services_factory_take_program_job(void);
stored_image_t services_program_image(void);
