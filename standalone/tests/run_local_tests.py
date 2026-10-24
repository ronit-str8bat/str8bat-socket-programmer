"""Host regression tests for the local-only hotspot profile."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cc = r'C:\msys64\mingw64\bin\gcc.exe'
env = dict(os.environ)
env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH', '')
with tempfile.TemporaryDirectory() as tmp:
    for name, sources in {
        'clock': [],
        'storage': ['src/services/local_clock.c', 'src/crc32.c', 'third_party/littlefs/lfs.c', 'third_party/littlefs/lfs_util.c'],
        'portal': ['src/services/local_clock.c'],
        'network': ['src/services/local_clock.c'],
    }.items():
        exe = Path(tmp) / f'{name}.exe'
        cmd = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
               '-DLFS_NO_DEBUG', '-DLFS_NO_WARN', '-DLFS_NO_ERROR', '-DSTANDALONE_LOCAL_AP=1',
               '-I', str(root/'tests/network_stubs'), '-I', str(root/'include'),
               '-I', str(root/'third_party/littlefs'), '-I', str(root/'third_party/pico_ap'),
               str(root/f'tests/test_local_{name}.c'), str(root/'tests/mock_powman.c'),
               *(str(root/s) for s in sources), '-o', str(exe)]
        if name == 'storage': cmd += ['-lcrypto']
        subprocess.run(cmd, check=True, env=env)
        subprocess.run([str(exe)], check=True, env=env)
