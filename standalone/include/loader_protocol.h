#pragma once
#include <stdint.h>

/* Fixed, non-overlapping SRAM1 regions on STM32L476. Not target flash. */
#define LOADER_CODE_ADDRESS 0x20000000u
#define LOADER_CODE_LIMIT   0x800u
#define LOADER_MAILBOX      0x20000800u
#define LOADER_BUFFER       0x20001000u
#define LOADER_BUFFER_SIZE  16384u
#define LOADER_STACK_BOTTOM 0x20005000u
#define LOADER_STACK_TOP    0x20006000u
#define LOADER_FAST_ROW_SIZE 256u
#define LOADER_PENDING 0u
#define LOADER_SUCCESS 1u
#define LOADER_INVALID 2u
#define LOADER_TIMEOUT 3u
#define LOADER_FLASH_ERROR 4u
#define LOADER_NOT_BLANK 5u
#define LOADER_BAD_DATA 6u
#define LOADER_NO_EOP 7u
#define LOADER_BAD_CLOCK 8u
#define LOADER_BLANK_CHECK 1u
#define LOADER_PROGRAM 2u
#define LOADER_PROGRAM_FAST 3u /* Dense rows only; keep FSTPG active across calls. */
#define LOADER_PROGRAM_SPARSE 4u /* Remaining sparse rows/tails; fast pass is finished. */
_Static_assert(LOADER_CODE_ADDRESS + LOADER_CODE_LIMIT <= LOADER_MAILBOX,
               "Loader overlaps mailbox");
_Static_assert(LOADER_MAILBOX + 32u <= LOADER_BUFFER,
               "Mailbox overlaps buffer");
_Static_assert(LOADER_BUFFER + LOADER_BUFFER_SIZE <= LOADER_STACK_BOTTOM,
               "Transfer buffer overlaps target stack");
_Static_assert(LOADER_STACK_TOP <= 0x20018000u,
               "Target SRAM1 overflow");
typedef struct {
    uint32_t operation, address, size, crc32;
    uint32_t status, detail, failure_address, completed;
} loader_mailbox_t;
_Static_assert(sizeof(loader_mailbox_t) == 32, "Mailbox ABI changed");
