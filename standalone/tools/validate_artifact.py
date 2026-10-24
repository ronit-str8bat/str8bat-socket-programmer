"""Check the generated UF2 payload and exact embedded production/loader bytes.

Read-only validation of firmware artifacts; never connects to either board.
Writes uf2-validation.json only after all comparisons pass.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', type=Path, default=Path('build-standalone'))
parser.add_argument('--firmware', type=Path, required=True)
args = parser.parse_args()
build = args.build_dir
image = args.firmware.read_bytes()
binary = (build/'standalone_programmer.bin').read_bytes()
uf2 = (build/'standalone_programmer.uf2').read_bytes()
loader = (build/'generated/target_loader.bin').read_bytes()
assert len(uf2) % 512 == 0 and len(image) == 524288
blocks = {}
for offset in range(0, len(uf2), 512):
    block = uf2[offset:offset+512]
    magic0, magic1, flags, address, length, number, total, family = struct.unpack_from('<8I', block)
    assert (magic0, magic1) == (0x0a324655, 0x9e5d5157)
    assert struct.unpack_from('<I', block, 508)[0] == 0x0ab16f30
    assert length <= 476 and number < total
    if family != 0xe48bff59:  # RP2350 Arm secure image; ignore compatibility blocks.
        continue
    assert flags & 0x2000 and address == 0x10000000+number*256 and length == 256
    assert number not in blocks
    blocks[number] = block[32:32+length]
    expected_blocks = total
assert len(blocks) == expected_blocks and set(blocks) == set(range(expected_blocks))
payload = b''.join(blocks[n] for n in range(expected_blocks))
assert payload[:len(binary)] == binary and len(payload)-len(binary) < 256
assert binary.count(image) == 1, 'Production image missing, modified or duplicated'
assert binary.count(loader) == 1, 'Compiled loader missing from Pico image'
generated = (build/'generated/firmware_image.c').read_text().split('{', 1)[1].split('}', 1)[0]
assert bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})\b', generated)) == image
manifest = json.loads((build/'generated/firmware_manifest.json').read_text())
assert manifest['bytes'] == len(image) and manifest['crc32'] == f'{zlib.crc32(image):08x}'
assert 'STANDALONE_FAST_PROGRAM:BOOL=ON' in (build/'CMakeCache.txt').read_text()
assert 'STANDALONE_SWD_PIO:BOOL=ON' in (build/'CMakeCache.txt').read_text()
sha = lambda data: hashlib.sha256(data).hexdigest()
halt = 'STANDALONE_HALT_AFTER_VERIFY:BOOL=ON' in (build/'CMakeCache.txt').read_text()
network = 'STANDALONE_NETWORK:BOOL=ON' in (build/'CMakeCache.txt').read_text()
if network:
    report_id = re.search(rb'PICO-PROGRAMMER (network-v[0-9]+(?:-[a-z]+)?|local-ap-v[0-9]+) result=', binary)
    assert report_id, 'Missing network build report identifier'
    build_id = report_id.group(1).decode('ascii')
else:
    build_id = 'fast-v3-halt' if halt else 'fast-v3'
if network:
    assert len(payload) <= 0x180000, 'UF2 writes into reserved persistent storage'
    link_map = (build/'standalone_programmer.elf.map').read_text()
    assert re.search(r'0x2007c000\s+__HeapLimit =', link_map)
    assert re.search(r'0x2007c000\s+__StackBottom =', link_map)
    assert re.search(r'0x20080000\s+__StackTop =', link_map)
    assert re.search(r'0x20081000\s+__StackOneTop =', link_map)
assert ('PICO-PROGRAMMER '+build_id+' result=').encode() in binary
record = {
    'uf2_sha256': sha(uf2), 'uf2_bytes': len(uf2), 'pico_binary_bytes': len(binary),
    'rp2350_payload_matches_build_bin': True,
    'embedded_stm32_bytes': len(image), 'embedded_stm32_sha256': sha(image),
    'embedded_stm32_matches_source': True, 'ram_loader_bytes': len(loader),
    'ram_loader_sha256': sha(loader), 'transport': 'PIO',
    'program_clock_request_khz': 2000, 'minimum_half_period_ns': 250,
    'ram_transfer_block_bytes': 16384, 'fast_programming_enabled': True,
    'fast_row_bytes': 256, 'sparse_rows_use_standard_doublewords': True,
    'build_report_id': build_id,
    'halt_after_verify': halt,
    'crc_algorithm': 'CRC-32/ISO-HDLC with 16-entry nibble table',
    'sparse_uploads': 'Leading/trailing irrelevant rows trimmed; internal dense rows skipped',
    'verify_chunk_bytes': 1024,
    'fast_sequence': 'One continuous FSTPG pass across dense rows and blocks; separate sparse PG pass',
    'all_ff_doublewords_preserve_erased_ecc': True,
    'completion_check': 'Fast: fresh EOP with EOPIE; standard: BSY/error polling; both: full byte verification',
    'recovery': 'One reset, full erase, blank check and standard-only retry after fast flash/NO_EOP errors',
    'usb_diagnostic_report': 'STANDALONE_USB_DIAGNOSTICS:BOOL=ON' in (build/'CMakeCache.txt').read_text(),
    'hardware_qualified': False,
    'network_enabled': network,
    'local_hotspot_only': 'STANDALONE_LOCAL_AP:BOOL=ON' in (build/'CMakeCache.txt').read_text(),
    'clock_initialization': 'HSI transition to MSI 24 MHz with readiness checks',
}
(build/'uf2-validation.json').write_text(json.dumps(record, indent=2)+'\n')
print(json.dumps(record, indent=2))
