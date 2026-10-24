#pragma once
#include <stdbool.h>
/* Core 0 only. This protocol exports logs; it cannot program or erase a target. */
void usb_export_poll(bool programmer_busy);
bool usb_export_active(void);
void usb_export_cancel(void);
