"""Run host C unit tests; no Pico/target hardware access."""
from pathlib import Path
import argparse
import importlib.util
import json
import os
import struct
import subprocess
import tempfile
import zlib

parser = argparse.ArgumentParser()
parser.add_argument('--cc', default='gcc')
parser.add_argument('--firmware', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
environment = dict(os.environ)
environment['PATH'] = str(Path(args.cc).resolve().parent) + os.pathsep + environment.get('PATH', '')
with tempfile.TemporaryDirectory() as tmp:
    for name, sources in {
        'logic': ['ui_logic.c', 'crc32.c'],
        'programmer': ['stm32_programmer.c', 'crc32.c'],
        'programmer_standard': ['stm32_programmer.c', 'crc32.c'],
        'programmer_halt': ['stm32_programmer.c', 'crc32.c'],
        'programmer_standard_halt': ['stm32_programmer.c', 'crc32.c'],
        'swd': ['swd.c'],
        'swd_pio': ['swd.c'],
    }.items():
        exe = Path(tmp) / (name + '.exe')
        subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
            *(['-DSWD_USE_PIO=1'] if name == 'swd_pio' else []),
            *(['-DSTM32_USE_FAST_PROGRAM=0'] if 'standard' in name else []),
            *(['-DSTM32_HALT_AFTER_VERIFY=1'] if name.endswith('_halt') else []),
            '-I', str(root/'include'), '-I', str(root/'tests/stubs'),
            str(root/f'tests/test_{"swd" if name == "swd_pio" else "programmer" if name.startswith("programmer") else name}.c'),
            *(str(root/'src'/s) for s in sources), '-o', str(exe)], check=True, env=environment)
        subprocess.run([str(exe), *([str(args.firmware.resolve())] if args.firmware and name.startswith('programmer') else [])], check=True, env=environment)
    spec = importlib.util.spec_from_file_location('embed', root/'tools/embed_firmware.py')
    embed = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(embed)
    binary = Path(tmp)/'test.bin'
    image = struct.pack('<II', 0x20000460, 0x08000009) + bytes(range(8))
    binary.write_bytes(image)
    embed.generate(binary, Path(tmp)/'generated')
    manifest = json.loads((Path(tmp)/'generated/firmware_manifest.json').read_text())
    assert manifest['bytes'] == 16 and manifest['crc32'] == f'{zlib.crc32(image):08x}'
    for invalid in [b'', image[:-1], bytes(16), image + bytes(512*1024)]:
        binary.write_bytes(invalid)
        try:
            embed.generate(binary, Path(tmp)/'generated')
        except ValueError:
            pass
        else:
            raise AssertionError('Invalid image accepted')
    print('Embedding validation tests passed')
