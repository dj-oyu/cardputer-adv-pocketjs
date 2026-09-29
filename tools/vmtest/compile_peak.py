"""The composition of a script's compile peak, by kind (host, WSL only).

docs/vm/spread-eval-oom.md. Builds tools/vmtest/compile_peak.c against the
vendored quickjs-ng -m32 (the device's 8 B JSValue and 4 B pointers,
tools/vmtest/m32_sysroot.sh), compiles FILE.js once with every allocation
tagged by its call stack, and prints what was live at the peak, by kind:

  python3 tools/vmtest/compile_peak.py apps/derby/derby_watch.js
  python3 tools/vmtest/compile_peak.py FILE.js --by-function   # raw sites
  python3 tools/vmtest/compile_peak.py FILE.js --write out.bc  # + JS_WriteObject
  python3 tools/vmtest/compile_peak.py FILE.js --read out.bc   # measure JS_ReadObject

Charged as the device charges (TLSF lengths); the figures are host
measurements of the device's allocation sizes, not device measurements.
The kind of a block is decided by the innermost frame that is not an
allocator helper (addr2line -i sees through inlining), mapped by KINDS.
"""
import argparse
import os
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# COMPILE_PEAK_QJS: measure a patched copy of the engine instead (a parser
# experiment), with its own cache so no object is shared with the real one.
QJS = Path(os.environ.get('COMPILE_PEAK_QJS', ROOT / 'components/quickjs-ng/quickjs-ng'))
OUT = ROOT / ('.cache/compile_peak_x' if 'COMPILE_PEAK_QJS' in os.environ else '.cache/compile_peak')

# Frames that only move memory for someone else: skipped when naming a block.
HELPERS = {
    'pk_malloc', 'pk_calloc', 'pk_realloc', 'pk_free', 'stack_id', 'charge',
    'js_malloc_rt', 'js_mallocz_rt', 'js_realloc_rt', 'js_calloc_rt', 'js_malloc', 'js_mallocz',
    'js_realloc', 'js_realloc2', 'js_def_malloc', 'js_def_realloc', 'js_calloc',
    'dbuf_realloc', 'dbuf_put', 'dbuf_putc', 'dbuf_put_u16', 'dbuf_put_u32', 'dbuf_put_u64',
    'dbuf_putstr', 'dbuf_printf', 'dbuf_claim', 'js_dbuf_realloc',
    'js_realloc_array', 'js_resize_array', 'expand_fast_array',
    'js_alloc_string', 'js_alloc_string_rt', 'js_new_string8_len', 'js_new_string16_len',
    'string_buffer_realloc', 'string_buffer_widen', 'string_buffer_putc', 'string_buffer_putc8',
    'string_buffer_putc16', 'string_buffer_write8', 'string_buffer_end', 'string_buffer_init2',
    'string_buffer_init', 'string_buffer_putc_slow', 'string_buffer_concat',
}

# First meaningful frame -> kind. Order matters only for readability.
KINDS = [
    ('js_new_function_def', 'JSFunctionDef struct'),
    ('emit_u8', 'pass-1 bytecode (fd->byte_code)'), ('emit_u16', 'pass-1 bytecode (fd->byte_code)'),
    ('emit_u32', 'pass-1 bytecode (fd->byte_code)'), ('emit_op', 'pass-1 bytecode (fd->byte_code)'),
    ('emit_atom', 'pass-1 bytecode (fd->byte_code)'), ('emit_label', 'pass-1 bytecode (fd->byte_code)'),
    ('emit_goto', 'pass-1 bytecode (fd->byte_code)'), ('emit_ic', 'pass-1 bytecode (fd->byte_code)'),
    ('emit_source_loc', 'pass-1 bytecode (fd->byte_code)'), ('emit_push_const', 'pass-1 bytecode (fd->byte_code)'),
    ('emit_class_field_init', 'pass-1 bytecode (fd->byte_code)'),
    ('add_pc2line_info', 'pc2line (resolve_labels)'), ('compute_pc2line_info', 'pc2line (resolve_labels)'),
    ('resolve_variables', 'resolve_variables: new bytecode'), ('resolve_scope_var', 'resolve_variables: new bytecode'),
    ('resolve_scope_private_field', 'resolve_variables: new bytecode'),
    ('optimize_scope_make_ref', 'resolve_variables: new bytecode'), ('instantiate_hoisted_definitions', 'resolve_variables: new bytecode'),
    ('resolve_labels', 'resolve_labels: new bytecode'), ('code_match', 'resolve_labels: new bytecode'),
    ('compute_stack_size', 'compute_stack_size'),
    ('js_create_function', 'final JSFunctionBytecode'),
    ('add_var', 'vars'), ('add_arg', 'args'), ('update_var_htab', 'vars hash'),
    ('add_scope', 'scopes'), ('push_scope', 'scopes'),
    ('new_label', 'label slots'), ('new_label_fd', 'label slots'), ('update_label', 'label slots'),
    ('cpool_add', 'cpool'), ('add_closure_var', 'closure vars'), ('get_closure_var2', 'closure vars'),
    ('__JS_NewAtom', 'atoms'), ('JS_NewAtomLen', 'atoms'), ('js_resize_atom_hash', 'atoms (hash table)'),
    ('JS_NewAtomStr', 'atoms'), ('__JS_NewAtomInit', 'atoms'),
    ('ident_realloc', 'tokens / identifiers'), ('parse_ident', 'tokens / identifiers'),
    ('js_parse_string', 'tokens: string literals'), ('js_parse_template_part', 'tokens: string literals'),
    ('js_parse_regexp', 'tokens: regexp'), ('lre_compile', 'regexp bytecode'),
    ('js_atof', 'tokens: numbers'), ('js_parse_init', 'parser state'),
    ('js_new_shape', 'shapes/objects'), ('JS_NewObjectFromShape', 'shapes/objects'),
    ('js_create_from_ctor', 'shapes/objects'), ('add_property', 'shapes/objects'),
    ('resize_properties', 'shapes/objects'), ('js_closure', 'closures'), ('js_closure2', 'closures'),
]


def sh(cmd, **k):
    return subprocess.run(cmd, check=True, **k)


def build():
    flags = subprocess.run(['bash', 'tools/vmtest/m32_sysroot.sh'], cwd=ROOT, check=True,
                           capture_output=True, text=True).stdout.split()
    obj = OUT / 'obj'
    obj.mkdir(parents=True, exist_ok=True)
    inc = OUT / 'include'
    inc.mkdir(exist_ok=True)
    (inc / 'sdkconfig.h').write_text('/* host */\n')
    cflags = ['-std=gnu11', '-O1', '-g', '-fno-omit-frame-pointer', '-fno-pie', *flags,
              '-DQUICKJS_NG_BUILD', '-D_GNU_SOURCE', '-I', str(inc), '-I', str(QJS),
              '-I', str(ROOT / 'components/pocketjs_guest/include')]
    objs = []
    for name in ('dtoa', 'libregexp', 'libunicode', 'quickjs', 'quickjs-libc', 'quickjs-vm'):
        o = obj / f'{name}.o'
        src = QJS / f'{name}.c'
        newest = max(p.stat().st_mtime for p in QJS.glob('*.h'))
        if not o.exists() or o.stat().st_mtime < max(src.stat().st_mtime, newest):
            sh(['gcc', '-c', '-w', *cflags, str(src), '-o', str(o)], cwd=ROOT)
        objs.append(str(o))
    binary = OUT / 'compile_peak'
    sh(['gcc', *cflags, '-no-pie', '-Wall', str(ROOT / 'tools/vmtest/compile_peak.c'), *objs,
        '-lm', '-lpthread', '-o', str(binary)], cwd=ROOT)
    return binary


def symbolise(binary, addrs):
    """addr -> list of function names, innermost first (inline frames included)."""
    addrs = sorted(addrs)
    # A return address points after the call; the call itself is one byte before.
    q = [hex(int(a, 16) - 1) for a in addrs]
    out = subprocess.run(['addr2line', '-f', '-i', '-e', str(binary), *q], capture_output=True,
                         text=True, check=True).stdout.splitlines()
    # With -i an address yields 2 lines per (inlined) frame; separate by re-running
    # per address would be slow, so ask addr2line for an address marker instead.
    res = {}
    out = subprocess.run(['addr2line', '-a', '-f', '-i', '-e', str(binary), *q], capture_output=True,
                         text=True, check=True).stdout.splitlines()
    cur, names, i = None, [], 0
    while i < len(out):
        l = out[i]
        if l.startswith('0x'):
            if cur is not None:
                res[cur] = names
            cur, names = l, []
            i += 1
            continue
        names.append(l)
        i += 2          # function, then file:line
    if cur is not None:
        res[cur] = names
    back = {}
    for a, qa in zip(addrs, q):
        key = '0x%08x' % int(qa, 16)
        back[a] = res.get(key, res.get(qa, ['??']))
    return back


def main():
    p = argparse.ArgumentParser()
    p.add_argument('file')
    p.add_argument('--by-function', action='store_true')
    p.add_argument('--write')
    p.add_argument('--read')
    a = p.parse_args()
    binary = build()
    run = OUT / 'run'
    run.mkdir(exist_ok=True)
    cmd = [str(binary), a.file, str(run)]
    if a.write:
        cmd += ['--write', a.write]
    if a.read:
        cmd += ['--read', a.read]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode:
        print(r.stderr)
        sys.exit(r.returncode)
    stacks = {}
    for l in (run / 'stacks.tsv').read_text().splitlines():
        f = l.split('\t')
        stacks[int(f[0])] = f[1:]
    peak = [tuple(map(int, l.split('\t'))) for l in (run / 'peak.tsv').read_text().splitlines()]
    addrs = {x for sid, _, _ in peak for x in stacks.get(sid, [])}
    names = symbolise(binary, addrs)
    by_kind = defaultdict(lambda: [0, 0])
    by_fn = defaultdict(lambda: [0, 0])
    for sid, b, n in peak:
        if sid == 0:
            by_kind['(realm before the compile)'][0] += b
            by_kind['(realm before the compile)'][1] += n
            continue
        chain = [nm for x in stacks[sid] for nm in names[x]]
        first = next((nm for nm in chain if nm not in HELPERS), '??')
        kind = None
        for nm in chain:
            if nm in HELPERS:
                continue
            kind = next((k for f, k in KINDS if f == nm), None)
            if kind:
                break
        # The pass-1 bytecode buffer's appends are inlined into many emitters;
        # anything under a js_parse_* frame that grows a DynBuf is that buffer.
        if kind is None and any(nm.startswith('dbuf') for nm in chain) and \
           any(nm.startswith('js_parse') for nm in chain):
            kind = 'pass-1 bytecode (fd->byte_code)'
        kind = kind or f'other ({first})'
        by_kind[kind][0] += b
        by_kind[kind][1] += n
        key = ' < '.join([nm for nm in chain if nm not in HELPERS][:3])
        by_fn[key][0] += b
        by_fn[key][1] += n
    total = sum(v[0] for v in by_kind.values())
    table = by_fn if a.by_function else by_kind
    print(f'live at the peak, charged: {total} B')
    for k, (b, n) in sorted(table.items(), key=lambda kv: -kv[1][0]):
        print(f'{b:8d} {100 * b / total:5.1f}% {n:6d}  {k}')


if __name__ == '__main__':
    main()
