"""Host tests of actual parsers/storage/LittleFS; requires host GCC + OpenSSL."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--cc', default=r'C:\msys64\mingw64\bin\gcc.exe')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
env = dict(os.environ)
env['PATH'] = str(Path(a.cc).resolve().parent) + os.pathsep + env.get('PATH', '')
with tempfile.TemporaryDirectory() as tmp:
    for name, sources in {
        'protocol': ['src/services/protocol.c'],
        'storage': ['src/crc32.c', 'third_party/littlefs/lfs.c', 'third_party/littlefs/lfs_util.c'],
        'portal': ['src/services/protocol.c'],
    }.items():
        exe = Path(tmp) / f'{name}.exe'
        cmd = [a.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
               '-DLFS_NO_DEBUG', '-DLFS_NO_WARN', '-DLFS_NO_ERROR',
               '-I', str(root/'tests/network_stubs'), '-I', str(root/'include'),
               '-I', str(root/'third_party/littlefs'), str(root/f'tests/test_network_{name}.c'),
               '-I', str(root/'third_party/pico_ap'),
               *(str(root/s) for s in sources), '-o', str(exe)]
        if name == 'storage':
            cmd += ['-lcrypto']
        subprocess.run(cmd, check=True, env=env)
        subprocess.run([str(exe)], check=True, env=env)
