"""The Shiki JIT's instruction table, from the assembler (docs/kasane/shiki-jit.md sec.4).

For each instruction form the JIT emits, assemble it with the toolchain that
builds the firmware (xtensa-esp32s3-elf-as), once with every operand at its
base value and once per operand bit set, and read off which instruction bit
each operand bit moves. The TRM's instruction-word diagrams disagree with the
assembler in places, so the table is not taken from them.

  python3 tools/shiki_jit/gen_encoding.py [--as PATH] [--check] OUT.h

--check also re-assembles every operand value in its range and compares
with the table's own encoding (slow; the CI form).
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile

# name, syntax with {0},{1}.. for operands, operands as (kind, lo, hi, scale)
# kind: q (0..7), a (AR 0..15), i (immediate: value = field * scale).
FORMS = [
    ('VLD128IP', 'ee.vld.128.ip q{0}, a{1}, {2}', [('q', 0, 7, 1), ('a', 0, 15, 1), ('i', -128, 127, 16)]),
    ('VST128IP', 'ee.vst.128.ip q{0}, a{1}, {2}', [('q', 0, 7, 1), ('a', 0, 15, 1), ('i', -128, 127, 16)]),
    ('VLDBC16', 'ee.vldbc.16 q{0}, a{1}', [('q', 0, 7, 1), ('a', 0, 15, 1)]),
    ('VADDS16', 'ee.vadds.s16 q{0}, q{1}, q{2}', [('q', 0, 7, 1)] * 3),
    ('VSUBS16', 'ee.vsubs.s16 q{0}, q{1}, q{2}', [('q', 0, 7, 1)] * 3),
    ('VMIN16', 'ee.vmin.s16 q{0}, q{1}, q{2}', [('q', 0, 7, 1)] * 3),
    ('VMAX16', 'ee.vmax.s16 q{0}, q{1}, q{2}', [('q', 0, 7, 1)] * 3),
    ('ZEROQACC', 'ee.zero.qacc', []),
    ('VMULAS16', 'ee.vmulas.s16.qacc q{0}, q{1}', [('q', 0, 7, 1)] * 2),
    ('VSMULAS16', 'ee.vsmulas.s16.qacc q{0}, q{1}, {2}', [('q', 0, 7, 1), ('q', 0, 7, 1), ('i', 0, 7, 1)]),
    ('SRCMB16', 'ee.srcmb.s16.qacc q{0}, a{1}, 0', [('q', 0, 7, 1), ('a', 0, 15, 1)]),
    ('LDQACCLL', 'ee.ld.qacc_l.l.128.ip a{0}, 16', [('a', 0, 15, 1)]),
    ('LDQACCLH', 'ee.ld.qacc_l.h.32.ip a{0}, 16', [('a', 0, 15, 1)]),
    ('LDQACCHL', 'ee.ld.qacc_h.l.128.ip a{0}, 16', [('a', 0, 15, 1)]),
    ('LDQACCHH', 'ee.ld.qacc_h.h.32.ip a{0}, -48', [('a', 0, 15, 1)]),
    ('STQACCLL', 'ee.st.qacc_l.l.128.ip a{0}, 16', [('a', 0, 15, 1)]),
    ('STQACCLH', 'ee.st.qacc_l.h.32.ip a{0}, 16', [('a', 0, 15, 1)]),
    ('STQACCHL', 'ee.st.qacc_h.l.128.ip a{0}, 16', [('a', 0, 15, 1)]),
    ('STQACCHH', 'ee.st.qacc_h.h.32.ip a{0}, -48', [('a', 0, 15, 1)]),
    ('L32I', 'l32i a{0}, a{1}, {2}', [('a', 0, 15, 1), ('a', 0, 15, 1), ('i', 0, 255, 4)]),
    ('ADDI', 'addi a{0}, a{1}, {2}', [('a', 0, 15, 1), ('a', 0, 15, 1), ('i', -128, 127, 1)]),
    ('LOOPGTZ', 'loopgtz a{0}, .+4+{1}', [('a', 0, 15, 1), ('i', 0, 255, 1)]),
    ('ENTRY32', 'entry a1, 32', []),
    ('RETWN', 'retw.n', []),
    ('NOPN', 'nop.n', []),
    ('NOP', 'nop', []),
]
BASE = {'q': 0, 'a': 2, 'i': 0}   # a2 as the base AR: a0/a1 are special to some forms


def assembler(path):
    if path:
        return path
    hits = glob.glob(os.path.expanduser('~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-as'))
    if not hits:
        sys.exit('gen_encoding: xtensa-esp32s3-elf-as not found (pass --as)')
    return sorted(hits)[-1]


def assemble_many(as_path, lines):
    """(bytes, length) of each line, in order: each instruction in its own
    16-aligned slot, its length read from objdump."""
    with tempfile.TemporaryDirectory() as tmp:
        src, obj = os.path.join(tmp, 'x.S'), os.path.join(tmp, 'x.o')
        body = ['.text']
        for l in lines:
            body += ['.align 16', l]
        open(src, 'w').write('\n'.join(body) + '\n')
        subprocess.run([as_path, '--no-transform', src, '-o', obj], check=True)
        dump = subprocess.run([as_path[:-2] + 'objdump', '-d', obj], check=True,
                              capture_output=True, text=True).stdout
    out = {}
    for line in dump.splitlines():
        parts = line.split('\t')
        if len(parts) < 3 or not parts[0].strip().endswith(':'):
            continue
        addr = int(parts[0].strip()[:-1], 16)
        if addr % 16:
            continue
        hexs = parts[1].strip()           # objdump prints the word most significant byte first
        n = len(hexs) // 2
        out[addr // 16] = (int(hexs, 16).to_bytes(n, 'big')[::-1], n)
    if len(out) != len(lines):
        sys.exit(f'gen_encoding: {len(lines)} lines but {len(out)} instructions')
    return [out[k] for k in range(len(lines))]


def build(as_path):
    table = []
    for name, syn, ops in FORMS:
        base_vals = [BASE[k] * 1 if k != 'i' else 0 for k, _, _, _ in ops]
        base_vals = [v if k != 'i' else 0 for v, (k, _, _, _) in zip(base_vals, ops)]
        lines = [syn.format(*[(v * s if k == 'i' else v) for v, (k, _, _, s) in zip(base_vals, ops)])]
        probes = []
        for j, (k, lo, hi, s) in enumerate(ops):
            bits = (hi - lo).bit_length() if k != 'i' or lo >= 0 else (hi - lo).bit_length()
            for bit in range(bits):
                vals = list(base_vals)
                field = 1 << bit
                if k == 'i' and lo < 0 and bit == bits - 1:
                    field = lo                       # the sign bit: the most negative value
                elif k != 'i':
                    field = base_vals[j] ^ (1 << bit)
                vals[j] = field
                lines.append(syn.format(*[(v * s if kk == 'i' else v) for v, (kk, _, _, s) in zip(vals, ops)]))
                probes.append((j, bit))
        enc = assemble_many(as_path, lines)
        word, length = int.from_bytes(enc[0][0], 'little'), enc[0][1]
        fields = []
        for (j, bit), (e, n) in zip(probes, enc[1:]):
            if n != length:
                sys.exit(f'gen_encoding: {name} changes length with operand {j}')
            diff = int.from_bytes(e, 'little') ^ word
            if bin(diff).count('1') != 1:
                sys.exit(f'gen_encoding: {name} operand {j} bit {bit} moved {bin(diff)}')
            fields.append((j, bit, diff.bit_length() - 1))
        table.append((name, syn, ops, word, length, fields, base_vals))
    return table


def emit_header(table, out):
    lines = ['/* Generated by tools/shiki_jit/gen_encoding.py from xtensa-esp32s3-elf-as; do not edit.',
             ' * Each form: the word with every operand at its base value (q0, a2, 0), its',
             ' * length in bytes, and for each operand bit the instruction bit it lands on.',
             ' * The encoder (ksn_shiki_jit.c) XORs in the bits that differ from the base. */',
             '#pragma once', '#include <stdint.h>', '']
    lines.append('typedef struct { uint8_t operand,bit,at; } ksn_jit_bit;')
    lines.append('typedef struct { uint32_t base; uint8_t len,nbits; int8_t base_op[3]; const ksn_jit_bit *bits; } ksn_jit_form;')
    lines.append('')
    for name, syn, ops, word, n, fields, base_vals in table:
        if fields:
            lines.append(f'static const ksn_jit_bit KSN_JIT_BITS_{name}[]={{' +
                         ','.join(f'{{{j},{b},{a}}}' for j, b, a in fields) + '};')
    lines.append('')
    lines.append('enum {' + ','.join(f'KSN_JIT_{t[0]}' for t in table) + ',KSN_JIT_FORMS};')
    lines.append('static const ksn_jit_form KSN_JIT_FORM[KSN_JIT_FORMS]={')
    for name, syn, ops, word, n, fields, base_vals in table:
        bv = (base_vals + [0, 0, 0])[:3]
        bits = f'KSN_JIT_BITS_{name}' if fields else '0'
        lines.append(f'    {{0x{word & ((1 << (8 * n)) - 1):08x}u,{n},{len(fields)},{{{bv[0]},{bv[1]},{bv[2]}}},{bits}}}, /* {syn} */')
    lines.append('};')
    open(out, 'w').write('\n'.join(lines) + '\n')


def encode(form, vals):
    """The table's encoding of vals (operand values as written, immediates
    already divided by their scale), as the C encoder does it."""
    name, syn, ops, word, n, fields, base_vals = form
    w = word
    for j, bit, at in fields:
        k, lo, hi, sc = ops[j]
        width = (hi - lo).bit_length()
        v = vals[j] & ((1 << width) - 1)
        b = base_vals[j] & ((1 << width) - 1)
        if ((v ^ b) >> bit) & 1:
            w ^= 1 << at
    return w.to_bytes(4, 'little')[:n]


def check(table, as_path):
    """Every q/a value and a spread of immediates, against the assembler."""
    import itertools
    bad = 0
    for form in table:
        name, syn, ops = form[0], form[1], form[2]
        choices = []
        for k, lo, hi, sc in ops:
            if k == 'i':
                choices.append(sorted({lo, hi, 0, 1, lo + 1, hi - 1, (lo + hi) // 2, 5, 37} & set(range(lo, hi + 1))))
            else:
                choices.append(list(range(lo, hi + 1)))
        combos = list(itertools.product(*choices)) if ops else [()]
        lines = [syn.format(*[(v * sc if k == 'i' else v) for v, (k, _, _, sc) in zip(c, ops)]) for c in combos]
        got = assemble_many(as_path, lines)
        for c, l, (b, n) in zip(combos, lines, got):
            if encode(form, list(c)) != b:
                bad += 1
                if bad < 10:
                    print(f'MISMATCH {l}: table {encode(form, list(c)).hex()} as {b.hex()}')
        print(f'{name}: {len(combos)} encodings checked')
    if bad:
        sys.exit(f'gen_encoding: {bad} mismatches')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--as', dest='as_path')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('out')
    a = ap.parse_args()
    as_path = assembler(a.as_path)
    table = build(as_path)
    if a.check:
        check(table, as_path)
    emit_header(table, a.out)


if __name__ == '__main__':
    main()
