"""Per-bucket working set of STRESS's JS turns (docs/vm/turn-working-set.md).

Reads the turns `icache.py extract` wrote and replays them, at the r5sim
image's own addresses, through the same 8-way 32 KiB LRU as `icache.py model`
(the totals agree with it run as `model turns sim.elf sim.elf`). What this adds
is attribution: every 32 B line is charged to the function whose instruction
fetched it, every function to a bucket (by name and by the object the map
puts it in), and the misses and distinct lines are reported per bucket.

    python tools/r5sim/turnset.py turns.pkl.gz build/r5sim.elf build/r5sim.map \
        --src components/quickjs-ng/quickjs-ng/quickjs.c [--top 25] [--mapping out.tsv]

Non-flash code (IRAM, ROM) costs no cache line; if the trace has it (a
-dfilter that includes 0x40000000..0x403dffff) it is counted separately as
fetches, not lines.

--src splits JS_CallInternal's touched lines by the opcode handler they
belong to (addr2line -i, outermost frame) and by heat (block executions that
reach the line per turn).
"""
import argparse, bisect, collections, gzip, pickle, re, subprocess, sys

FLASH = (0x42000000, 0x44000000)

def in_flash(a):
    return FLASH[0] <= a < FLASH[1]

def symbols(nm, elf):
    out = subprocess.run([nm, '-S', '--defined-only', elf], capture_output=True, text=True).stdout
    funcs = []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in 'tTwW':
            a, n = int(p[0], 16), int(p[1], 16)
            if n:
                funcs.append((a, n, p[3]))
    funcs.sort()
    return funcs

def map_objects(path):
    """(start, size, object) of every input section the map places."""
    ranges = []
    sec = None
    pat = re.compile(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$')
    head = re.compile(r'^ (\.\S+)(.*)$')
    with open(path, errors='replace') as f:
        for line in f:
            m = head.match(line)
            if m:
                sec = m.group(1)
                rest = m.group(2)
                m2 = pat.match(' ' + rest) if rest.strip() else None
                if m2:
                    ranges.append((int(m2.group(1), 16), int(m2.group(2), 16), m2.group(3).strip()))
                    sec = None
                continue
            if sec:
                m2 = pat.match(line)
                if m2:
                    ranges.append((int(m2.group(1), 16), int(m2.group(2), 16), m2.group(3).strip()))
                sec = None
    ranges = [r for r in ranges if r[1]]
    ranges.sort()
    return ranges

def short_obj(o):
    # "esp-idf/quickjs-ng/libquickjs-ng.a(quickjs.c.obj)" -> "libquickjs-ng.a(quickjs.c.obj)"
    o = o.replace('\\', '/')
    m = re.search(r'([^/]+\.a)\(([^)]+)\)', o)
    if m:
        return f'{m.group(1)}({m.group(2)})'
    return o.rsplit('/', 1)[-1]

# Buckets. Order matters: first match wins. Each rule is (bucket, object
# regex or None, name regex or None); both given must match.
BUCKETS = ['interp-core', 'interp-helpers', 'object/alloc/GC', 'builtins',
           'native boundary', 'libc/other']
QJS = r'\(quickjs(-vm)?\.c\.obj\)'
RULES = [
    ('interp-core', QJS, r'^JS_CallInternal(\..*)?$'),
    # Allocation (QuickJS's wrappers and the heap behind them), objects,
    # shapes, closures, strings as allocations, GC and finalizers.
    ('object/alloc/GC', None, r'^(tlsf_|block_|control_|heap_caps_|multi_heap_|malloc$|free$|calloc$|realloc$|_malloc_r$|_free_r$|_calloc_r$|_realloc_r$)'),
    ('object/alloc/GC', QJS, r'^(js_malloc|js_mallocz|js_calloc|js_free|js_realloc|js_def_|js__malloc|js_dbuf_|js_strdup|js_strndup|js_vm_seg|vm_seg|vm_slab|slab_'
        r'|JS_NewObject|JS_NewArray|js_create_from_ctor|js_create_array|js_new_shape|new_shape|js_free_shape|js_clone_shape|resize_properties|compact_properties'
        r'|add_property|add_shape_property|convert_fast_array|expand_fast_array|js_allocate_fast_array|js_array_grow|js_realloc_array'
        r'|free_object|free_property|free_var_ref|free_function_bytecode|free_gc_object|free_zero_refcount|free_bytecode|__JS_FreeValueRT_gc'
        r'|js_array_finalizer|js_bytecode_function_finalizer|js_c_function_finalizer|js_\w+_finalizer|gc_|mark_|JS_RunGC|js_trigger_gc|reset_weak'
        r'|js_closure|js_build_arguments|js_build_rest|js_build_mapped|async_func_init|close_var_refs'
        r'|js_new_string|js_alloc_string|JS_NewString|js_new_string8|js_new_string16|JS_ConcatString|string_buffer_)'),
    # C builtins, and the libraries only they call (libm behind Math,
    # libregexp/libunicode behind RegExp/String).
    ('builtins', QJS, r'^(js_math_|js_call_math_fast|js_array_|js_string_|js_number_|js_object_|js_function_|js_promise_|js_typed_array|js_array_buffer|js_json_'
        r'|js_regexp_|js_date_|js_map_|js_set_|js_weak|js_bigint_|js_boolean_|js_symbol_|js_error_|js_reflect_|js_proxy_|js_iterator_|js_async_|js_generator_'
        r'|js_global_|JS_ExecutePendingJob|promise_|js_create_resolving_functions|typed_array_init|array_buffer_|perform_promise|fulfill_or_reject|js_fmod|js_TA_|js_dataview|js_allocate_typed|js_create_array_iterator|js_sort)'),
    ('builtins', r'\((libunicode|libregexp|libbf|xsum)\.c\.obj\)', None),
    ('builtins', r'\(libm_[^)]*\)', None),
    ('builtins', None, r'^(sqrt|sqrtf|sqrtl|floor|ceil|trunc|round|fabs|fmod|fmodl|pow|exp|log|sin|cos|tan|atan2?|asin|acos|hypot|cbrt|__kernel_|__ieee754_|__rem_pio2)'),
    # Everything else in QuickJS: property lookup, atoms, conversions (dtoa
    # included), calls, frames, the VM stack.
    ('interp-helpers', QJS, None),
    ('interp-helpers', r'\((dtoa|cutils|quickjs-libc)\.c\.obj\)', None),
    # The pocket/Kasane side and the image's own glue (r5sim.c stands in for
    # app_session.c).
    ('native boundary', r'\((pocket_|ksn_|app_|r5sim|jpfont|pet_|kasane)[^)]*\)', None),
    ('native boundary', None, r'^(pocket_|ksn_|kasane_|app_)'),
    ('libc/other', None, None),
]
RULES_C = [(b, re.compile(o) if o else None, re.compile(n) if n else None) for b, o, n in RULES]

def bucket_of(name, obj):
    for b, o, n in RULES_C:
        if o and not o.search(obj or ''):
            continue
        if n and not n.search(name):
            continue
        return b
    return 'libc/other'

class Cache:
    def __init__(self, sets=128, ways=8):
        self.sets, self.ways = sets, ways
        self.flush()
    def flush(self):
        self.lru = [collections.OrderedDict() for _ in range(self.sets)]
    def access(self, line):
        s = self.lru[line % self.sets]
        if line in s:
            s.move_to_end(line)
            return False
        if len(s) >= self.ways:
            s.popitem(last=False)
        s[line] = None
        return True

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('turns'); ap.add_argument('elf'); ap.add_argument('map')
    ap.add_argument('--nm', default='xtensa-esp32s3-elf-nm')
    ap.add_argument('--addr2line', default='xtensa-esp32s3-elf-addr2line')
    ap.add_argument('--src', help='quickjs.c of this image: split JS_CallInternal by opcode')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--full', action='store_true', help='fully associative 32 KiB: no conflict misses')
    ap.add_argument('--mapping', help='write function\\tobject\\tbucket\\ttouched B\\tsize here')
    ap.add_argument('--rom-ld', help='esp_rom/esp32s3/ld: names for the ROM addresses in the trace')
    ap.add_argument('--hot', type=float, default=4.0,
                    help='a JS_CallInternal line reached by fewer block executions per turn is "cold"')
    a = ap.parse_args()

    d = pickle.load(gzip.open(a.turns, 'rb'))
    syms = symbols(a.nm, a.elf)
    starts = [s for s, z, n in syms]
    ranges = map_objects(a.map)
    rstarts = [r[0] for r in ranges]
    rom = []
    if a.rom_ld:
        import glob, os
        for f in glob.glob(os.path.join(a.rom_ld, '*.ld')):
            for line in open(f, errors='replace'):
                m = re.match(r'\s*(?:PROVIDE\s*\(\s*)?(\w+)\s*=\s*0x(4000[0-9a-f]{4}|4005[0-9a-f]{4}|400[0-5][0-9a-f]{4})', line)
                if m:
                    rom.append((int(m.group(2), 16), m.group(1)))
        rom.sort()
    romstarts = [r[0] for r in rom]

    def locate(addr):
        i = bisect.bisect_right(starts, addr) - 1
        # Overlapping/aliased symbols: take the innermost that contains addr.
        while i >= 0 and not (syms[i][0] <= addr < syms[i][0] + syms[i][1]):
            if addr - syms[i][0] > 0x40000:
                return None
            i -= 1
        return syms[i] if i >= 0 else None

    obj_cache = {}
    def obj_of(addr):
        i = bisect.bisect_right(rstarts, addr) - 1
        if i >= 0 and addr < ranges[i][0] + ranges[i][1]:
            return short_obj(ranges[i][2])
        return '?'

    fn_info = {}   # name -> (start, size, obj, bucket)
    def info(addr):
        f = locate(addr)
        if not f and addr < 0x40060000 and rom:
            i = bisect.bisect_right(romstarts, addr) - 1
            if i >= 0:
                s, n = rom[i]
                if addr - s > 0x800:  # far past the last named entry
                    n = f'unnamed ROM {addr & ~0xfff:#x}'; s = addr & ~0xfff
                return (n, s, 0, 'ROM', bucket_of(n, 'ROM'))
        if not f:
            return ('?', 0, 0, obj_of(addr), 'libc/other')
        s, z, n = f
        r = fn_info.get((s, n))
        if r is None:
            o = obj_of(s)
            r = fn_info[(s, n)] = (n, s, z, o, bucket_of(n, o))
        return r

    # Per block: the ordered unique flash lines with their owner function, as
    # icache.py builds them (each instruction plus the last one's end), and
    # the non-flash fetches.
    blocks = []
    for addrs in d['blocks']:
        e = addrs[-1] + 2  # a wide last instruction's last byte; see icache.py
        if in_flash(e) and not locate(e) and locate(addrs[-1]):
            e = addrs[-1] + 1
        pts = list(addrs) + [e]
        ls = []; seen = set(); nf = []
        for x in pts:
            if not in_flash(x):
                nf.append(x); continue
            l = x >> 5
            if l not in seen:
                seen.add(l); ls.append((l, info(x)))
        blocks.append((ls, len(addrs), [info(x) for x in addrs if not in_flash(x)]))

    n = len(d['turns'])
    warm, cold = (Cache(1, 1024), Cache(1, 1024)) if a.full else (Cache(), Cache())
    wm = collections.Counter(); cm = collections.Counter()   # by bucket
    wmf = collections.Counter(); cmf = collections.Counter()  # by function
    lines_b = collections.Counter(); lines_f = collections.Counter()  # distinct per turn, summed
    union_line_owner = {}
    line_reach = collections.Counter()  # block executions reaching each line, all turns
    nonflash = collections.Counter()    # non-flash instruction fetches by function
    present = collections.Counter()     # turns in which each line is touched
    tot = [0, 0, 0]
    for t in d['turns']:
        cold.flush(); uniq = {}
        for bi in t:
            ls, ninsn, nfi = blocks[bi]
            for l, fi in ls:
                line_reach[l] += 1
                if l not in uniq:
                    uniq[l] = fi
                if warm.access(l):
                    wm[fi[4]] += 1; wmf[fi] += 1; tot[0] += 1
                if cold.access(l):
                    cm[fi[4]] += 1; cmf[fi] += 1; tot[1] += 1
            for fi in nfi:
                nonflash[fi] += 1
        tot[2] += len(uniq)
        for l, fi in uniq.items():
            present[l] += 1
            lines_b[fi[4]] += 1; lines_f[fi] += 1
            union_line_owner.setdefault(l, fi)

    union_b = collections.Counter(fi[4] for fi in union_line_owner.values())
    print(f'{a.elf}: {n} turns (from {d["first"]}), {"fully associative" if a.full else "8-way"} 32 KiB LRU, own layout')
    print(f'  total/turn: warm {tot[0]/n:.0f}  cold {tot[1]/n:.0f}  lines {tot[2]/n:.0f} '
          f'({tot[2]/n*32/1024:.1f} KiB)  union over turns {len(union_line_owner)} lines '
          f'({len(union_line_owner)*32/1024:.1f} KiB)')
    # "rare": lines reached by fewer than --hot block executions per turn,
    # i.e. fetched a handful of times for (at least) one miss.
    rare_b = collections.Counter()
    for l, k in present.items():
        if line_reach[l] / n < a.hot:
            rare_b[union_line_owner[l][4]] += k
    print(f'  {"bucket":18s} {"KiB/turn":>9s} {"lines":>7s} {"warm":>7s} {"cold":>7s} {"union KiB":>10s} {"rare KiB":>9s}')
    for b in BUCKETS:
        print(f'  {b:18s} {lines_b[b]/n*32/1024:9.2f} {lines_b[b]/n:7.0f} {wm[b]/n:7.0f} {cm[b]/n:7.0f} '
              f'{union_b[b]*32/1024:10.2f} {rare_b[b]/n*32/1024:9.2f}')
    # Non-flash code costs no cache line. The tick interrupt and the
    # scheduler run in IRAM here as often as QEMU's wall clock makes them
    # (slowed by the trace), so they are set apart as an artifact.
    isr = re.compile(r'\((xtensa_vectors|xtensa_context|xtensa_intr|portasm|port|tasks|port_systick|systimer_hal|systimer|freertos_hooks|cpu|list|queue|timers|esp_ipc|crosscore_int|int_wdt|panic|state_asm|usb_serial_jtag_connection_monitor)[^)]*\)')
    nf_isr = 0; nf_rom = collections.Counter(); nf_iram = collections.Counter(); nf_fn = collections.Counter()
    for fi, k in nonflash.items():
        if fi[3] == 'ROM':
            nf_rom[fi[4]] += k; nf_fn[fi] += k
        elif isr.search(fi[3]) or fi[0] == '?':
            nf_isr += k
        else:
            nf_iram[fi[4]] += k; nf_fn[fi] += k
    if nonflash:
        print(f'  non-flash fetches/turn (no cache line): interrupts/scheduler (QEMU timing) {nf_isr/n:.0f}; '
              f'IRAM ' + (', '.join(f'{b} {k/n:.0f}' for b, k in nf_iram.most_common()) or '0') +
              '; ROM ' + (', '.join(f'{b} {k/n:.0f}' for b, k in nf_rom.most_common()) or '0'))
        print('  top non-flash functions outside the interrupt path (fetches/turn):')
        for fi, k in nf_fn.most_common(12):
            print(f'    {k/n:8.0f}  {fi[0]}  [{fi[3]}] {fi[4]}')

    print(f'\n  top {a.top} functions by touched KiB/turn:')
    print(f'    {"KiB":>6s} {"of size":>7s} {"warm":>6s} {"cold":>6s}  function [object] bucket')
    for fi, k in lines_f.most_common(a.top):
        print(f'    {k/n*32/1024:6.2f} {fi[2]:7d} {wmf[fi]/n:6.1f} {cmf[fi]/n:6.1f}  {fi[0]} [{fi[3]}] {fi[4]}')

    if a.mapping:
        with open(a.mapping, 'w') as f:
            f.write('bucket\tfunction\tobject\ttouched_B_per_turn\tsize_B\twarm_per_turn\tcold_per_turn\n')
            for fi in sorted(lines_f, key=lambda fi: (BUCKETS.index(fi[4]), -lines_f[fi])):
                f.write(f'{fi[4]}\t{fi[0]}\t{fi[3]}\t{lines_f[fi]/n*32:.0f}\t{fi[2]}\t'
                        f'{wmf[fi]/n:.1f}\t{cmf[fi]/n:.1f}\n')

    if a.src:
        core(a, d, n, union_line_owner, line_reach, lines_f, blocks)

INLINE_LABELS = {'default', 'not_a_function', 'ret_fail', 'normal_this', 'has_call_argc', 'non_ctor_call',
                 'mul_fp_res', 'neg_fp_res', 'no_with', 'free_and_set_true', 'set_true', 'free_and_set_false'}

def core(a, d, n, owner, reach, lines_f, blocks):
    """JS_CallInternal's lines by opcode region and by heat."""
    src = open(a.src, encoding='utf-8', errors='replace').read().splitlines()
    fstart = next(i + 1 for i, s in enumerate(src)
                  if s.startswith('static JSValue JS_CallInternal(JSContext *caller_ctx'))
    fend = next(i + 1 for i in range(fstart, len(src)) if src[i].startswith('}'))
    # Regions: every CASE(OP_x) and every C label in the function. A label's
    # name says what it is (the *_slow paths, exception, the flat call).
    labels = []  # (source line, region)
    lab = re.compile(r'^\s*([a-z_][a-z0-9_]*):\s*(\{)?\s*(//.*)?$')
    for i in range(fstart, fend):
        s = src[i - 1]
        m = re.match(r'\s*CASE\((OP_\w+)\)', s)
        if m:
            labels.append((i, m.group(1))); continue
        if re.match(r'^DEFAULT:', s):
            labels.append((i, 'DEFAULT')); continue
        m = lab.match(s)
        # Labels that name a statement or two inside the code around them
        # (a throw, a shared tail of an opcode) do not start a region.
        if m and m.group(1) not in INLINE_LABELS:
            labels.append((i, m.group(1)))
    lstarts = [x[0] for x in labels]
    def region(line):
        if line < fstart or line > fend:
            return None
        i = bisect.bisect_right(lstarts, line) - 1
        return labels[i][1] if i >= 0 else 'entry'

    core_lines = sorted(l for l, fi in owner.items() if fi[4] == 'interp-core')
    if not core_lines:
        return
    # Every executed instruction address in those lines.
    insns = collections.defaultdict(set)
    cl = set(core_lines)
    for addrs in d['blocks']:
        for x in addrs:
            if (x >> 5) in cl:
                insns[x >> 5].add(x)
    addrs = sorted(x for s in insns.values() for x in s)
    # -i prints the inline chain innermost first; -a delimits the addresses.
    out = subprocess.run([a.addr2line, '-a', '-i', '-e', a.elf],
                         input='\n'.join(hex(x) for x in addrs) + '\n',
                         capture_output=True, text=True).stdout.splitlines()
    srcline = {}
    cur = None; last = None
    for s in out:
        if s.startswith('0x'):
            if cur is not None:
                srcline[cur] = last
            cur = int(s, 16); last = None
        else:
            m = re.search(r'quickjs\.c:(\d+)', s)
            if m:
                last = int(m.group(1))  # keeps the outermost quickjs.c frame
    if cur is not None:
        srcline[cur] = last
    # A line's region: the majority of its instructions' regions.
    reg_of_line = {}
    for l, xs in insns.items():
        c = collections.Counter(region(srcline.get(x) or 0) or '?' for x in xs)
        reg_of_line[l] = c.most_common(1)[0][0]
    # Per-turn presence of each core line (lines touched per turn, by region).
    per_turn_present = collections.Counter()
    for t in d['turns']:
        u = set()
        for bi in t:
            for l, fi in blocks[bi][0]:
                if fi[4] == 'interp-core':
                    u.add(l)
        for l in u:
            per_turn_present[l] += 1
    # Region totals: lines/turn and union, and heat class.
    by_reg = collections.defaultdict(lambda: [0.0, 0, 0.0])  # lines/turn, union lines, reach/turn
    for l in core_lines:
        r = reg_of_line.get(l, '?')
        by_reg[r][0] += per_turn_present[l] / n
        by_reg[r][1] += 1
        by_reg[r][2] += reach[l] / n
    hot = sum(per_turn_present[l] / n for l in core_lines if reach[l] / n >= a.hot)
    cold_ = sum(per_turn_present[l] / n for l in core_lines if reach[l] / n < a.hot)
    total = hot + cold_
    print(f'\n  JS_CallInternal [{fstart}..{fend}]: {total:.0f} lines/turn ({total*32/1024:.2f} KiB), '
          f'union {len(core_lines)} lines')
    print(f'    heat: reached >= {a.hot}/turn {hot:.0f} lines ({hot*32/1024:.2f} KiB); '
          f'< {a.hot}/turn {cold_:.0f} lines ({cold_*32/1024:.2f} KiB)')
    for thr in (1, 4, 16, 64):
        h = sum(per_turn_present[l] / n for l in core_lines if reach[l] / n >= thr)
        print(f'      reached >= {thr:3d}/turn: {h*32/1024:5.2f} KiB')
    SLOW = {'exception', 'vm_yield', 'DEFAULT'}
    CALL = {'entry', 'frame_pushed', 'restart', 'flat_call', 'flat_async_call', 'restore_call_inputs',
            'resume_caller', 'call_inputs_ready', 'done', 'done_generator'}
    def klass(r):
        if r in SLOW or 'slow' in r:
            return 'slow/exception paths'
        if r in CALL:
            return 'entry/call/return'
        return 'opcode handlers' if r != '?' else '?'
    # An opcode handler is "rare" when its hottest line is reached fewer than
    # --hot times per turn; inside common handlers, lines below --hot are the
    # handler's own uncommon branches.
    hmax = collections.Counter()
    for l in core_lines:
        r = reg_of_line.get(l, '?')
        hmax[r] = max(hmax[r], reach[l] / n)
    cls = collections.defaultdict(float)
    for l in core_lines:
        r = reg_of_line.get(l, '?'); k = klass(r)
        if k == 'opcode handlers':
            k = ('opcode handlers, rare opcode' if hmax[r] < a.hot else
                 'opcode handlers, common: hot lines' if reach[l] / n >= a.hot else
                 'opcode handlers, common: cold lines')
        else:
            k += ' (hot lines)' if reach[l] / n >= a.hot else ' (cold lines)'
        cls[k] += per_turn_present[l] / n
    print(f'    by class (hot = reached >= {a.hot}/turn):')
    for k in sorted(cls):
        print(f'      {k:44s} {cls[k]*32/1024:6.2f} KiB')
    rare = sorted((r for r in hmax if klass(r) == 'opcode handlers' and hmax[r] < a.hot))
    print(f'    rare opcodes touched: {" ".join(rare)}')
    print(f'    {"region":32s} {"KiB/turn":>8s} {"union":>6s} {"reach/turn":>10s}')
    for r, (lt, un, rc) in sorted(by_reg.items(), key=lambda kv: -kv[1][0]):
        print(f'    {r:32s} {lt*32/1024:8.2f} {un:6d} {rc:10.1f}')

if __name__ == '__main__':
    main()
