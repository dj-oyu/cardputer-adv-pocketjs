"""Host test of pie_rank.py's pattern matcher on hand-written loops.

    python tools/r5sim/test_pie_rank.py
"""
import json, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def loop(base, func, body, iters, entries, kind='branch'):
    """body: list of (mnemonic, text, length). The last instruction is the back edge."""
    pcs, pc = {}, base
    for mn, text, ln in body:
        pcs[f'{pc:#x}'] = [iters + entries, mn, text, ln, func]
        last = pc
        pc += ln
    return pcs, [[f'{last:#x}', f'{base:#x}', kind, iters]]


def main():
    pcs, edges = {}, []
    # copy: 4 bytes an iteration, 64 iterations an entry
    p, e = loop(0x42001000, 'copy_fn', [
        ('l32i.n', 'l32i.n\ta8, a3, 0', 2), ('s32i.n', 's32i.n\ta8, a2, 0', 2),
        ('addi.n', 'addi.n\ta3, a3, 4', 2), ('addi.n', 'addi.n\ta2, a2, 4', 2),
        ('bltu', 'bltu\ta2, a4, 0x42001000', 3)], 63 * 50, 50)
    pcs.update(p); edges += e
    # fill
    p, e = loop(0x42002000, 'fill_fn', [
        ('s32i.n', 's32i.n\ta5, a2, 0', 2), ('addi.n', 'addi.n\ta2, a2, 4', 2),
        ('bne', 'bne\ta2, a4, 0x42002000', 3)], 31 * 40, 40)
    pcs.update(p); edges += e
    # pointer chase (a list walk): must be rejected
    p, e = loop(0x42003000, 'walk_fn', [
        ('l32i.n', 'l32i.n\ta2, a2, 0', 2), ('l32i.n', 'l32i.n\ta8, a2, 4', 2),
        ('bnez', 'bnez\ta2, 0x42003000', 3)], 20 * 100, 100)
    pcs.update(p); edges += e
    # a branch over one move, taken half the time
    pcs['0x42004000'] = [1000, 'bnez', 'bnez\ta3, 0x42004005', 3, 'sel_fn']
    pcs['0x42004003'] = [500, 'mov.n', 'mov.n\ta2, a4', 2, 'sel_fn']
    d = {'turns': 1, 'pcs': pcs, 'backedges': edges,
         'insn_by_func': {'x': 100000}, 'taken_by_func': {}, 'loaduse_by_func': {}, 'mnem': {}}
    with tempfile.NamedTemporaryFile('w', suffix='.json', delete=False) as f:
        json.dump(d, f)
    out = subprocess.run([sys.executable, os.path.join(HERE, 'pie_rank.py'), f.name, '--min-cycles', '10'],
                         capture_output=True, text=True).stdout
    os.unlink(f.name)
    checks = [('PIE copy', 'copy_fn'), ('PIE fill', 'fill_fn'), ('MOVcc', 'sel_fn'), ('LOOP', 'copy_fn')]
    cand = out.split('== hottest rejected')[0]
    rej = out.split('== hottest rejected')[1]
    ok = True
    for kind, fn in checks:
        hit = any(kind in l and fn in l for l in cand.splitlines())
        print(('ok  ' if hit else 'FAIL'), kind, fn)
        ok &= hit
    hit = any('walk_fn' in l and 'pointer chasing' in l for l in rej.splitlines())
    print(('ok  ' if hit else 'FAIL'), 'walk_fn rejected as pointer chasing')
    ok &= hit
    if not ok:
        print(out)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
