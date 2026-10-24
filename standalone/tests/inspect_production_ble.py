"""Inspect BLE staging cleanup in the exact supplied 4.0.2 product binary.

Executes actual Thumb instructions; external BLE/I/O and flash HAL calls are
stubbed. This proves conditional erase intent, not what happened on a live board.
No programmer image or hardware is changed.
"""
from pathlib import Path
import argparse
import hashlib
import struct
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--image', type=Path, default=Path(r'D:\shashank\Firmware_bin\production_firmware_v4.0.2.bin'))
args = parser.parse_args()
sys.path.insert(0, str(Path('build-standalone/test-deps').resolve()))
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_LR,
                               UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_XPSR)

BASE, START, END = 0x08000000, 0x08040000, 0x0807e800
image = args.image.read_bytes()
assert hashlib.sha256(image).hexdigest() == '3d656a56028eb7cda59d7ec13c2a07275e3ef281848754afe16ca96d6eca98b9'
assert image[START-BASE:START-BASE+8] == b'\xab'*8
last_data = max(i for i in range(START-BASE, END-BASE) if image[i] != 0xff)
assert last_data + BASE == 0x0806c7be

def inspect(ble_result, signature=True):
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    u.mem_map(BASE, len(image)); u.mem_write(BASE, image)
    if not signature: u.mem_write(START, b'\xff'*8)
    u.mem_map(0x20000000, 0x20000)
    u.mem_map(0x1fff7000, 0x1000)
    u.mem_write(0x1fff75e0, struct.pack('<H',512))
    u.mem_map(0x40010000, 0x1000)  # SYSCFG bank swap disabled.
    u.reg_write(UC_ARM_REG_SP, 0x20018000)
    u.reg_write(UC_ARM_REG_LR, 0x20000101)
    u.reg_write(UC_ARM_REG_XPSR, 0x01000000)
    erases, events, returned = [], [], []
    def hook(uc, pc, size, user):
        if pc == 0x20000100:
            returned.append(True); uc.emu_stop(); return
        if pc == 0x0802b220:  # HAL_FLASHEx_Erase
            p = uc.reg_read(UC_ARM_REG_R0)
            request = struct.unpack('<4I', uc.mem_read(p,16))
            erases.append(request)
            assert request == (0,2,0,125), request  # page erase, bank 2, page 0, 125 pages
            uc.mem_write(START, b'\xff'*(END-START))
            uc.mem_write(uc.reg_read(UC_ARM_REG_R1), struct.pack('<I',0xffffffff))
            value = 0
        elif pc == 0x0802e03a:  # External BLE bootloader update
            events.append('BLE update'); value = ble_result
        elif pc == 0x080134de:
            events.append('BLE failure flag'); value = 0
        elif pc in (0x0800e370, 0x0802de54, 0x0802e222, 0x08012270,
                     0x0802ba22, 0x0801440e, 0x0802ba3c):
            # Delay, BLE run/reset operations, tick, flash unlock/flags/lock.
            value = 0
        else:
            return
        uc.reg_write(UC_ARM_REG_R0, value)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    u.hook_add(UC_HOOK_CODE, hook)
    u.emu_start(0x08014d71, 0, count=10000)
    assert returned
    return erases, events, bytes(u.mem_read(BASE,len(image)))

erases, events, flash = inspect(0)
assert erases == [(0,2,0,125)] and events == ['BLE update']
assert flash[START-BASE:END-BASE] == b'\xff'*(END-START)
assert flash[:START-BASE] == image[:START-BASE] and flash[END-BASE:] == image[END-BASE:]
diff = [BASE+i for i,(a,b) in enumerate(zip(image,flash)) if a != b]
assert min(diff) == START and max(diff) == 0x0806c7be
print(f'BLE success: erases 0x{START:08X}..0x{END-1:08X}; simulated differences 0x{min(diff):08X}..0x{max(diff):08X}')
erases, events, flash = inspect(1)
assert not erases and events == ['BLE update','BLE failure flag'] and flash == image
print('BLE failure: no staging-area erase')
erases, events, flash = inspect(0,signature=False)
assert not erases and not events
print('Absent staging signature: no BLE update or staging-area erase')
