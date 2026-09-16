"""Execute the actual frost assembly; compare every channel to integer division."""
from pathlib import Path
import random
from piesim import Sim, extract_asm, store16, load16

root = Path(__file__).resolve().parents[2]
asm = extract_asm(root / 'main/ui/kasane/ksn_frost_kernel.h', 'ksn_frost_cell_pie(')
rng = random.Random(318)
for trial in range(4096):
    alpha = trial % 256
    k = [255-alpha, 1, 128, 3]
    for mask, factor in [(248, 2048), (252, 64), (255, 1)]:
        left, right = (rng.randrange(4081) for _ in range(2))
        if trial < 256:
            left, right = 0, 4080
        tint = rng.randrange(256)*alpha+127
        k += [left, right, tint, mask, factor]
    mem = bytearray([0xa5]*512)
    store16(mem, 0, list(range(15, 0, -2))+list(range(1, 16, 2)))
    store16(mem, 66, k)  # Deliberately not 16-byte aligned: broadcast is legal.
    before = bytes(mem)
    sim = Sim(mem)
    sim.run(asm, {'wp': 0, 'kp': 66, 'round': 70, 'dst': 256, 'n': 3, 'sar': 3, 'sh8': 8})
    for lane in range(8):
        pixel = 0
        for c in range(3):
            left, right, tint, mask, factor = k[4+5*c:9+5*c]
            v = (left*(15-2*lane)+right*(1+2*lane)+128)//256
            v = (v*(255-alpha)+tint)//255
            pixel |= ((v & mask)*factor)//8
        assert load16(mem, 256+lane*2, 1)[0] == pixel, (trial, lane)
    assert mem[:256] == before[:256] and mem[272:] == before[272:]
    assert sim.ar['kp'] == 66+38 and sim.ar['dst'] == 272
print('frost PIE assembly: PASS (4096 cells, all alpha, output guards)')
