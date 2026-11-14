"""Compiler launcher for the hot/cold layout (docs/vm/qjs-hotcold.md).

CMake puts this in front of the compiler for the quickjs-ng component
(<LANG>_COMPILER_LAUNCHER, components/quickjs-ng/CMakeLists.txt):

    python hc_cc.py --mode use --profile P.json [--only quickjs.c] [--keep-out X.lf] [--pad N] -- <compiler> <args...>

For the sources named by --only it compiles to assembly, rewrites the assembly
(hc_asm.py: `gen` adds per-block symbols, `use` lays the blocks out by the
profile's tiers and verifies the result against the original), and assembles
that with the same compiler driver, so the assembler gets the options the
driver would have given it. Every other source is compiled as if this script
were not there.

The two intermediate files stay next to the object: <obj>.hc0.s is GCC's
output, <obj>.hc.s is what was assembled.
"""
import json, os, re, subprocess, sys

# The build must not write into the source tree: a __pycache__ next to this
# file changes the source snapshot the build broker verifies after a build.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hc_asm  # noqa: E402

def main():
    argv = sys.argv[1:]
    cut = argv.index('--')
    opts, cmd = argv[:cut], argv[cut + 1:]
    mode = opts[opts.index('--mode') + 1]
    only = opts[opts.index('--only') + 1].split(',') if '--only' in opts else ['quickjs.c']
    profile = opts[opts.index('--profile') + 1] if '--profile' in opts else None
    pad = int(opts[opts.index('--pad') + 1]) if '--pad' in opts else 0
    keep_out = opts[opts.index('--keep-out') + 1].split(',') if '--keep-out' in opts else []
    # --wrap: the build's own compile launcher (ESP-IDF's RULE_LAUNCH_COMPILE,
    # ccache), which pocket_hotcold() takes off the target so that it runs
    # here, in front of the compiler. In front of this script, ccache took
    # Python for the compiler, and on Windows a long command reached Python as
    # `python @response-file`, which Python reads as a script name.
    wrap = opts[opts.index('--wrap') + 1].split(';') if '--wrap' in opts else []

    src = None
    if '-c' in cmd and '-o' in cmd:
        cand = [a for a in cmd[1:] if a.endswith('.c') and not a.startswith('-')]
        src = cand[-1] if cand else None
    if not src or os.path.basename(src) not in only:
        sys.exit(subprocess.call(wrap + cmd))

    obj = cmd[cmd.index('-o') + 1]
    s0, s1 = obj + '.hc0.s', obj + '.hc.s'
    # 1. C -> assembly, with one change to the debug information: no
    # location views (symbolic view numbers the assembler has to count in
    # address order, which moved blocks break). -fno-var-tracking would make
    # the debug information simpler still, but on this GCC it changes the
    # generated code (8 functions of quickjs.c), so it is not used.
    c1 = list(cmd)
    c1[c1.index('-c')] = '-S'
    c1[c1.index('-o') + 1] = s0
    r = subprocess.call(c1 + os.environ.get('HC_CC_DEBUG_FLAGS', '-gno-variable-location-views').split())
    if r:
        sys.exit(r)
    lines = open(s0).read().split('\n')
    base = os.path.basename(src)
    tag = hc_asm.file_tag(base)
    # quickjs.c keeps the section name its profile and docs were written
    # with; every other file gets its own, so two objects never contribute
    # to a section of the same name (each is placed where its object is).
    section = hc_asm.SECTION if base == 'quickjs.c' else f'.text.hc_{tag}_'
    if mode == 'gen':
        new = hc_asm.gen(lines, tag)
    elif mode == 'use':
        prof = json.load(open(profile))
        fns = dict(prof.get('files', {}).get(base, {}))
        keep = set()
        for lf in keep_out:
            # Functions a linker fragment places by name (the IRAM helpers,
            # main/vm/helper_iram8.lf) must stay in their own .text.<name>
            # sections, or the fragment would silently stop matching them.
            if os.path.exists(lf):
                for name in re.findall(r'^\s*(?:\S+:\s*)?([\w$.]+)\s+\(', open(lf).read(), re.M):
                    fns.pop(name, None)
                    keep.add(name)
        st = {}
        lines = hc_asm.hoist_files(lines)
        # --lines: place functions the block profile misses by their source
        # lines (hc_asm.tiers_from_lines). Off by default: on the device it
        # moved 8 pocket_derby.c functions and the DERBY turn did not change
        # (+0.23%, docs/vm/qjs-hotcold.md sec.13.6).
        lmap = prof.get('lines', {}).get(base) if '--lines' in opts else None
        known = set(prof.get('known', {}).get(base, ()))
        new = hc_asm.use(lines, fns, st, pad, prof.get('meta', {}).get('hot_classes', 1), section, lmap, known, keep)
        st['verified'] = hc_asm.verify(lines, new)
        # One line in the build log: a stale profile is visible, not silent.
        print(f'hc_cc: {base}: {st["matched"]} functions laid out hot/cold '
              f'({st["missed"]} of the profile\'s did not match this object code; '
              f'{st["by_lines"]} placed by source line instead)',
              file=sys.stderr)
        if fns and st['matched'] * 2 < len(fns):
            print('hc_cc: warning: most of the profile is stale; retrain (tools/qjs_hotcold/train.sh)',
                  file=sys.stderr)
    else:
        raise SystemExit(f'hc_cc: unknown mode {mode}')
    open(s1, 'w').write('\n'.join(new))
    # 2. assembly -> object, through the same driver. The dependency file was
    # written by step 1; -MD here would overwrite it with the .s alone.
    c2 = []
    skip = False
    for a in cmd:
        if skip:
            skip = False; continue
        if a in ('-MT', '-MF', '-MQ'):
            skip = True; continue
        if a in ('-MD', '-MMD', '-MP'):
            continue
        c2.append(s1 if a == src else a)
    sys.exit(subprocess.call(c2))

if __name__ == '__main__':
    main()
