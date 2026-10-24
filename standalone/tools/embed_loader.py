"""Embed a separately compiled Cortex-M4 RAM algorithm into the Pico image."""
from pathlib import Path
import sys
data = Path(sys.argv[1]).read_bytes()
if not 0 < len(data) <= 2048:
    raise ValueError('RAM loader exceeds its reserved SRAM region')
data += bytes((-len(data)) % 4)
words = [int.from_bytes(data[i:i+4], 'little') for i in range(0, len(data), 4)]
Path(sys.argv[2]).write_text('#pragma once\n#include <stdint.h>\n'
    'static const uint32_t target_loader[] = {\n' +
    ',\n'.join(f'0x{x:08x}u' for x in words) + '\n};\n', encoding='utf-8')
