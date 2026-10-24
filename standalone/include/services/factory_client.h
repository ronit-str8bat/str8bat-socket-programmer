#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Slave-side station client for the local factory master. All calls run on
 * core 0 from the normal network poll path. */
void factory_client_init(const char *pico_id);
void factory_client_poll(bool programmer_busy);
bool factory_client_master_connected(void);
bool factory_client_receiving(void);
bool factory_client_take_program_job(void);
void factory_client_program_result(unsigned result);

