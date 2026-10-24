"""Validate the mode-initializer behavior in this exact supplied production BIN.

Runs its actual Thumb instructions in Unicorn, stubbing flash-driver calls.
This is analysis of the product binary, not a hardware programming test.
Addresses are specific to the SHA-256 below and must not be reused for another build.
"""
from pathlib import Path
import argparse
import hashlib
import struct
import sys
parser=argparse.ArgumentParser()
parser.add_argument('--image',type=Path,default=Path(r'D:\shashank\Firmware_bin\production_firmware_v4.0.2.bin'))
args=parser.parse_args()
sys.path.insert(0,str(Path('build-standalone/test-deps').resolve()))
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_XPSR
image=args.image.read_bytes()
assert hashlib.sha256(image).hexdigest()=='3d656a56028eb7cda59d7ec13c2a07275e3ef281848754afe16ca96d6eca98b9'
assert image[0x7e800:0x7e808]==b'\xff'*8

def inspect(initial):
    u=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS)
    u.mem_map(0x08000000,0x80000); u.mem_write(0x08000000,image)
    u.mem_write(0x0807e800,initial)
    u.mem_map(0x20000000,0x20000)
    u.reg_write(UC_ARM_REG_SP,0x20018000)
    u.reg_write(UC_ARM_REG_LR,0x20000101)
    u.reg_write(UC_ARM_REG_XPSR,0x01000000)
    calls=[]; returned=[]
    def hook(uc,pc,size,user):
        if pc==0x20000100:
            returned.append(True); uc.emu_stop(); return
        if pc in (0x0802ba22,0x0801440e,0x0802ba3c):
            # Unlock, clear flash flags and lock: model as successful driver calls.
            uc.reg_write(UC_ARM_REG_R0,0)
            uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR)); return
        if pc==0x0802b98c:
            kind=uc.reg_read(UC_ARM_REG_R0); address=uc.reg_read(UC_ARM_REG_R1)
            lo=uc.reg_read(UC_ARM_REG_R2); hi=uc.reg_read(UC_ARM_REG_R3)
            calls.append((kind,address,lo,hi))
            assert kind==0 and address==0x0807e800
            uc.mem_write(address,struct.pack('<II',lo,hi))
            uc.reg_write(UC_ARM_REG_R0,0)
            uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
    u.hook_add(UC_HOOK_CODE,hook)
    u.emu_start(0x0801204d,0,count=2000)
    assert returned
    return calls,bytes(u.mem_read(0x0807e800,8))

calls,result=inspect(b'\xff'*8)
assert calls==[(0,0x0807e800,0,0)] and result==bytes(8)
print('Erased mode field: initializer calls flash driver with address 0x0807E800 and data 0x0000000000000000')
calls,result=inspect(bytes(8))
assert not calls and result==bytes(8)
print('Already-initialized zero field: no additional programming call')
