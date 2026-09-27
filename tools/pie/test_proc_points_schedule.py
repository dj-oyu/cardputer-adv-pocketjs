"""Host check for the documented PIE block8 schedule; never touches hardware."""

import pathlib
import random
import unittest

from piesim import Sim, extract_asm, s16
from stalls import analyse, parse

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'main/ui/kasane/ksn_proc_points_pie.c'
NOTE = ROOT / 'docs/kasane/proc-pie-stall-options.md'
TERMS = ('neg_32768', 'xr', 'xa', 'xb', 'xc', 'm00', 'm01',
         'yr', 'ya', 'yb', 'yc', 'm10', 'm11')


def programs():
    # Keep the pre-scheduling baseline independent of the production source.
    original = (pathlib.Path(__file__).parent / 'fixtures/proc_points_original.asm').read_text(encoding='utf-8')
    candidate = NOTE.read_text(encoding='utf-8').split('```asm', 1)[1].split('```', 1)[0]
    return original, '\n'.join(line.split('//', 1)[0] for line in candidate.splitlines())


def operations(program):
    return [line.replace(',', ' ').split() for line in program.splitlines() if line.strip()]


def symbolic(program):
    q = {'q0': 'src.x', 'q1': 'src.y'}
    acc = None
    field = 0
    outputs = []
    events = []
    for ins in operations(program):
        op, *a = ins
        if op == 'ee.vld.128.ip':
            q[a[0]] = {'%[sx]': 'src.x', '%[sy]': 'src.y'}[a[1]]
            events.append(('source', a[1]))
        elif op == 'ee.vldbc.16.ip':
            assert a[1] == '%[p]' and a[2] == '2'
            q[a[0]] = TERMS[field]
            field += 1
        elif op == 'ee.mov.s16.qacc':
            acc = [q[a[0]]]
        elif op == 'ee.vmulas.s16.qacc':
            acc.append((q[a[0]], q[a[1]]))
        elif op == 'ee.srcmb.s16.qacc':
            assert a[1:] == ['%[shift]', '0']
            q[a[0]] = tuple(acc)
            acc = ('shifted', q[a[0]])
        elif op == 'ee.vst.128.ip':
            outputs.append((a[1], q[a[0]]))
            events.append(('store', a[1]))
        else:
            raise AssertionError(op)
    assert field == len(TERMS)
    assert events[:2] == [('source', '%[sx]'), ('source', '%[sy]')]
    assert [e for e in events[2:]] == [('store', '%[dx]'), ('store', '%[dy]')]
    return outputs


def terms(t):
    quotient = abs(t) // 32768 * (1 if t >= 0 else -1)
    rest = t - quotient * 32768
    q = -quotient
    out = [rest]
    for _ in range(3):
        part = max(-32768, min(32767, q))
        out.append(part)
        q -= part
    assert q == 0
    return out


def put16(mem, address, value):
    v = value & 0xffff
    mem[address:address + 2] = bytes((v & 255, v >> 8))


class ProcPointsSchedule(unittest.TestCase):
    def test_static_qr_load_use_prediction(self):
        original, candidate = programs()
        self.assertEqual(len(parse(original)), 31)
        self.assertEqual(len(parse(candidate)), 31)
        self.assertEqual(len(analyse(parse(original))), 12)
        self.assertEqual(analyse(parse(candidate)), [])

    def test_production_schedule_is_verified(self):
        _, candidate = programs()
        actual = operations(extract_asm(SOURCE, 'static void block8'))
        self.assertEqual(actual, operations(candidate))

    def test_symbolic_stream_and_qacc(self):
        original, candidate = programs()
        x, y = symbolic(original), symbolic(candidate)
        self.assertEqual(x, y)
        self.assertEqual(x[0][1], ('xr', ('xa', 'neg_32768'), ('xb', 'neg_32768'),
                                   ('xc', 'neg_32768'), ('src.x', 'm00'), ('src.y', 'm01')))
        self.assertEqual(x[1][1], ('yr', ('ya', 'neg_32768'), ('yb', 'neg_32768'),
                                   ('yc', 'neg_32768'), ('src.x', 'm10'), ('src.y', 'm11')))

    def test_simulator_against_scalar(self):
        original, candidate = programs()
        rng = random.Random(0x5150)
        edge = [-2147483648, -1073741824, -32768, -1, 0, 1, 32767,
                1073741824, 2147483647]
        for case in range(512):
            sx = [rng.randint(-32768, 32767) for _ in range(8)]
            sy = [rng.randint(-32768, 32767) for _ in range(8)]
            if case < 8:
                sx[case] = [-32768, 32767][case % 2]
                sy[case] = [32767, -32768][case % 2]
            m = [rng.randint(-32768, 32767) for _ in range(4)]
            tx = edge[case % len(edge)] if case < len(edge) else rng.randint(-2147483648, 2147483647)
            ty = edge[-1 - case % len(edge)] if case < len(edge) else rng.randint(-2147483648, 2147483647)
            coefficients = [-32768] + terms(tx) + m[:2] + terms(ty) + m[2:]
            expected_x = [max(-32768, min(32767, (m[0] * x + m[1] * y + tx) // 16384))
                          for x, y in zip(sx, sy)]
            expected_y = [max(-32768, min(32767, (m[2] * x + m[3] * y + ty) // 16384))
                          for x, y in zip(sx, sy)]
            for inplace in (False, True):
                results = []
                for program in (original, candidate):
                    mem = bytearray(0x600)
                    for base, values in ((0x100, sx), (0x200, sy), (0x500, coefficients)):
                        for i, value in enumerate(values):
                            put16(mem, base + 2*i, value)
                    dx, dy = (0x100, 0x200) if inplace else (0x300, 0x400)
                    sim = Sim(mem)
                    sim.run(program, {'sx': 0x100, 'sy': 0x200, 'p': 0x500,
                                      'dx': dx, 'dy': dy, 'shift': 14})
                    got = ([s16(sim.ld16(dx + 2*i)) for i in range(8)],
                           [s16(sim.ld16(dy + 2*i)) for i in range(8)])
                    self.assertEqual(got, (expected_x, expected_y), (case, inplace))
                    self.assertEqual(sim.ar['p'], 0x500 + 2*len(TERMS))
                    results.append(got)
                self.assertEqual(results[0], results[1])


if __name__ == '__main__':
    unittest.main()
