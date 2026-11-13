"""Hot/cold layout of GCC's Xtensa assembly (docs/vm/qjs-hotcold.md).

GCC for Xtensa cannot split a function into hot and cold parts
(-freorder-blocks-and-partition is turned off by the back end), and at -Os
it lays basic blocks out in source order whatever the branch predictions
say. So the split is done here, on the assembly GCC writes, with a profile
of which basic blocks a JS turn runs (hc_profile.py):

    gen     add a local symbol in front of every basic block, so a trace of
            the linked image can be turned into a per-block profile.
            The instructions are unchanged.
    use     move every function with hot blocks into one section, .text.qjshc0
            (a second one, .text.qjshc1, if they outgrow a jump's reach),
            written class by class: first the hot blocks of all functions,
            back to back (one hot class per set of training workloads that
            use the block, the blocks all of them use first), then the warm
            blocks (run, but in no steady turn), then the cold ones (never
            run). One section, so every branch between the parts is resolved
            -- and relaxed, when it does not reach -- by the assembler, and
            no linker fragment is needed. Functions without a hot block stay
            as GCC wrote them, in their own sections.
    verify  check a `use` output against the original: every block is still
            there, with the same instructions and the same successors.
            `use` callers run it on every build.

Only the order of blocks changes. Where a block used to fall through into a
block that now lives elsewhere, either its closing conditional branch is
inverted (when its target is what follows now) or a `j` is appended.

A function is matched to its profile by a hash of its instructions (labels
renumbered), so a profile taken from other object code leaves that function
alone instead of mislabelling its blocks.

What is not kept: the address ranges of the debug information's scopes and
variable locations inside the moved functions (label differences across
moved code). Line tables and symbols are right: a part that left its
function's entry part gets a symbol of its own, <function>.hot<N>, .warm
or .cold.
"""
import hashlib, json, re, sys

SECTION = '.text.qjshc'
DIGITS = '0123456789abcdefghijklmnopqrstuvwxyz'

def tier_name(t, hot):
    """The suffix of a part's symbol: classes below `hot` are the hot ones."""
    return ('hot' if t == 0 else f'hot{t}') if t < hot else ('warm' if t == hot else 'cold')

FUNC_SECTION = re.compile(r'^\t\.section\t\.text\.([^,\s]+),"ax",@progbits$')
SIZE = re.compile(r'^\t\.size\t(\S+), \.-(\S+)$')
CODE_LABEL = re.compile(r'^(\.L\d+):$')
LEND_LABEL = re.compile(r'^\t?(\.L\d+_LEND):$')
ANY_LABEL = re.compile(r'^\t?([.\w$]+):')
INSN = re.compile(r'^\t([a-z_][a-z0-9_.]*)(?:\t(.*))?$')
SWITCH = re.compile(r'^\t\.(section|text|data|bss|pushsection|popsection|previous|subsection)\b')
LOCAL_REF = re.compile(r'\.L(?:C)?\d+(?:_LEND)?')

UNCOND = {'j', 'jx', 'ret', 'ret.n', 'retw', 'retw.n'}
INVERT = {}
for _a, _b in (('beq', 'bne'), ('blt', 'bge'), ('bltu', 'bgeu'), ('beqi', 'bnei'), ('blti', 'bgei'),
               ('bltui', 'bgeui'), ('beqz', 'bnez'), ('bltz', 'bgez'), ('bany', 'bnone'), ('ball', 'bnall'),
               ('bbc', 'bbs'), ('bbci', 'bbsi'), ('beqz.n', 'bnez.n'), ('bf', 'bt')):
    INVERT[_a] = _b; INVERT[_b] = _a
LOOPS = {'loop', 'loopnez', 'loopgtz'}

class Block:
    __slots__ = ('lines', 'insns', 'labels')
    def __init__(self):
        self.lines = []      # text lines, in order
        self.insns = []      # (index into lines, mnemonic, operands)
        self.labels = []     # code labels at its head
    def term(self):
        return self.insns[-1] if self.insns else None
    def falls_through(self):
        t = self.term()
        return t is None or t[1] not in UNCOND
    def cond_target(self):
        """The label a closing conditional branch jumps to, or None."""
        t = self.term()
        if t and t[1] in INVERT:
            return t[2].rsplit(',', 1)[-1].strip()
        return None

class Func:
    __slots__ = ('name', 'head', 'blocks', 'tail', 'index')
    def __init__(self, name):
        self.name = name
        self.head = []       # .section .. NAME: (inclusive)
        self.blocks = []
        self.tail = []       # .LFEn: and .size
    def hash(self):
        """Instructions with file-wide label numbers replaced by ordinals."""
        seen = {}
        def sub(m):
            return seen.setdefault(m.group(0), f'@{len(seen)}')
        h = hashlib.sha1()
        for b in self.blocks:
            for _, mn, ops in b.insns:
                h.update((mn + ' ' + LOCAL_REF.sub(sub, ops or '') + '\n').encode())
            h.update(b'--\n')
        return h.hexdigest()[:16]

def split_blocks(body):
    """Basic blocks of a function body: a new one at every code label and
    after every branch, jump or return. A zero-overhead loop (loop .. LEND)
    stays whole: its body must end exactly at the LEND label."""
    blocks = [Block()]
    lend = None            # inside a loop until this label
    island = False         # inside a jump table or other non-text lines
    after_term = False
    for line in body:
        b = blocks[-1]
        if island:
            b.lines.append(line)
            if FUNC_SECTION.match(line):
                island = False
            continue
        if SWITCH.match(line):
            island = True
            b.lines.append(line)
            continue
        if lend:
            m = LEND_LABEL.match(line)
            if m and m.group(1) == lend:
                lend = None
            mi = INSN.match(line)
            if mi and not line.startswith('\t.'):
                b.insns.append((len(b.lines), mi.group(1), mi.group(2) or ''))
            b.lines.append(line)
            continue
        m = CODE_LABEL.match(line)
        if m or after_term:
            if b.insns:
                b = Block(); blocks.append(b)
            after_term = False
        if m:
            b.labels.append(m.group(1))
            b.lines.append(line)
            continue
        mi = INSN.match(line) if not line.startswith('\t.') else None
        if mi:
            mn = mi.group(1)
            b.insns.append((len(b.lines), mn, mi.group(2) or ''))
            b.lines.append(line)
            if mn in LOOPS:
                lend = (mi.group(2) or '').rsplit(',', 1)[-1].strip()
            elif mn in UNCOND or mn in INVERT:
                after_term = True
            continue
        b.lines.append(line)
    if island or lend:
        raise ValueError('function body ends inside a section switch or a loop')
    if not blocks[-1].insns and len(blocks) > 1:       # trailing debug labels
        blocks[-2].lines += blocks[-1].lines
        blocks.pop()
    return blocks

def parse(lines):
    """-> list of str (a line kept as is) or Func, in file order."""
    out = []
    i, n = 0, len(lines)
    nf = 0
    while i < n:
        m = FUNC_SECTION.match(lines[i])
        if not m:
            out.append(lines[i]); i += 1
            continue
        name = m.group(1)
        # The function's label follows its literals; anything else (a second
        # .section first, a name that is not this section's) is not a
        # function this tool knows, and is passed through.
        j = i + 1
        while j < n and lines[j] != name + ':' and not FUNC_SECTION.match(lines[j]) and not SIZE.match(lines[j]):
            j += 1
        if j >= n or lines[j] != name + ':':
            out.append(lines[i]); i += 1
            continue
        k = j + 1
        while k < n and not (SIZE.match(lines[k]) and SIZE.match(lines[k]).group(1) == name):
            k += 1
        if k >= n:
            out.append(lines[i]); i += 1
            continue
        f = Func(name)
        f.head = lines[i:j + 1]
        body = lines[j + 1:k]
        # .LFEn: closes the function for the debug information.
        t = len(body)
        while t > 0 and re.match(r'^\.LFE\d+:$', body[t - 1]):
            t -= 1
        f.tail = body[t:] + [lines[k]]
        try:
            f.blocks = split_blocks(body[:t])
        except ValueError:
            out.extend(lines[i:k + 1]); i = k + 1
            continue
        f.index = nf; nf += 1
        out.append(f)
        i = k + 1
    return out

def sym(name, k):
    return f'__hc_{name}__{k}'

def gen(lines):
    out = []
    for it in parse(lines):
        if isinstance(it, str):
            out.append(it); continue
        out.extend(it.head)
        for k, b in enumerate(it.blocks):
            at = b.insns[0][0] if b.insns else len(b.lines)
            out.extend(b.lines[:at])
            if k:                       # block 0 is the function's own symbol
                out.append(sym(it.name, k) + ':')
            out.extend(b.lines[at:])
        out.extend(it.tail)
    return out

def hc_label(f, k):
    return f'.Lhc{f.index}_{k}'

# One section holds a group of partitioned functions, hot parts first. Every
# jump between a function's parts must reach: `j` spans +-131 KiB, so a group
# is closed before its code can exceed GROUP_LIMIT (an upper estimate: three
# bytes an instruction, six more for a branch the assembler has to relax).
GROUP_LIMIT = 110 * 1024

def estimate(f):
    n = sum(len(b.insns) for b in f.blocks)
    nc = sum(1 for b in f.blocks if b.term() and b.term()[1] in INVERT)
    return 3 * n + 6 * nc + 3 * len(f.blocks)

class Group:
    """The text of one section: one buffer a class, written out in class order.
    (Not assembler subsections: this gas dies in subseg_set_rest on a
    longcall's literal once a text section has a second subsection.)"""
    def __init__(self, number, pad=0, hot=1):
        self.section = f'{SECTION}{number}'
        self.number = number
        self.pad = pad
        self.hot = hot
        self.tiers = [[] for _ in range(hot + 2)]
        self.room = GROUP_LIMIT
    def open_line(self):
        return f'\t.section\t{self.section},"ax",@progbits'
    def lines(self):
        out = [f'# hc-group {self.number}', self.open_line()]
        if self.pad:
            # Measurement only: shift the hot code against the 32 B cache
            # lines, to tell a layout's effect from its alignment's luck.
            out.append(f'\t.space\t{self.pad}')
        for t in range(len(self.tiers)):
            out.append(f'# hc-tier {t}')
            out.extend(self.tiers[t])
        out.append(f'# hc-group-end {self.number}')
        return out

FILE_N = re.compile(r'^\t\.file \d+ ')

def hoist_files(lines):
    """GCC writes each `.file N "name"` just before the first `.loc N`. Text
    that moves could then use a number before its directive; put them all
    first. (`use` and `verify` both take the hoisted lines.)"""
    files = [l for l in lines if FILE_N.match(l)]
    rest = [l for l in lines if not FILE_N.match(l)]
    return rest[:1] + files + rest[1:]

def use(lines, profile, stats=None, pad=0, hot=1):
    """Lay out the functions the profile calls hot. A block's class is one
    base-36 digit of the profile's string: classes 0..hot-1 are hot (several,
    when the profile groups blocks by the workloads that use them), class
    `hot` is warm, `hot`+1 cold; they are emitted in that order. A function
    without a hot block (run in the training, but in no steady turn) is left
    as GCC wrote it: splitting it would only help a path that is not hot."""
    out = []                # str, or a Group where its section's text goes
    matched = missed = skipped = 0
    group = None
    ngroups = 0
    for it in parse(lines):
        if isinstance(it, str):
            out.append(it); continue
        p = profile.get(it.name)
        if p and (p['h'] != it.hash() or len(p['t']) != len(it.blocks)):
            missed += 1; p = None
        tier = [min(DIGITS.index(c), hot + 1) for c in p['t']] if p else None
        if p and min(tier) >= hot:
            skipped += 1; p = None
        if not p:
            out.extend(it.head); [out.extend(b.lines) for b in it.blocks]; out.extend(it.tail)
            continue
        matched += 1
        # The entry block is never colder than what it leads to in a turn;
        # if a profile says otherwise, keep the function symbol's part hot.
        tier[0] = min(tier)
        est = estimate(it)
        if group is None or (est > group.room and group.room != GROUP_LIMIT):
            group = Group(ngroups, pad, hot); ngroups += 1
            out.append(group)       # the section's text goes where its first function was
        group.room -= est
        emit_function(group, it, tier)
    flat = []
    for it in out:
        flat.extend(it.lines() if isinstance(it, Group) else [it])
    if stats is not None:
        stats.update(matched=matched, missed=missed, skipped=skipped, groups=ngroups)
    return flat

def emit_function(g, f, tier):
    nb = len(f.blocks)
    label_block = {}
    for k, b in enumerate(f.blocks):
        for l in b.labels:
            label_block[l] = k
    def next_same(k):
        for j in range(k + 1, nb):
            if tier[j] == tier[k]:
                return j
        return None
    opened = set()
    def buf(t):
        if t not in opened:
            opened.add(t)
            g.tiers[t].append(f'# hc-fn {f.name}')
            if t == tier[0]:
                # literals, alignment, the function's symbol (the original
                # .section line is the group's)
                g.tiers[t].extend(f.head[1:])
            else:
                g.tiers[t].append(f'\t.type\t{f.name}.{tier_name(t, g.hot)}, @function')
                g.tiers[t].append(f'{f.name}.{tier_name(t, g.hot)}:')
        return g.tiers[t]
    buf(tier[0])
    for k, b in enumerate(f.blocks):
        t = tier[k]
        # A jump table inside the block ends with ".section .text.NAME": back to ours.
        lines = [g.open_line() if FUNC_SECTION.match(l) else l for l in b.lines]
        extra = []
        if k + 1 < nb and tier[k + 1] != t and b.falls_through():
            tgt = b.cond_target()
            nxt = next_same(k)
            if tgt is not None and nxt is not None and label_block.get(tgt) == nxt:
                # "branch over the cold part": invert it, fall into the target.
                mn, ops = b.term()[1], b.term()[2]
                want = f'\t{mn}\t{ops}'
                at = max(i for i, l in enumerate(lines) if l == want)
                head, _ = ops.rsplit(',', 1)
                lines[at] = f'\t{INVERT[mn]}\t{head}, {hc_label(f, k + 1)}'
            else:
                extra = [f'\tj\t{hc_label(f, k + 1)}']
        o = buf(t)
        o.append(hc_label(f, k) + ':')
        o.extend(lines)
        o.extend(extra)
    # sizes: the function symbol covers its entry part, the others get their own.
    for t in sorted(opened):
        if t != tier[0]:
            g.tiers[t].append(f'\t.size\t{f.name}.{tier_name(t, g.hot)}, .-{f.name}.{tier_name(t, g.hot)}')
    g.tiers[tier[0]].extend(f.tail)

# ------------------------------------------------------------------ verify

HC_LABEL = re.compile(r'^\.Lhc(\d+)_(\d+):$')

def insn_list(lines):
    """Instructions of a run of lines, skipping jump tables and directives."""
    out = []
    island = False
    for l in lines:
        if island:
            if l.startswith('\t.section\t.text.'):
                island = False
            continue
        if SWITCH.match(l):
            island = True
            continue
        m = INSN.match(l)
        if m:
            out.append((m.group(1), m.group(2) or ''))
    return out

def verify(orig_lines, new_lines):
    """Raise ValueError unless every function inside the hc-group regions of
    new_lines is the original function with its blocks reordered and nothing
    else, and every line outside those regions is untouched. Returns the
    number of functions checked."""
    items = parse(orig_lines)
    funcs = {it.name: it for it in items if isinstance(it, Func)}
    parts = {}              # name -> {tier: lines}
    rest_new = []
    group = tier = name = None
    for l in new_lines:
        if l.startswith('# hc-group-end '):
            group = tier = name = None
        elif l.startswith('# hc-group '):
            if group is not None:
                raise ValueError('nested group')
            group = l
        elif group is None:
            rest_new.append(l)
        elif l.startswith('# hc-tier '):
            tier = int(l.split()[-1]); name = None
        elif l.startswith('# hc-fn '):
            name = l[len('# hc-fn '):]
            if name not in funcs or tier in parts.setdefault(name, {}):
                raise ValueError(f'{name}: not a function of the original, or two parts in one tier')
            parts[name][tier] = []
        elif name is None:
            if INSN.match(l):
                raise ValueError('an instruction outside every function part')
        else:
            parts[name][tier].append(l)
    for name, p in parts.items():
        check_function(funcs[name], p)
    rest_old = []
    for it in items:
        if isinstance(it, str):
            rest_old.append(it)
        elif it.name not in parts:
            rest_old.extend(it.head); [rest_old.extend(b.lines) for b in it.blocks]; rest_old.extend(it.tail)
    if rest_old != rest_new:
        raise ValueError('lines outside the partitioned functions changed')
    return len(parts)

def check_function(f, parts):
    name = f.name
    def fail(msg):
        raise ValueError(f'{name}: {msg}')
    order = {}                 # tier -> blocks in emission order
    body = {}                  # block -> its lines
    entry = None
    for t, lines in parts.items():
        cur = None
        for l in lines:
            m = HC_LABEL.match(l)
            if m:
                if int(m.group(1)) != f.index:
                    fail('a block label of another function')
                cur = int(m.group(2))
                if cur in body:
                    fail(f'block {cur} emitted twice')
                body[cur] = []
                order.setdefault(t, []).append(cur)
            elif cur is not None:
                body[cur].append(l)
            elif INSN.match(l):
                fail('an instruction in front of the first block of a part')
            elif l == name + ':':
                entry = t
    nb = len(f.blocks)
    if sorted(body) != list(range(nb)):
        fail(f'blocks {sorted(set(range(nb)) - set(body))} missing')
    if entry is None or order[entry][0] != 0:
        fail('the function symbol is not at its first block')
    for blocks in order.values():
        if blocks != sorted(blocks):
            fail('blocks of one tier out of their original order')
    label_at = {}              # code label -> (block, True if no instruction precedes it there)
    for k, ls in body.items():
        seen_insn = False
        for l in ls:
            m = CODE_LABEL.match(l)
            if m:
                if m.group(1) in label_at:
                    fail(f'label {m.group(1)} defined twice')
                label_at[m.group(1)] = (k, not seen_insn)
            elif INSN.match(l):
                seen_insn = True
    label_at.update({hc_label(f, k): (k, True) for k in range(nb)})
    nxt = {}                   # block -> the block the assembler places after it
    for blocks in order.values():
        for a, b in zip(blocks, blocks[1:]):
            nxt[a] = b
    for k, b in enumerate(f.blocks):
        old = [(mn, ops) for _, mn, ops in b.insns]
        new = insn_list(body[k])
        for l in b.labels:     # the labels it defined are still its own
            if label_at.get(l, (None,))[0] != k:
                fail(f'label {l} left block {k}')
        last = k + 1 >= nb
        falls = b.falls_through()
        if new == old:
            # Unchanged: what it fell into must still follow it. The last
            # block of the function may end without a jump (a call that does
            # not return); it ran into the next function before, and still
            # runs into something it never reaches.
            if falls and not last and nxt.get(k) != k + 1:
                fail(f'block {k} falls through into block {nxt.get(k)}, not {k + 1}')
            continue
        if falls and not last and new == old + [('j', hc_label(f, k + 1))]:
            continue
        if falls and not last and old and old[-1][0] in INVERT and new[:-1] == old[:-1]:
            mn, ops = old[-1]
            head, tgt = [x.strip() for x in ops.rsplit(',', 1)]
            if new[-1] == (INVERT[mn], f'{head}, {hc_label(f, k + 1)}'):
                # was: branch to tgt, else fall into k+1. Now: branch to k+1,
                # else fall into what follows, which must be tgt's block.
                tb = label_at.get(tgt)
                if tb and tb[1] and nxt.get(k) == tb[0]:
                    continue
                fail(f'block {k}: the inverted branch falls into block {nxt.get(k)}, its target {tgt} is at {tb}')
        fail(f'block {k}: instructions changed')

def main():
    mode = sys.argv[1]
    if mode == 'gen':
        src, dst = sys.argv[2:4]
        open(dst, 'w').write('\n'.join(gen(open(src).read().split('\n'))))
    elif mode == 'use':
        src, prof, dst = sys.argv[2:5]
        lines = hoist_files(open(src).read().split('\n'))
        st = {}
        prof = json.load(open(prof))
        new = use(lines, prof['files']['quickjs.c'], st, 0, prof.get('meta', {}).get('hot_classes', 1))
        st['verified'] = verify(lines, new)
        open(dst, 'w').write('\n'.join(new))
        print(f'hc_asm: {st["matched"]} functions partitioned and verified, {st["missed"]} profiles did not match')
    elif mode == 'hash':
        for it in parse(open(sys.argv[2]).read().split('\n')):
            if isinstance(it, Func):
                print(it.name, it.hash(), len(it.blocks))
    else:
        raise SystemExit(__doc__)

if __name__ == '__main__':
    main()
