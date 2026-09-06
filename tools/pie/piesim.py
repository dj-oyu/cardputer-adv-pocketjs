"""Instruction-level model of the ESP32-S3 PIE kernels used in this project.

The simulator executes the inline-assembly text of a kernel exactly as written
in the C source (see `extract_asm`) against a flat byte array, with the lane
semantics of the ESP32-S3 Technical Reference Manual, chapter 1.8. It exists so
that a kernel can be checked for *register-allocation and scheduling mistakes*
on the host before it is flashed: the arithmetic of a kernel is proven
separately by the exhaustive C models in `models/`, and this file proves that
the assembly actually implements that arithmetic with the registers it claims.

It is a functional model only. It knows nothing about timing; see `stalls.py`
for the pipeline side.

Supported instructions (TRM section in brackets):
    wsr.sar, mov, addi, bnez, loopgtz                         (Xtensa core)
    ee.vld.128.ip [1.8.88]   ee.vst.128.ip [1.8.192]  ee.vld.l.64.ip [1.8.92]
    ee.vldbc.16 [1.8.94]     ee.vldbc.16.ip [1.8.95]  ee.ldxq.32 [1.8.37]
    ee.vadds.s16 [1.8.70]    ee.vsubs.s16 [1.8.198]   ee.vmax.s16 [1.8.104]
    ee.vmin.s16 [1.8.113]    ee.vcmp.lt.s16 [1.8.85]  ee.vrelu.s16 [1.8.184]
    ee.vmul.s16 [1.8.122]    ee.vmul.u16 [1.8.128]
    ee.andq [1.8.1]          ee.orq [1.8.45]          ee.xorq [1.8.214]
    ee.vunzip.16 [1.8.207]   ee.vzip.8 [1.8.212]      ee.zero.q [1.8.216]
    ee.zero.qacc [1.8.217]   ee.vmulas.u16.qacc [1.8.163]  ee.srcmb.s16.qacc [1.8.54]
    ee.vprelu.s16 [1.8.182]  and the fused forms ee.vadds.s16.ld.incp [1.8.71],
    ee.vsubs.s16.ld.incp [1.8.199], ee.vmul.s16.ld.incp [1.8.123], ee.vmul.u16.ld.incp [1.8.129]
Anything else raises NotImplementedError, on purpose: add the instruction here
from its TRM pseudocode before relying on a kernel that uses it.

Lane convention: q registers are lists of eight unsigned 16-bit lanes, lane 0
being bits 15:0 (little-endian, the order EE.VLD.128 reads memory in). QACC is
eight 40-bit lanes. Alignment is *forced* the way the hardware does it (low
address bits cleared), so a misaligned pointer silently reads the wrong place
here too — which is the behaviour a test should catch.
"""
import re


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def sat16(v):
    return max(-32768, min(32767, v)) & 0xFFFF


ALU = ('ee.vadds.s16', 'ee.vsubs.s16', 'ee.vmax.s16', 'ee.vmin.s16', 'ee.vcmp.lt.s16',
       'ee.andq', 'ee.orq', 'ee.xorq', 'ee.vmul.s16', 'ee.vmul.u16')


class Sim:
    def __init__(self, mem):
        self.mem = mem
        self.q = [[0] * 8 for _ in range(8)]
        self.qacc = [0] * 8
        self.sar = 0
        self.ar = {}
        self.count = 0

    # ---- memory ----
    def ld16(self, a):
        return self.mem[a] | (self.mem[a + 1] << 8)

    def ld32(self, a):
        return self.ld16(a) | (self.ld16(a + 2) << 16)

    def ldq(self, a):
        a &= ~15
        return [self.ld16(a + 2 * i) for i in range(8)]

    def stq(self, a, v):
        a &= ~15
        for i in range(8):
            self.mem[a + 2 * i] = v[i] & 0xFF
            self.mem[a + 2 * i + 1] = (v[i] >> 8) & 0xFF

    # ---- lane arithmetic shared by the plain and the fused (.LD.INCP) forms ----
    def _alu(self, op, x, y):
        out = []
        for i in range(8):
            if op == 'ee.vadds.s16':
                out.append(sat16(s16(x[i]) + s16(y[i])))
            elif op == 'ee.vsubs.s16':
                out.append(sat16(s16(x[i]) - s16(y[i])))
            elif op == 'ee.vmax.s16':
                out.append(max(s16(x[i]), s16(y[i])) & 0xFFFF)
            elif op == 'ee.vmin.s16':
                out.append(min(s16(x[i]), s16(y[i])) & 0xFFFF)
            elif op == 'ee.vcmp.lt.s16':
                out.append(0xFFFF if s16(x[i]) < s16(y[i]) else 0)
            elif op == 'ee.andq':
                out.append(x[i] & y[i])
            elif op == 'ee.orq':
                out.append(x[i] | y[i])
            elif op == 'ee.xorq':
                out.append(x[i] ^ y[i])
            elif op == 'ee.vmul.s16':      # 32-bit product, arithmetic >> SAR, low 16
                out.append(((s16(x[i]) * s16(y[i])) >> self.sar) & 0xFFFF)
            elif op == 'ee.vmul.u16':      # 32-bit product, logical >> SAR, low 16
                out.append(((x[i] * y[i]) >> self.sar) & 0xFFFF)
            else:
                raise NotImplementedError(op)
        return out

    # ---- execution ----
    def run(self, asm, ar):
        """Run `asm` (the text of one __asm__ block) with the named address
        registers in `ar` (e.g. {'row': 0x1000, 'k': 0x2000, 'blocks': 30}).
        Returns the number of instructions executed; `self.ar` holds the
        registers afterwards so a test can check pointer post-increments."""
        self.ar = dict(ar)
        lines = []
        for raw in asm.splitlines():
            t = raw.split('/*')[0].strip()
            if t:
                lines.append(t)
        labels = {t[:-1]: i for i, t in enumerate(lines) if re.fullmatch(r'[0-9A-Za-z_.]+:', t)}
        pc, loop = 0, None
        while pc < len(lines):
            t = lines[pc]
            pc += 1
            if t.endswith(':'):
                if loop and loop[0] == t[:-1]:      # end of a loopgtz body
                    loop[1] -= 1
                    if loop[1] > 0:
                        pc = loop[2]
                    else:
                        loop = None
                continue
            self.count += 1
            op, *a = t.replace(',', ' ').split()
            Q = self.q

            def qi(x):
                return int(x[1])

            def arv(x):
                return self.ar[x.strip('%[]')]

            def arset(x, v):
                self.ar[x.strip('%[]')] = v

            if op == 'wsr.sar':
                self.sar = arv(a[0]) & 63
            elif op == 'mov':
                arset(a[0], arv(a[1]))
            elif op == 'addi':
                arset(a[0], arv(a[1]) + int(a[2]))
            elif op == 'bnez':
                if arv(a[0]) != 0:
                    pc = labels[a[1][:-1]] + 1
            elif op == 'loopgtz':
                n, lbl = arv(a[0]), a[1][:-1]
                if n <= 0:
                    pc = labels[lbl] + 1
                else:
                    loop = [lbl, n, pc]
            elif op == 'ee.vld.128.ip':
                Q[qi(a[0])] = self.ldq(arv(a[1]))
                arset(a[1], arv(a[1]) + int(a[2]))
            elif op == 'ee.vst.128.ip':
                self.stq(arv(a[1]), Q[qi(a[0])])
                arset(a[1], arv(a[1]) + int(a[2]))
            elif op == 'ee.vldbc.16.ip':
                Q[qi(a[0])] = [self.ld16(arv(a[1]) & ~1)] * 8
                arset(a[1], arv(a[1]) + int(a[2]))
            elif op == 'ee.vldbc.16':
                Q[qi(a[0])] = [self.ld16(arv(a[1]) & ~1)] * 8
            elif op == 'ee.vld.l.64.ip':
                ad, q = arv(a[1]) & ~7, Q[qi(a[0])]
                for i in range(4):
                    q[i] = self.ld16(ad + 2 * i)
                arset(a[1], arv(a[1]) + int(a[2]))
            elif op == 'ee.ldxq.32':
                qu, qs, base, sel4, sel8 = qi(a[0]), qi(a[1]), arv(a[2]), int(a[3]), int(a[4])
                v = self.ld32((base + Q[qs][sel8] * 4) & ~3)
                Q[qu][2 * sel4] = v & 0xFFFF
                Q[qu][2 * sel4 + 1] = v >> 16
            elif op == 'ee.vunzip.16':
                both = Q[qi(a[0])] + Q[qi(a[1])]
                Q[qi(a[0])], Q[qi(a[1])] = both[0::2], both[1::2]
            elif op == 'ee.vzip.8':
                b0 = [b for v in Q[qi(a[0])] for b in (v & 0xFF, v >> 8)]
                b1 = [b for v in Q[qi(a[1])] for b in (v & 0xFF, v >> 8)]
                z = [x for pair in zip(b0, b1) for x in pair]
                Q[qi(a[0])] = [z[2 * i] | (z[2 * i + 1] << 8) for i in range(8)]
                Q[qi(a[1])] = [z[16 + 2 * i] | (z[16 + 2 * i + 1] << 8) for i in range(8)]
            elif op == 'ee.zero.q':
                Q[qi(a[0])] = [0] * 8
            elif op == 'ee.zero.qacc':
                self.qacc = [0] * 8
            elif op in ALU:
                Q[qi(a[0])] = self._alu(op, Q[qi(a[1])], Q[qi(a[2])])
            elif op.endswith('.ld.incp'):
                # EE.<op>.LD.INCP qu, as, qa, qx, qy: qa = op(qx, qy) from the values
                # before the load, then qu = load128(aligned(as)), as += 16
                # (1.8.71 VADDS, 1.8.199 VSUBS, 1.8.123 VMUL.S16, 1.8.129 VMUL.U16).
                base = op[:-len('.ld.incp')]
                qu, qa, x, y = qi(a[0]), qi(a[2]), Q[qi(a[3])], Q[qi(a[4])]
                Q[qa] = self._alu(base, x, y)
                Q[qu] = self.ldq(arv(a[1]))
                arset(a[1], arv(a[1]) + 16)
            elif op == 'ee.vprelu.s16':
                # qz[i] = (qx[i] <= 0) ? (qx[i]*qy[i]) >> ay[5:0] : qx[i]      (1.8.182)
                x, y, ay = Q[qi(a[1])], Q[qi(a[2])], arv(a[3]) & 63
                Q[qi(a[0])] = [x[i] if s16(x[i]) > 0 else ((s16(x[i]) * s16(y[i])) >> ay) & 0xFFFF
                               for i in range(8)]
            elif op == 'ee.vrelu.s16':
                ax, ay, q = arv(a[1]) & 0xFFFF, arv(a[2]) & 63, Q[qi(a[0])]
                Q[qi(a[0])] = [q[i] if s16(q[i]) > 0 else ((s16(q[i]) * s16(ax)) >> ay) & 0xFFFF
                               for i in range(8)]
            elif op == 'ee.vmulas.u16.qacc':
                x, y = Q[qi(a[0])], Q[qi(a[1])]
                self.qacc = [min(self.qacc[i] + x[i] * y[i], (1 << 40) - 1) for i in range(8)]
            elif op == 'ee.srcmb.s16.qacc':
                n = arv(a[1]) & 63
                self.qacc = [v >> n for v in self.qacc]
                Q[qi(a[0])] = [sat16(v) for v in self.qacc]
            else:
                raise NotImplementedError(op)
        return self.count


# ---- pulling kernels and their constants out of the C sources ----

def _strip_comments(text):
    return re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)


_STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')


def extract_asm(path, func):
    """Return the assembly text of the first `__asm__ volatile(` after the
    definition of `func` in the C file at `path`: the concatenation of its
    string literals up to the operand list (the first ':' that is outside a
    string literal and outside a comment)."""
    with open(path, encoding='utf-8') as f:
        src = f.read()
    i = src.index(func)
    k = src.index('__asm__ volatile(', i) + len('__asm__ volatile(')
    out, depth = [], 1
    while k < len(src):
        c = src[k]
        if src.startswith('/*', k):                  # comment: skip, whatever it contains
            k = src.index('*/', k) + 2
        elif c == '"':
            m = _STRING.match(src, k)
            out.append(m.group(1).encode().decode('unicode_escape'))
            k = m.end()
        elif c == ':':
            break
        elif c == '(':
            depth += 1
            k += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                break
            k += 1
        else:
            k += 1
    return ''.join(out)


def extract_constants(path, func, env):
    """Evaluate the `int16_t k[N] = { ... };` initializer inside `func` with the
    C variables given in `env` (e.g. depth, cross, span, haze). C casts are
    dropped and `/` is integer division; every element in the current kernels
    is non-negative before a division, which is what makes that exact."""
    with open(path, encoding='utf-8') as f:
        src = f.read()
    i = src.index(func)
    m = re.compile(r'int16_t\s+k\[\d*\][^=]*=\s*\{(.*?)\};', re.S).search(src, i)
    body = _strip_comments(m.group(1))
    items, depth, cur = [], 0, ''
    for c in body:
        if c == ',' and depth == 0:
            items.append(cur)
            cur = ''
            continue
        depth += (c == '(') - (c == ')')
        cur += c
    items.append(cur)
    vals = []
    for it in items:
        it = re.sub(r'\(\s*int16_t\s*\)', '', it).replace('/', '//').strip()
        if it:
            vals.append(int(eval(it, {}, dict(env))) & 0xFFFF)
    return vals


def store16(mem, addr, values):
    for i, v in enumerate(values):
        mem[addr + 2 * i] = v & 0xFF
        mem[addr + 2 * i + 1] = (v >> 8) & 0xFF


def store32(mem, addr, values):
    for i, v in enumerate(values):
        v &= 0xFFFFFFFF
        for n in range(4):
            mem[addr + 4 * i + n] = (v >> (8 * n)) & 0xFF


def load16(mem, addr, count):
    return [mem[addr + 2 * i] | (mem[addr + 2 * i + 1] << 8) for i in range(count)]
