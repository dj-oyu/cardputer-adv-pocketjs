"""Execute the actual fill asm in piesim; C tests cover dispatch/head/tail."""
from pathlib import Path
import sys

root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tools/pie'))
from piesim import Sim, extract_asm, store16, load16

asm=extract_asm(str(root/'main/ui/kasane/ksn_render.c'),'fill_blocks')
for blocks in (1,2,3,29,30,210,240):
    for color in (0,1,0x8000,0xffff,0x1234,0x07e0,0x001f):
        mem=bytearray([0x5a])*8192
        start=256
        store16(mem,start,[color])
        sim=Sim(mem)
        sim.run(asm,{'dst':start,'blocks':blocks})
        assert sim.ar['dst']==start+16*blocks
        assert mem[:start]==bytes([0x5a])*start
        assert mem[start+16*blocks:]==bytes([0x5a])*(len(mem)-start-16*blocks)
        assert load16(mem,start,blocks*8)==[color]*(blocks*8)
print('RGB565 fill PIE assembly: PASS (full strips, RGB565 lanes, guards)')
