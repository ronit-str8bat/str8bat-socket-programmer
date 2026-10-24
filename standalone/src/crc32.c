#include "crc32.h"
#include "crc32_nibble.h"

uint32_t image_crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffu;
    while (size--) {
        crc = crc32_update_byte(crc, *data++);
    }
    return ~crc;
}
