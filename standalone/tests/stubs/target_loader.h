#pragma once
#include <stdint.h>
/* Host orchestration tests model the target; real ARM code is built separately. */
static const uint32_t target_loader[] = {0xe7febe00u};
