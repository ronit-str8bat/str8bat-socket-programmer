"""Generate version.json for the strict HTTPS target-image OTA contract."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from urllib.parse import urlsplit

p = argparse.ArgumentParser()
p.add_argument('--binary', type=Path, required=True)
p.add_argument('--url', required=True, help='Public HTTPS binary URL on the manifest host')
p.add_argument('--revision', type=int, required=True, help='Monotonically increasing release number, >40002 initially')
p.add_argument('--output', type=Path, default=Path('version.json'))
a = p.parse_args()
data = a.binary.read_bytes()
if not 8 <= len(data) <= 524288 or len(data) % 8:
    p.error('Binary must contain 8..524288 bytes and be a multiple of 8 bytes')
sp, pc = struct.unpack_from('<II', data)
if sp % 8 or not (0x20000000 < sp <= 0x20018000 or 0x10000000 < sp <= 0x10008000) or not pc & 1 or not 0x08000000 <= (pc & ~1) < 0x08000000 + len(data):
    p.error('Invalid STM32 initial stack/reset vector')
url = urlsplit(a.url)
if url.scheme != 'https' or not url.hostname or not url.path or url.port not in (None, 443) or url.username or url.fragment:
    p.error('Use an HTTPS URL on port 443, without user info or a fragment')
if ':' in url.netloc or len(url.hostname) >= 128 or len(url.path + ('?' + url.query if url.query else '')) >= 384:
    p.error('Firmware expects a DNS hostname without explicit port, and a path under 384 characters')
if not 0 < a.revision <= 0xffffffff:
    p.error('revision must be a positive uint32')
manifest = {'revision': a.revision, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
            'url': a.url, 'target': 'STM32L476JE', 'address': '0x08000000'}
a.output.write_text(json.dumps(manifest, indent=2) + '\n', encoding='ascii')
print(f'Wrote {a.output}: {len(data)} bytes, revision {a.revision}, SHA-256 {manifest["sha256"]}')
