#pragma once
#include <stdint.h>

/* Reflected CRC-32/ISO-HDLC, four bits per step. On the target this table is
 * linked with the loader into SRAM: never read STM32 flash during FSTPG.
 * Initial/final XOR and polynomial are unchanged from the bitwise algorithm.
 */
static inline uint32_t crc32_update_byte(uint32_t crc, uint8_t byte) {
    static const uint32_t table[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
        0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu
    };
    crc ^= byte;
    crc = (crc >> 4) ^ table[crc & 15u];
    return (crc >> 4) ^ table[crc & 15u];
}
