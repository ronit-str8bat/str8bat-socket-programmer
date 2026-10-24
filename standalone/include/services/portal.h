#pragma once
#include <stdbool.h>
bool portal_start(void);
void portal_poll(bool programmer_busy);
bool portal_reboot_pending(void);
/* START takes priority; a partial download is closed without deleting logs. */
void portal_cancel_download(void);
/* True from a committed master BIN until all three paired slaves report. */
bool portal_factory_transfer_active(void);
