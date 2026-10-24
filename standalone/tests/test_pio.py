"""Small instruction model for the assembled PIO bit engine (not silicon timing)."""
from pathlib import Path
import argparse
import re
parser=argparse.ArgumentParser()
parser.add_argument('--build-dir',type=Path,default=Path('build-standalone'))
args=parser.parse_args()
header=(args.build_dir/'swd_wire.pio.h').read_text()
text=header.split('swd_wire_program_instructions[] = {')[1].split('};')[0]
program=[int(h,16) for h in re.findall(r'0x([0-9a-f]{4}),',text)]
wrap=int(re.search(r'#define swd_wire_wrap (\d+)',header)[1])

def exchange(read,count,data):
    tx=[count-1+(32 if read else 0)]+([] if read else [data])
    pc=x=y=osr=isr=pin=clock=cycles=0
    incoming=[(data>>i)&1 for i in range(count)]
    received=[]; edges=[]
    for _ in range(200):
        ins=program[pc]; op=ins>>13; side=(ins>>12)&1; delay=(ins>>8)&15
        arg=(ins>>5)&7; n=ins&31; next_pc=0 if pc==wrap else pc+1
        # IN captures the old target value at this edge; target advances afterward.
        target=incoming[0] if incoming else 0
        if op==0: # JMP
            jump=arg==0 or (arg==3 and y==0) or (arg==2 and x!=0)
            if arg==2: x=(x-1)&0xffffffff
            if jump: next_pc=n
        elif op==2: # IN pins
            assert arg==0 and n==1 and read
            isr=(isr>>1)|(target<<31)
        elif op==3: # OUT
            value=osr&((1<<n)-1); osr>>=n
            if arg==0: pin=value
            elif arg==1: x=value
            elif arg==2: y=value
            else: raise AssertionError(arg)
        elif op==4: # PUSH/PULL
            if ins&0x80: osr=tx.pop(0)
            else:
                assert side==0 and clock==0
                assert len(edges)==count*2
                for i in range(1,len(edges)):
                    assert edges[i][0]-edges[i-1][0]>=2
                result=isr if count==32 else isr>>(32-count)
                assert result==(data&((1<<count)-1) if read else 0)
                if not read: assert received==[(data>>i)&1 for i in range(count)]
                return
        elif op==5: # MOV ISR,NULL
            assert arg==6 and n==3; isr=0
        else: raise AssertionError(hex(ins))
        if side!=clock:
            edges.append((cycles,side))
            if side:
                if read: incoming.pop(0)
                else: received.append(pin)
        clock=side; cycles+=1+delay; pc=next_pc
    raise AssertionError('PIO did not complete')

for read in (False,True):
    for count in (1,2,3,8,16,31,32):
        for data in (0,0xffffffff,0x2ba01477,0xa5a5a5a5): exchange(read,count,data)
print('Assembled PIO: LSB order, 1..32-bit transfers, target-edge sampling, phase lengths and low-clock completion passed')
