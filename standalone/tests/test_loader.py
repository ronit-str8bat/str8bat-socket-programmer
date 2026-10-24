"""Execute compiled Cortex-M4 instructions against an independent flash model.

Checks FSTPG row ordering, EOP/errors, ECC preservation and SRAM bounds.
Does not certify silicon/electrical timing. Dependency: unicorn==2.1.4.
"""
import argparse
from pathlib import Path
import struct
import sys
import zlib

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', type=Path, default=Path('build-standalone'))
parser.add_argument('--firmware', type=Path, help='Also emulate every block of an actual production image')
parser.add_argument('--regression-loader', type=Path, help='Reproduce missing-EOPIE failure using the previous compiled loader')
parser.add_argument('--sequence-regression-loader', type=Path, help='Compare previous per-row FSTPG toggling')
args = parser.parse_args()
sys.path.insert(0, str(args.build_dir/'test-deps'))
from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS,
                     UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ, UC_HOOK_INTR)
from unicorn.arm_const import (UC_ARM_REG_SP, UC_ARM_REG_R0, UC_ARM_REG_XPSR,
                               UC_ARM_REG_PC, UC_ARM_REG_PRIMASK, UC_ARM_REG_R5, UC_ARM_REG_R2, UC_ARM_REG_R3)

code = (args.build_dir/'generated/target_loader.bin').read_bytes()
assert 0 < len(code) <= 2048
BASE, END, BUFFER, STACK = 0x08000000, 0x08080000, 0x20001000, 0x20006000
SR, CR, FSTPG = 0x40022010, 0x40022014, 1 << 18
EOPIE = 1 << 24
BLOCK, ROW = 16384, 256


def run(operation, address, data=None, size=None, bad_crc=False, dirty=None,
        flash_error=0, locked=False, missing_eop=False, stuck_busy=False,
        bad_clock=False, prior=None):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    uc.mem_map(0x20000000, 0x6000)
    uc.mem_map(BASE, END-BASE)
    uc.mem_write(0x20000000, code)
    uc.mem_write(BASE, prior[0] if prior else b'\xff'*(END-BASE))
    if dirty is not None:
        uc.mem_write(dirty, b'\x00')
    if data is not None:
        uc.mem_write(BUFFER, data)
    size = len(data) if size is None else size
    crc = zlib.crc32(data or b'') ^ int(bad_crc)
    uc.mem_write(0x20000800, struct.pack('<8I', operation, address, size, crc, 0, 0, 0, 0))
    uc.reg_write(UC_ARM_REG_SP, STACK)
    uc.reg_write(UC_ARM_REG_R0, 0x20000800)
    uc.reg_write(UC_ARM_REG_XPSR, 0x01000000)
    # Stale EOP must be cleared; success requires a new completion event.
    state = prior[1] if prior else {'sr': 1, 'cr': 0x80000000 if locked else 0, 'row': [],
             'busy_reads': 0, 'fast_rows': [], 'standard_pairs': 0,
             'fast_enables': 0, 'fast_disables': 0, 'ecc': set()}
    writes, ecc = [], state['ecc']

    def complete_write():
        state['busy_reads'] = 2
        state['sr'] = 0x10000

    def memory_read(u, access, a, n, value, user):
        if BASE <= a < END:
            assert not (state['cr'] & FSTPG), 'Flash read during fast programming'
        if a == SR:
            if state['busy_reads']:
                state['busy_reads'] -= 1
                if not state['busy_reads']:
                    # RM0351 FLASH_SR.EOP is generated only with EOPIE enabled.
                    # The old model omitted this gate and hid a real regression.
                    eop = int(bool(state['cr'] & EOPIE) and not missing_eop)
                    state['sr'] = 0x10000 if stuck_busy else flash_error or eop
            return state['sr']
        if a == CR:
            return state['cr']
        if a == 0x40021000: return 0x63 if bad_clock else 0x59b
        if a == 0x40007000: return 0x200
        return 0

    def memory_write(u, access, a, n, value, user):
        if BASE <= a < END:
            assert operation in (2, 3, 4) and n == 4 and address <= a < a+4 <= address+size
            assert bytes(u.mem_read(a, 4)) == b'\xff'*4, ('Double programming without erase', hex(a), len(writes), hex(u.reg_read(UC_ARM_REG_PC)), [hex(u.reg_read(r)) for r in (UC_ARM_REG_R5, UC_ARM_REG_R2, UC_ARM_REG_R3)])
            assert u.reg_read(UC_ARM_REG_PRIMASK) == 1, 'Loader interrupts must stay masked'
            mode = state['cr'] & ~EOPIE
            assert mode in (1, FSTPG), 'Invalid flash mode'
            if mode == FSTPG:
                assert operation == 3
                if len(state['row']) == 64:
                    assert a > state['row'][-1], 'Fast rows must ascend, including across calls'
                    assert not state['sr'], 'Previous row not completed/cleared'
                    state['row'] = []
                row = state['row']
                if not row:
                    assert a % ROW == 0
                else:
                    assert a == row[-1]+4 and (a & ~255) == row[0]
                row.append(a)
                if len(row) == 64:
                    state['fast_rows'].append(row[0])
                    complete_write()
            elif len(writes) % 2:
                assert a == writes[-1]+4
                state['standard_pairs'] += 1
                complete_write()
            else:
                assert a % 8 == 0
            if a % 8 == 0:
                assert a not in ecc, 'Double word ECC already programmed'
                ecc.add(a)
            writes.append(a)
        elif a == SR:
            assert value == 1, 'Only clear EOP; never hide a flash error'
            state['sr'] &= ~value
        elif a == CR:
            assert not (state['sr'] & 0x10000), 'Changed control register while busy'
            if value:
                assert not state['cr'] and value in (1, FSTPG, 1 | EOPIE, FSTPG | EOPIE)
                assert not state['sr'], 'Stale EOP/error before programming'
                if value & FSTPG: state['fast_enables'] += 1
            elif state['cr'] & FSTPG:
                assert len(state['row']) == 64
                state['row'] = []
                state['fast_disables'] += 1
            state['cr'] = value
        else:
            assert (0x20000800 <= a < a+n <= 0x20000820 or
                    0x20005000 <= a < a+n <= STACK or a == 0x40003000), hex(a)
            if a == 0x40003000:
                assert not (state['cr'] & FSTPG and 0 < len(state['row']) < 64), 'Watchdog call inside row'

    stopped = []

    def interrupt(u, number, user):
        pc = u.reg_read(UC_ARM_REG_PC)
        assert bytes(u.mem_read(pc, 2)) == b'\x00\xbe', (number, hex(pc))
        stopped.append(True)
        u.emu_stop()

    # Native MMIO callbacks avoid changing emulated memory from a read hook.
    # That invalidates Unicorn translation blocks/IT state and can skip the
    # instruction following an IT block, producing a false repeated-row write.
    for page in (0x40022000, 0x40021000, 0x40007000, 0x40003000):
        def rd(u,offset,n,user,base=page): return memory_read(u,0,base+offset,n,0,None)
        def wr(u,offset,n,value,user,base=page): memory_write(u,0,base+offset,n,value,None)
        uc.mmio_map(page,0x1000,rd,None,wr,None)
    uc.hook_add(UC_HOOK_MEM_READ, memory_read, begin=BASE, end=END-1)
    uc.hook_add(UC_HOOK_MEM_WRITE, memory_write, begin=BASE, end=END-1)
    uc.hook_add(UC_HOOK_MEM_WRITE, memory_write, begin=0x20000000, end=STACK-1)
    uc.hook_add(UC_HOOK_INTR, interrupt)
    uc.emu_start(0x20000001, 0x20000000+len(code), count=50000000)
    assert stopped, 'Loader failed to reach BKPT within the instruction budget'
    m = struct.unpack('<8I', uc.mem_read(0x20000800, 32))
    return m, writes, bytes(uc.mem_read(BASE, END-BASE)), state


m, w, _, _ = run(1, BASE, size=0x80000)
assert m[4] == 1 and m[7] == 0x80000 and not w
m, w, _, _ = run(1, BASE, size=0x80000, dirty=0x0807e800)
assert m[4] == 5 and m[6] == 0x0807e800 and not w

# Standard/sparse modes handle short tails; the fast pass deliberately skips them.
for op in (2, 4):
    for address in (0x080003f8, 0x080007f8, 0x0803fff8, 0x0807e7f8, 0x0807e800, 0x0807fff0):
        data = bytes(range(16))
        m, w, flash, s = run(op, address, data)
        assert m[4] == 1 and m[7] == 16 and w == list(range(address, address+16, 4))
        assert flash[address-BASE:address-BASE+16] == data and not s['fast_rows']

# Full 16 KiB transfers, across pages, the bank boundary and up to flash end.
data = bytes(range(256))*64
for address in (BASE, 0x0803e000, 0x0807c000):
    m, w, flash, s = run(3, address, data)
    assert m[4] == 1 and m[7] == BLOCK and len(w) == BLOCK//4
    assert s['fast_rows'] == list(range(address, address+BLOCK, ROW))
    assert s['standard_pairs'] == 0 and flash[address-BASE:address-BASE+BLOCK] == data
    assert s['fast_enables']==1 and s['fast_disables']==0 and s['cr']==FSTPG|EOPIE

def end_fast_pass(state):
    # Explicit host FLASH_CR=0 after all fast blocks, before any sparse write.
    assert not state['sr'] & 0x10000
    if state['cr'] & FSTPG: state['fast_disables'] += 1
    state['cr'] = 0
    state['row'] = []

# Session persists between separate loader calls/16 KiB uploads, even at banks.
m, w, flash, s = run(3, BASE+0x3c000, data)
m, w, flash, s = run(3, BASE+0x40000, data, prior=(flash,s))
assert m[4]==1 and len(s['fast_rows'])==128 and s['fast_enables']==1
assert s['fast_disables']==0
# PG is refused until host explicitly terminates the fast pass.
m, w, _, _ = run(4, BASE+0x44000, b'\x55'*8, prior=(flash,s))
assert m[4]==2 and not w
end_fast_pass(s)
m, w, flash, s = run(4, BASE+0x44000, b'\x55'*8, prior=(flash,s))
assert m[4]==1 and flash[0x44000:0x44008]==b'\x55'*8

# Dense -> sparse -> dense -> erased -> partial row. Preserve an erased double
# word inside the sparse row, including its untouched ECC bits.
mixed = bytes(range(256)) + b'\x55'*120 + b'\xff'*8 + b'\xaa'*128
mixed += bytes(range(256)) + b'\xff'*256 + b'\x33'*24
m, w, flash, s = run(3, BASE, mixed)
assert s['fast_enables']==1 and s['fast_disables']==0 and s['standard_pairs']==0
end_fast_pass(s)
m, sparse_w, flash, s = run(4, BASE, mixed, prior=(flash,s))
w += sparse_w
assert m[4] == 1 and m[7] == len(mixed) and flash[:len(mixed)] == mixed
assert s['fast_rows'] == [BASE, BASE+512] and s['standard_pairs'] == 34
assert BASE+376 not in w and all(BASE+768+i not in w for i in range(0, 256, 4))

# The earlier configuration page stays physically erased inside a larger block.
mixed = bytearray(data)
mixed[0x2800:0x3000] = b'\xff'*2048
m, w, flash, s = run(3, 0x0807c000, bytes(mixed))
assert m[4] == 1 and flash[0x7e800:0x7f000] == b'\xff'*2048
assert not any(0x0807e800 <= a < 0x0807f000 for a in w)
assert len(s['fast_rows']) == 56

# Standard-only mode remains available without executing FSTPG.
m, w, _, s = run(2, BASE, data)
assert m[4] == 1 and m[7] == BLOCK and not s['fast_rows'] and s['standard_pairs'] == BLOCK//8
for op in (2, 3):
    m, w, flash, s = run(op, BASE, b'\xff'*BLOCK)
    assert m[4] == 1 and not w and flash == b'\xff'*0x80000
    m, w, _, _ = run(op, BASE, data, bad_crc=True)
    assert m[4] == 6 and not w
    m, w, _, _ = run(op, BASE, data, locked=True)
    assert m[4] == 4 and not w
    for error in (0x80, 0x300, 0x10):  # sequence, fast data miss, write protection
        m, w, _, _ = run(op, BASE, data, flash_error=error)
        assert m[4] == 4 and m[5] == error and m[7] == 0 and m[6] == BASE
        assert len(w) == (64 if op == 3 else 2), 'Wrote beyond failed operation'
    m, w, _, _ = run(op, BASE, data, missing_eop=True)
    if op == 3:
        assert m[4] == 7 and m[7] == 0
    else:
        # Standard polling accepts BSY-clear/no errors without EOP. The host
        # still requires the full byte comparison before displaying green.
        assert m[4] == 1 and m[7] == BLOCK and len(w)==BLOCK//4
    for address, size in ((BASE+4, 8), (END-8, 16), (BASE, 7), (BASE, BLOCK+8)):
        m, w, _, _ = run(op, address, b'\x55'*min(size, BLOCK+8), size=size)
        assert m[4] == 2 and not w
m, w, _, _ = run(3, BASE, data, bad_clock=True)
assert m[4] == 8 and not w
m, w, _, s = run(3, BASE, data, stuck_busy=True)
assert m[4] == 3 and m[7] == 0 and len(w) == 64 and s['cr'] == FSTPG | EOPIE
m, w, _, _ = run(99, BASE, data)
assert m[4] == 2 and not w
print('Compiled ARM loader: 16 KiB blocks, fast rows, sparse/ECC preservation, boundaries, CRC, EOP, clock and error/timeout tests passed')

if args.firmware:
    image = args.firmware.read_bytes()
    assert 8 <= len(image) <= END-BASE and len(image) % 8 == 0
    prior, uploads, uploaded_bytes = None, 0, 0
    for op in (3,4):
        for offset in range(0, len(image), BLOCK):
            block = image[offset:offset+BLOCK]
            dense = lambda row: len(row)==ROW and all(row[i:i+8]!=b'\xff'*8 for i in range(0,ROW,8))
            rows = [block[i:i+ROW] for i in range(0,len(block),ROW)]
            needed = any(dense(r) for r in rows) if op==3 else any(not dense(r) and r!=b'\xff'*len(r) for r in rows)
            if not needed: continue
            first=0
            if op==4:
                indices=[i for i,r in enumerate(rows) if not dense(r) and r!=b'\xff'*len(r)]
                first=indices[0]*ROW
                block=block[first:min((indices[-1]+1)*ROW,len(block))]
            m, w, flash, s = run(op, BASE+offset+first, block, prior=prior)
            assert m[4] == 1 and m[7] == len(block)
            prior = (flash,s)
            uploads += 1
            uploaded_bytes += len(block)
        if op==3:
            assert s['fast_enables']==1 and s['fast_disables']==0
            end_fast_pass(s)
    assert flash[:len(image)]==image
    for i in range(0,len(image),8):
        if image[i:i+8]==b'\xff'*8: assert BASE+i not in s['ecc']
    print(f"Actual production image matched: {uploads} uploads / {uploaded_bytes} bytes, {len(s['fast_rows'])} fast rows, {s['standard_pairs']} standard double words; erased ECC preserved")

if args.sequence_regression_loader:
    current_code = code
    code = args.sequence_regression_loader.read_bytes()
    m, _, _, s = run(3, BASE, bytes(range(256))*2)
    assert m[4]==1 and s['fast_enables']==2 and s['fast_disables']==2
    code = current_code
    m, _, _, s = run(3, BASE, bytes(range(256))*2)
    assert m[4]==1 and s['fast_enables']==1 and s['fast_disables']==0
    print('Compiled old/new sequence comparison: per-row FSTPG toggling removed')

if args.regression_loader:
    code = args.regression_loader.read_bytes()
    for op in (2, 3):
        m, w, flash, _ = run(op, BASE, bytes(range(256)))
        assert m[4] == 7 and m[5] == 0 and m[7] == 0
        assert len(w) == (2 if op == 2 else 64)
        assert flash[:len(w)*4] == bytes(range(256))[:len(w)*4]
    print('Previous compiled loader reproduces false failure after successful standard/fast writes when EOPIE is absent')
