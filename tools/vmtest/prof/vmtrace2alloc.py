"""Convert a vmrun --trace allocator log ('+ id size', '- id', '~ old new size')
to alloc_sim.py's format, with the device's tlsf rounding (4 B, 12 B min).
    python3 vmtrace2alloc.py VMRUN_TRACE > ALLOC_TRACE"""
import sys
u = lambda n: max(12, (int(n) + 3) & ~3)
print('F 1')
for line in open(sys.argv[1]):
    p = line.split()
    if not p or p[0] == '#': continue
    if p[0] == '+': print(f'm {u(p[2])} {p[1]}')
    elif p[0] == '-': print(f'f {p[1]}')
    elif p[0] == '~': print(f'r {p[1]} {p[2]} {u(p[3])}')
