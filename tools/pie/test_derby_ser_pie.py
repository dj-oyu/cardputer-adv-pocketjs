"""Run the asm of main/pocket/derby_ser_pie.c (dsp_rotate_pie, dsp_cull_pie)
through piesim.py on the same vectors the device self-test uses, and compare
every output bit with the scalar reference in the same file.

    python tools/pie/test_derby_ser_pie.py          512 cases (the device's count)
    python tools/pie/test_derby_ser_pie.py 4096

The reference side is tools/pie/derby_ser_pie_dump.c, compiled on the host
(it #includes the C file with DERBY_SERPIE_HOST, so the reference that runs
here is the one that is flashed). Compared: all eblk x 32 output lanes of the
rotation, and the chord mask (masked with `valid`, as dsp_cull_pie returns it)
of every series. The cull is run on the reference's rotated lanes, so a
rotation mismatch cannot hide a cull mismatch or the other way round.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SRC = os.path.join(ROOT, 'main', 'pocket', 'derby_ser_pie.c')
sys.path.insert(0, HERE)
from piesim import Sim, extract_asm, store16, load16  # noqa: E402
from run_models import compiler  # noqa: E402

IN, OUT, KROT, KCULL, M = 0x1000, 0x1000 + 5 * 64, 0x2000, 0x2100, 0x3000


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 512
    rot_asm = extract_asm(SRC, 'void dsp_rotate_pie(')
    cull_asm = extract_asm(SRC, 'uint32_t dsp_cull_pie(')
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'dump')
        subprocess.check_call(compiler() + ['-O2', '-o', exe, os.path.join(HERE, 'derby_ser_pie_dump.c'), '-lm'])
        lines = subprocess.run([exe, str(n)], capture_output=True, text=True, check=True).stdout.splitlines()
    cases = rot_bad = cull_ok = cull_bad = skipped = 0
    counts = {}                             # eblk -> (rotate, cull) instructions executed
    out_ref = None
    for line in lines:
        f = line.split()
        if f[0] == 'R':
            cases += 1
            eblk, _ends, valid = int(f[1]), int(f[2]), int(f[3])
            rk = [int(x) for x in f[4:7]]
            vals = [int(x) for x in f[7:]]
            lanes = eblk * 32
            vin, out_ref = vals[:lanes], vals[lanes:2 * lanes]
            mem = bytearray(0x4000)
            store16(mem, IN, vin)
            store16(mem, KROT, rk + [0] * 5)
            sim = Sim(mem)
            sim.run(rot_asm, {'in': IN, 'out': OUT, 'k': KROT, 'n': eblk, 'sh': 14})
            counts[eblk] = [sim.count, counts.get(eblk, [0, 0])[1]]
            got = [v - 0x10000 if v & 0x8000 else v for v in load16(mem, OUT, lanes)]
            if got != out_ref:
                rot_bad += 1
                if rot_bad <= 3:
                    bad = [i for i in range(lanes) if got[i] != out_ref[i]]
                    print(f'rotation mismatch, case {cases - 1}: lanes {bad[:8]} got '
                          f'{[got[i] for i in bad[:8]]} want {[out_ref[i] for i in bad[:8]]}')
        elif f[0] == 'S':
            skipped += 1
        elif f[0] == 'C':
            consts = [int(x) for x in f[2:15]]
            want = int(f[15])
            mem = bytearray(0x4000)
            store16(mem, OUT, out_ref)          # the reference's rotation: isolate the cull
            store16(mem, KCULL, consts + [0] * 3 + [-(1 << i) for i in range(8)])
            sim = Sim(mem)
            sim.run(cull_asm, {'v': OUT, 'k': KCULL, 'm': M, 't': 0, 'n': eblk,
                               's14': 14, 's0': 0, 'two': 2})
            counts[eblk][1] = sim.count
            assert sim.ar['k'] == KCULL, 'constant pointer not back at the table'
            mask = 0
            for b in range(eblk - 1):
                mask |= mem[M + 1 + b] << (8 * b)
            got = mask & valid
            if got == want:
                cull_ok += 1
            else:
                cull_bad += 1
                if cull_bad <= 3:
                    print(f'cull mismatch, case {cases - 1} series {f[1]}: got {got:#x} want {want:#x}')
    print(f'derby_ser_pie: {cases} cases ({n} geometric, {n} synthetic), rotation mismatches={rot_bad}; '
          f'cull {cull_ok + cull_bad} calls ({skipped} out of range), mismatches={cull_bad}')
    for eblk, (r, c) in sorted(counts.items()):
        print(f'instructions executed per call, {eblk} end blocks: rotate {r}, cull {c}')
    if rot_bad or cull_bad or not cull_ok:
        sys.exit(1)


if __name__ == '__main__':
    main()
