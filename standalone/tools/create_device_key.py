"""Create once, never print or overwrite the fixture credential encryption key."""
from pathlib import Path
import secrets
import sys

path = Path(sys.argv[1])
path.parent.mkdir(parents=True, exist_ok=True)
if not path.exists():
    key = secrets.token_bytes(32)
    # Exclusive create handles concurrent build configurations safely.
    try:
        with path.open('x', encoding='ascii') as stream:
            stream.write('#pragma once\n#include <stdint.h>\n')
            stream.write('static const uint8_t device_storage_key[32] = {\n')
            stream.write(','.join(f'0x{b:02x}' for b in key) + '\n};\n')
    except FileExistsError:
        pass
