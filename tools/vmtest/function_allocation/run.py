#!/usr/bin/env python3
"""Isolated final-function-allocation regression matrix (no prepare overlay)."""
import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]
QJS = ROOT / 'components/quickjs-ng/quickjs-ng'
OUT = ROOT / '.cache/function_allocation'


def run(label, args, env, allow_expected_difference=False):
    log = OUT / 'evidence' / (label + '.log')
    log.parent.mkdir(parents=True, exist_ok=True)
    print(label, flush=True)
    with log.open('w') as f:
        result = subprocess.run(args, cwd=ROOT, env=env, stdout=f, stderr=subprocess.STDOUT)
    if result.returncode and not (allow_expected_difference and result.returncode == 1):
        print(log.read_text()[-12000:], flush=True)
        raise SystemExit(f'{label}: exit={result.returncode}; {log}')
    return log


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--m32', action='store_true')
    p.add_argument('--oom', action='store_true')
    p.add_argument('--variant', choices=['all', 'o2', 'asan'], default='all')
    p.add_argument('--only', choices=['candidate', 'baseline', 'poison'])
    p.add_argument('--negative', action='store_true', help='omit header memset in poison source; must be rejected')
    p.add_argument('--negative-copy', action='store_true', help='omit bytecode copy; coverage oracle must reject it')
    a = p.parse_args()
    negative = a.negative or a.negative_copy
    assert not (a.negative and a.negative_copy)
    source = (QJS / 'quickjs.c').read_text()
    begin = source.index('static JSValue js_create_function(')
    end = source.index('static void free_function_bytecode(', begin)
    body = source[begin:end]
    assert body.count('b = js_alloc_function_bytecode(ctx, function_size, false, &allocation_shift);') == 1
    assert body.count('memset(b, 0, sizeof(*b));') == 1
    modes = [a.only] if a.only else ['baseline', 'candidate', 'poison']
    if a.only in ['candidate', 'poison'] and not negative:
        modes.insert(0, 'baseline')
    flags = os.environ.get('VMTEST_CFLAGS', '')
    if a.m32:
        flags += ' ' + subprocess.check_output(['bash', 'tools/vmtest/m32_sysroot.sh'], cwd=ROOT, text=True).strip()
    for mode in modes:
        suffix = '-negative-header' if a.negative else '-negative-copy' if a.negative_copy else ''
        folder = OUT / f'source-{"m32" if a.m32 else "native"}-{mode}-{a.variant}{suffix}'
        shutil.copytree(QJS, folder, dirs_exist_ok=True)
        modified = body
        if mode == 'baseline':
            modified = modified.replace('function_size, false, &allocation_shift)', 'function_size, true, &allocation_shift)')
            modified = modified.replace('    memset(b, 0, sizeof(*b));\n', '')
        if mode == 'poison':
            modified = modified.replace('    memset(b, 0, sizeof(*b));', '''    /* Test-only: dirty every byte, including header padding, before init. */
    memset(b, 0xa5, function_size);
    memset(b, 0, sizeof(*b));
    for (size_t test_i = 0; test_i < sizeof(*b); test_i++)
        assert(((uint8_t *)b)[test_i] == 0);
    uint8_t *test_coverage = (calloc)(function_size, 1);
    assert(test_coverage != NULL);
#define FA_COPY(dst, src, n) do { \\
    size_t test_n = (n); \\
    size_t test_offset = (uint8_t *)(dst) - (uint8_t *)b; \\
    assert(test_offset >= sizeof(*b) && test_offset <= function_size); \\
    assert(test_n <= function_size - test_offset); \\
    for (size_t test_i = 0; test_i < test_n; test_i++) { \\
        assert(test_coverage[test_offset + test_i] == 0); \\
        test_coverage[test_offset + test_i] = 1; \\
    } \\
    memcpy(dst, src, test_n); \\
} while (0)
    assert(cpool_offset == sizeof(*b));
    assert(vardefs_offset == cpool_offset + fd->cpool_count * sizeof(*fd->cpool));
    /* docs/vm/compact-vars.md: a hardened context keeps 2 B per variable. */
    assert(closure_var_offset == vardefs_offset + (ctx->harden ?
           js_compact_vars_size(fd->arg_count + fd->var_count) :
           (fd->arg_count + fd->var_count) * sizeof(*fa_vardefs(b))));
    assert(byte_code_offset == closure_var_offset +
           fd->closure_var_count * sizeof(*fd->closure_var));
    assert(function_size == byte_code_offset + fd->byte_code.size);''')
            # Copy lengths and destination intervals must form a complete partition.
            # Existing copies cover each interval exactly; zero counts need NULLs.
            modified = modified.replace('        memcpy(', '        FA_COPY(').replace('    memcpy(b->byte_code_buf', '    FA_COPY(b->byte_code_buf')
            modified = modified.replace('    b->stack_size = stack_size;', '''    assert((b->cpool_count != 0) == (fa_cpool(b) != NULL));
    assert((b->arg_count + b->var_count != 0) ==
           ((ctx->harden ? (void *)fa_compact_vars(b) : (void *)fa_vardefs(b)) != NULL));
    assert(!(fa_vardefs(b) && fa_compact_vars(b)));
    b->stack_size = stack_size;''')
            modified = modified.replace('    fa_set_has_prototype(b, fd->has_prototype);', '''    for (size_t test_i = sizeof(*b); test_i < function_size; test_i++)
        assert(test_coverage[test_i] == 1);
    (free)(test_coverage);
#undef FA_COPY
    printf("#info function_allocation header=%zu payload=%zu\\n", sizeof(*b), function_size - sizeof(*b));
    assert((b->closure_var_count != 0) == (b->closure_var != NULL));
    fa_set_has_prototype(b, fd->has_prototype);''')
        if negative:
            assert mode == 'poison', 'negative controls require --only poison'
            if a.negative:
                modified = modified.replace('    memset(b, 0, sizeof(*b));', '')
            else:
                needle = 'FA_COPY(b->byte_code_buf, fd->byte_code.buf, fd->byte_code.size);'
                assert modified.count(needle) == 1
                modified = modified.replace(needle, '(void)0; /* negative: missing bytecode copy */')
        (folder / 'quickjs.c').write_text(source[:begin] + modified + source[end:])
        for debug in ['debug', 'strip']:
            for variant in (['o2', 'asan'] if a.variant == 'all' else [a.variant]):
                label = f'{"m32" if a.m32 else "native"}-{mode}-{debug}-{variant}' + suffix
                env = dict(os.environ, VMTEST_QJS=str(folder), VMTEST_OUT=str(OUT / label),
                           VMTEST_CFLAGS=flags + (' -DCONFIG_POCKET_VM_STRIP_DEBUG=1' if debug == 'strip' else ''),
                           JOBS=os.environ.get('JOBS', '8'))
                run(label + '-build', ['bash', 'tools/vmtest/build.sh', variant], env)
                if mode == 'candidate':
                    cflags = ['-O2', '-g'] if variant == 'o2' else [
                        '-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=undefined']
                    objects = OUT / label / ('obj-' + variant)
                    executable = OUT / label / 'frame-alignment'
                    # Force only the first raw function allocation; an OOM
                    # exception must continue to use naturally aligned objects.
                    frame_source = OUT / label / 'frame-source'
                    shutil.copytree(folder, frame_source, dirs_exist_ok=True)
                    frame_code = (frame_source / 'quickjs.c').read_text()
                    frame_code = frame_code.replace(
                        '    raw = zero ? js_mallocz(ctx, size + JS_FUNCTION_RESERVE) :',
                        '    fa_function_allocation_active = 1;\n'
                        '    raw = zero ? js_mallocz(ctx, size + JS_FUNCTION_RESERVE) :')
                    frame_code = frame_code.replace(
                        '                 js_malloc(ctx, size + JS_FUNCTION_RESERVE);',
                        '                 js_malloc(ctx, size + JS_FUNCTION_RESERVE);\n'
                        '    fa_function_allocation_active = 0;')
                    (frame_source / 'quickjs.c').write_text(frame_code)
                    command = ['gcc', '-std=gnu11', '-w', '-DQUICKJS_NG_BUILD', '-D_GNU_SOURCE',
                        '-I', str(OUT / label / 'include'), '-I', str(frame_source), *cflags,
                        *shlex.split(env['VMTEST_CFLAGS']),
                        *(['-DFA_EXPECT_ALIGN8'] if a.m32 else []),
                        'tools/vmtest/function_allocation/frame_alignment.c',
                        *[str(objects / (name + '.o')) for name in
                          ['dtoa', 'libregexp', 'libunicode', 'quickjs-libc', 'quickjs-vm']],
                        '-lm', '-lpthread', '-ldl', '-o', str(executable)]
                    run(label + '-frame-build', command, env)
                    run(label + '-frame', [str(executable)], env)
                if negative:
                    result = subprocess.run([str(OUT / label / ('vmrun-' + variant)),
                                             '--profile', 'host', 'tools/vmtest/corpus/closures.js'],
                                            cwd=ROOT, env=env, capture_output=True)
                    (OUT / 'evidence' / (label + '-negative.log')).write_bytes(result.stdout + result.stderr)
                    assert result.returncode != 0, 'negative control unexpectedly passed'
                    expression = b'((uint8_t *)b)[test_i] == 0' if a.negative else b'test_coverage[test_i] == 1'
                    assert expression in result.stderr, result.stderr
                    print('negative control rejected by ' + expression.decode(), flush=True)
                    return
                log = run(label + '-corpus', ['bash', 'tools/vmtest/run.sh', '--variant', variant], env,
                          allow_expected_difference=debug == 'strip' or a.m32)
                actual = OUT / label / ('actual-' + variant)
                expected_names = {p.stem for p in (ROOT / 'tools/vmtest/corpus').glob('*.js')}
                assert {p.stem for p in actual.glob('*.txt')} == expected_names
                for name in expected_names:
                    output = (actual / (name + '.txt')).read_bytes()
                    if re.search(rb'(?:Address|Leak|UndefinedBehavior|Memory)Sanitizer|runtime error:', output):
                        print(f'{label}/{name}: sanitizer diagnostic follows', flush=True)
                        print(output.decode(errors='replace'), flush=True)
                    assert not re.search(rb'(?:Address|Leak|UndefinedBehavior|Memory)Sanitizer', output), (label, name)
                    assert b'runtime error:' not in output, (label, name)
                    status = int(re.search(rb'exit=(\d+)\s*$', output).group(1))
                    assert status < 128 and status != 124, (label, name, status)
                if mode == 'baseline' and debug == 'strip' and not a.m32:
                    # The shared expectations describe debug metadata and exact
                    # allocation indices; stripping changes both in the old code.
                    failures = set(re.findall(r'^FAIL (\S+)', log.read_text(), re.M))
                    assert failures == {'error_toplevel', 'job_throw', 'math_fast_call', 'oom_resolving_functions',
                                        'oom_with_resolvers', 'try_finally'}, failures
                if mode != 'baseline':
                    baseline = OUT / f'{"m32" if a.m32 else "native"}-baseline-{debug}-{variant}' / ('actual-' + variant)
                    for name in sorted(expected_names):
                        assert (actual / (name + '.txt')).read_bytes() == (baseline / (name + '.txt')).read_bytes(), (label, name)
                    with log.open('a') as f:
                        f.write(f'baseline equivalence: {len(expected_names)} files, identical output and exit status\n')
                if a.oom and variant == 'asan':
                    count = 4800
                    for name in ['closures', 'generators']:
                        trace = OUT / 'evidence' / f'{label}-{name}.trace'
                        run(label + '-trace-' + name, [str(OUT / label / ('vmrun-' + variant)),
                            '--trace', str(trace), f'tools/vmtest/corpus/{name}.js'], env)
                        attempts = sum(line.startswith(('+ ', '~ ', '! ', '!~ ')) for line in trace.read_text().splitlines())
                        count = max(count, attempts)
                    run(label + '-oom', ['bash', 'tools/vmtest/oom_sweep.sh', '-n', str(count)], dict(env, VARIANT=variant))


if __name__ == '__main__':
    main()
