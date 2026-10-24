"""Embed the exact supplied image in Pico XIP flash; no target-side file system.

CRC-32/ISO-HDLC (zlib) detects accidental image corruption, not authenticity.
The manifest records SHA-256 for release traceability. No automatic padding or
trimming: every input byte must later be read back from the target and compared.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib


def generate(source: Path, output: Path):
    data = source.read_bytes()
    if len(data) < 8 or len(data) > 512 * 1024 or len(data) % 8:
        raise ValueError("Image must be 8-byte aligned in length and at most 512 KiB")
    sp, reset = struct.unpack_from("<II", data)
    if not (0x20000000 < sp <= 0x20018000 or 0x10000000 < sp <= 0x10008000) or sp % 8:
        raise ValueError("Initial SP is not an aligned STM32L476 SRAM address")
    if not reset & 1 or not 0x08000000 <= (reset & ~1) < 0x08000000 + len(data):
        raise ValueError("Reset vector is not Thumb code within an image linked at 0x08000000")
    crc = zlib.crc32(data)
    output.mkdir(parents=True, exist_ok=True)
    header = '''#pragma once
#include <stdint.h>
#include <stddef.h>
extern const uint8_t firmware_image[];
extern const size_t firmware_image_size;
extern const uint32_t firmware_image_crc32;
'''
    body = ['#include "firmware_image.h"',
            'const uint8_t firmware_image[] __attribute__((aligned(8))) = {']
    body += ['    ' + ','.join(f'0x{x:02x}' for x in data[i:i+16]) + ','
             for i in range(0, len(data), 16)]
    body += ['};', f'const size_t firmware_image_size = {len(data)}u;',
             f'const uint32_t firmware_image_crc32 = 0x{crc:08x}u;']
    (output / 'firmware_image.h').write_text(header, encoding='utf-8')
    (output / 'firmware_image.c').write_text('\n'.join(body) + '\n', encoding='utf-8')
    (output / 'firmware_manifest.json').write_text(json.dumps({
        'filename': source.name, 'bytes': len(data), 'base': '0x08000000',
        'crc32': f'{crc:08x}', 'sha256': hashlib.sha256(data).hexdigest()
    }, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    generate(args.input, args.output)
