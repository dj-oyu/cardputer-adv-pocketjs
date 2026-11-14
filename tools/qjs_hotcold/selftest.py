"""Checks of the hot/cold tooling that need only the compiler (docs/vm/qjs-hotcold.md sec.6).

    python tools/qjs_hotcold/selftest.py <build dir> [--random N] [--seed S]

<build dir> is any configured firmware or r5sim build directory (its
compile_commands.json gives quickjs.c's command line). In a scratch directory:

1. identity: `gen` (labels only) assembles to the same instructions as the
   compiler's own object, function by function.
2. random tiers: N times, give every block of every function a random tier
   (a third of the functions untouched), run `use` + `verify`, assemble.
   This drives the fix-ups (inverted branches, appended jumps, jump-table
   islands, loops) through far more shapes than a real profile has.
   With --keep DIR the last object is left there (to link into an image and
   run: tools/qjs_hotcold/train.sh --torture).
"""
import argparse, json, os, random, re, shlex, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hc_asm  # noqa: E402

def command(build):
    cc = json.load(open(os.path.join(build, 'compile_commands.json')))
    e = next(x for x in cc if x['file'].replace('\\', '/').endswith('quickjs-ng/quickjs.c'))
    cmd = shlex.split(e['command'])
    return [c for c in cmd if not c.endswith('hc_cc.py')], e['directory']

def strip_launcher(cmd):
    """Drop a launcher (python hc_cc.py ... --) if the build already has one."""
    return cmd[cmd.index('--') + 1:] if '--' in cmd else cmd

def disasm(objdump, obj):
    """section -> its instructions as text. Addresses and branch targets are
    dropped, and so is what the assembler decides by itself: the narrow or
    wide form of an instruction and its padding bytes (a label in front of an
    instruction changes where it may widen one to align a loop)."""
    out = subprocess.run([objdump, '-d', '--no-show-raw-insn', obj], capture_output=True, text=True).stdout
    secs = {}; cur = None
    for l in out.splitlines():
        m = re.match(r'^Disassembly of section (\S+):$', l)
        if m:
            cur = secs.setdefault(m.group(1), []); continue
        m = re.match(r'^\s*[0-9a-f]+:\s+(.*)$', l)
        if m and cur is not None:
            t = re.sub(r'\b[0-9a-f]+ <[^>]+>', '@', m.group(1)).strip()
            if t.startswith('.byte'):
                continue
            mn, _, ops = t.partition('\t')
            cur.append((mn[:-2] if mn.endswith('.n') else mn) + ' ' + ops)
    return secs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('build')
    ap.add_argument('--random', type=int, default=3)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--keep')
    a = ap.parse_args()
    cmd, cwd = command(a.build)
    cmd = strip_launcher(cmd)
    objdump = cmd[0].replace('-gcc', '-objdump')
    tmp = os.path.abspath(a.keep or tempfile.mkdtemp(prefix='hc_selftest_'))
    os.makedirs(tmp, exist_ok=True)

    def build(out, launcher):
        c = list(cmd)
        c[c.index('-o') + 1] = out
        # the dependency file goes to the scratch directory too
        if '-MF' in c:
            c[c.index('-MF') + 1] = out + '.d'
        r = subprocess.run(launcher + c, cwd=cwd, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("selftest: build failed:\n" + "\n".join(l for l in r.stderr.splitlines() if "warning" not in l)[-3000:])

    plain = os.path.join(tmp, 'plain.o')
    build(plain, [])
    genobj = os.path.join(tmp, 'gen.o')
    build(genobj, [sys.executable, os.path.join(HERE, 'hc_cc.py'), '--mode', 'gen', '--'])
    d0, d1 = disasm(objdump, plain), disasm(objdump, genobj)
    bad = [n for n in d0 if d0[n] != d1.get(n)]
    if bad or set(d0) != set(d1):
        raise SystemExit(f'selftest: gen changed the code of {len(bad)} sections, e.g. {bad[:5]}')
    print(f'selftest: gen is the identity on {len(d0)} text sections')

    lines = open(genobj + '.hc0.s').read().split('\n')
    funcs = [it for it in hc_asm.parse(lines) if isinstance(it, hc_asm.Func)]
    print(f'selftest: {len(funcs)} functions, {sum(len(f.blocks) for f in funcs)} blocks')
    rng = random.Random(a.seed)
    for r in range(a.random):
        prof = {}
        for f in funcs:
            if rng.random() < 0.33 and f.name != 'JS_CallInternal':
                continue
            # runs of equal tiers of random length, as a real profile has
            t = []; cur = rng.randrange(5)
            for _ in f.blocks:
                if rng.random() < 0.4:
                    cur = rng.randrange(5)
                t.append(str(cur))
            t[rng.randrange(len(t))] = '0'      # (use skips a function with no hot block)
            prof[f.name] = {'h': f.hash(), 't': ''.join(t)}
        pj = os.path.join(tmp, f'random{r}.json')
        # three hot classes, warm, cold
        json.dump({'meta': {'hot_classes': 3}, 'files': {'quickjs.c': prof}}, open(pj, 'w'))
        obj = os.path.join(tmp, f'random{r}.o')
        build(obj, [sys.executable, os.path.join(HERE, 'hc_cc.py'), '--mode', 'use', '--profile', pj, '--'])
        print(f'selftest: random tiers {r}: {len(prof)} functions partitioned, verified, assembled')

if __name__ == '__main__':
    main()
