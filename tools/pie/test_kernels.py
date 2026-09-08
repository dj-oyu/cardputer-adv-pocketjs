"""Run the project's PIE kernels through the instruction-level model and compare
them with their scalar definitions.

    python tools/pie/test_kernels.py           (from the repository root)

Each test extracts the inline assembly straight out of main/scene/ocean.c, main/scene/wave.c or
main/scene/render_accel.c, builds the same memory the C code would (tables, per-row
constants, the 8-column input blocks), executes the assembly with `piesim`, and
compares every output pixel with a Python transcription of the scalar
reference next to the kernel (ocean_row_scalar in ocean.c,
blend_px in render_accel.c, which is the Rust blend_rgb565 formula).

What this catches: a wrong register in a rescheduled kernel, a constant read in
the wrong order, an off-by-one in a lookup lane, a pointer that does not advance
by what the caller assumes. What it does not catch: an algorithm that is itself
wrong for some input — that is the job of the exhaustive C models in models/,
and of the `__attribute__((unused))` scalar functions they were written from.

The constant arrays of the ocean and wave kernels are evaluated from the C
initializer text, so reordering k[] in the source is checked automatically.
The blend constants come from blend_constants() in render_accel.c, which has
no initializer to parse; blend_k() below mirrors it and must be kept in step.
"""
import math
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from piesim import Sim, extract_asm, extract_constants, store16, store32, load16  # noqa: E402

ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
# The two vector rows left shell.c when each background became its own
# file; the assembly moved verbatim, so only these paths changed.
OCEAN = os.path.join(ROOT, 'main', 'scene', 'ocean.c')
WAVE = os.path.join(ROOT, 'main', 'scene', 'wave.c')
ACCEL = os.path.join(ROOT, 'main', 'scene', 'render_accel.c')
LCD_W = 240


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def cdiv(a, b):
    """C integer division (truncates toward zero)."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


# The tables are built here exactly as shell.c builds them. Python's double
# precision may differ from sinf/expf by one unit in a few entries; that does
# not matter because both sides of the comparison use the same table.
SINE = [int(math.sin(i * 6.2831853 / 256) * 256) for i in range(256)]
WIDTHS, BRIGHT = (18, 5, 24), (14, 32, 21)
SOFTNESS = [[int(BRIGHT[l] * math.exp(-d * d / (2 * WIDTHS[l] * WIDTHS[l]))) for d in range(64)]
            for l in range(3)]


class OceanRow(unittest.TestCase):
    """ocean_row_pie against ocean_row_scalar (scene/ocean.c)."""

    @staticmethod
    def scalar(depth, cross, span, haze, distortion):
        out = []
        for x in range(LCD_W):
            bend = distortion[x]
            swell = SINE[(depth + bend) & 255]
            ripple = SINE[(cross + x * 2 + bend * 2) & 255]
            crest = max(swell - 180 + cdiv(ripple, 6), 0)
            dx = abs(x - 160)
            reflection = (span - dx) * 128 // span if dx < span else 0
            glint = crest * (40 + reflection) // 128
            shade = (swell + 256) // 32
            lift = shade + haze + glint
            clamp = lambda v: min(max(v, 0), 255)
            out.append(rgb565(clamp(3 + glint), clamp(20 + lift), clamp(39 + lift)))
        return out

    def test_rows(self):
        with open(OCEAN, encoding='utf-8') as f:
            src = f.read()
        exact = 'ocean_sine16' in src       # table kernel (bit-exact) or the parabola kernel
        asm = extract_asm(OCEAN, 'ocean_row_pie(')
        rng = random.Random(3)
        moved, worst, total = 0, [0, 0, 0], 0
        for _ in range(40):
            y = rng.randint(37, 134)
            span, haze = 12 + (y - 36) // 3, 24 - (y - 36) // 5
            depth, cross = rng.randint(0, 200000), rng.randint(0, 200000)
            distortion = [rng.randint(-28, 28) for _ in range(LCD_W)]
            mem = bytearray(1 << 17)
            ROW, COLS, TA, TB, K, KV = 0x1000, 0x2000, 0x4000, 0x4400, 0x5000, 0x5100
            scale = 1 if exact else 16     # build_columns(): the parabola kernel keeps its phases x16
            for x in range(LCD_W):        # ocean_cols[b][0..2][i]
                b, i = x >> 3, x & 7
                planes = (distortion[x] * scale, (2 * x + 2 * distortion[x]) * scale, abs(x - 160))
                for l, v in enumerate(planes):
                    store16(mem, COLS + (b * 3 + l) * 16 + 2 * i, [v & 0xFFFF])
            store32(mem, TA, [s * 16 for s in SINE])                        # ocean_sine16
            store32(mem, TB, [cdiv(s, 6) * 16 - 180 * 16 for s in SINE])     # ocean_sine6
            k = extract_constants(OCEAN, 'ocean_row_pie(', dict(depth=depth, cross=cross, span=span, haze=haze))
            store16(mem, K, k)
            sim = Sim(mem)
            sim.run(asm, {'row': ROW, 'in': COLS, 'k': K, 'kv': KV, 'k8': KV + 16 * (len(k) - 1),
                          'ta': TA, 'tb': TB, 'zero': 0, 's15': 15, 'nk': len(k),
                          'blocks': LCD_W // 8, 'sar': 11})
            got, want = load16(mem, ROW, LCD_W), self.scalar(depth, cross, span, haze, distortion)
            if exact:
                self.assertEqual(got, want, f'row y={y} depth={depth} cross={cross}')
            else:
                for g, w in zip(got, want):
                    total += 1
                    moved += g != w
                    for c, (sh, m) in enumerate(((11, 31), (5, 63), (0, 31))):
                        worst[c] = max(worst[c], abs(((g >> sh) & m) - ((w >> sh) & m)))
            self.assertEqual(sim.ar['row'], ROW + LCD_W * 2)
        if not exact:
            # The parabola kernel's own contract (see its comment in shell.c): about one
            # pixel in ten moves by one RGB565 step, none by more than two in green.
            self.assertLessEqual(worst[0], 1, 'red moved by more than one step')
            self.assertLessEqual(worst[1], 2, 'green moved by more than two steps')
            self.assertLessEqual(worst[2], 1, 'blue moved by more than one step')
            self.assertLess(moved / total, 0.2, f'{moved}/{total} pixels differ from the scalar row')
            print(f'ocean (approximate kernel): {moved}/{total} pixels moved, worst step r/g/b = {worst}')


class WaveRow(unittest.TestCase):
    """wave_row_pie against its scalar model (scene/wave.c)."""

    @staticmethod
    def scalar(y, ribbons):
        green, blue = 14 + y // 7, 30 + y // 5
        out = []
        for x in range(LCD_W):
            light = []
            for l in range(3):
                d = abs(y - ribbons[l][x])
                light.append(SOFTNESS[l][d] if d < 64 else 0)
            s = light[0] + light[1]
            out.append(rgb565(5 + light[0] // 4 + light[1] // 3 + light[2] // 2,
                              green + s + light[2] // 2, blue + s + light[2]))
        return out

    def test_rows(self):
        asm = extract_asm(WAVE, 'wave_row_pie(')
        rng = random.Random(5)
        for _ in range(40):
            y = rng.randint(0, 134)
            ribbons = [[rng.randint(-40, 200) for _ in range(LCD_W)] for _ in range(3)]
            mem = bytearray(1 << 16)
            ROW, COLS, T, K = 0x1000, 0x2000, 0x4000, 0x6000
            for x in range(LCD_W):
                for l in range(3):
                    store16(mem, COLS + ((x >> 3) * 3 + l) * 16 + 2 * (x & 7), [ribbons[l][x] & 0xFFFF])
            lut = []
            for l in range(3):                            # wave_lut[l][d] = light | (weighted << 16), [64] = 0
                for d in range(65):
                    lo = SOFTNESS[l][d] if d < 64 else 0
                    hi = (lo // 4, lo // 3, lo // 2)[l]
                    lut.append(lo | (hi << 16))
            store32(mem, T, lut)
            k = extract_constants(WAVE, 'wave_row_pie(', dict(y=y, green=14 + y // 7, blue=30 + y // 5))
            store16(mem, K, k)
            sim = Sim(mem)
            sim.run(asm, {'row': ROW, 'in': COLS, 'k': K, 't0': T, 't1': T + 65 * 4, 't2': T + 130 * 4,
                          'blocks': LCD_W // 8, 'sar': 11})
            self.assertEqual(load16(mem, ROW, LCD_W), self.scalar(y, ribbons), f'row y={y}')
            self.assertEqual(sim.ar['row'], ROW + LCD_W * 2)


def blend_k(r, g, b):
    """Mirror of blend_constants() plus the pack constants in accel_blend()."""
    k = [1, 64, 63, 31, 16384, 512, 8192, 128]
    for s in (r, g, b):
        k += [s, 0x00FF, 1, 127, 128]
    k += [0x00F8, 0x8000, 0x00FC, 16384, 256]
    return k


class BlendBlocks(unittest.TestCase):
    """blend_blocks_pie against blend_px (render_accel.c) = the Rust software blend."""

    @staticmethod
    def blend_px(p, r, g, b, a):
        r5, g6, b5 = (p >> 11) & 31, (p >> 5) & 63, p & 31
        dr, dg, db = (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)
        ia = 255 - a
        return rgb565((r * a + dr * ia + 127) // 255, (g * a + dg * ia + 127) // 255,
                      (b * a + db * ia + 127) // 255)

    def test_blocks(self):
        asm = extract_asm(ACCEL, 'blend_blocks_pie(')
        rng = random.Random(7)
        for _ in range(200):
            n = rng.randint(1, 30)
            px = [rng.getrandbits(16) for _ in range(n * 8)]
            mask = [rng.choice((0, 255, rng.getrandbits(8), rng.getrandbits(8))) for _ in range(n * 8)]
            r, g, b = (rng.getrandbits(8) for _ in range(3))
            mem = bytearray(1 << 16)
            DST, MASK, K = 0x1000, 0x3000, 0x4000
            store16(mem, DST, px)
            mem[MASK:MASK + n * 8] = bytes(mask)
            store16(mem, K, blend_k(r, g, b))
            sim = Sim(mem)
            sim.run(asm, {'d': DST, 'm': MASK, 'n': n, 'k': K, 'sar': 11, 'sh8': 8})
            self.assertEqual(load16(mem, DST, n * 8), [self.blend_px(p, r, g, b, a) for p, a in zip(px, mask)])
            self.assertEqual((sim.ar['d'], sim.ar['m'], sim.ar['n']), (DST + n * 16, MASK + n * 8, 0))


class FillBlocks(unittest.TestCase):
    def test_blocks(self):
        asm = extract_asm(ACCEL, 'fill_blocks_pie(')
        mem = bytearray(1 << 12)
        store16(mem, 0x800, [0xBEEF])
        sim = Sim(mem)
        sim.run(asm, {'d': 0x100, 'c': 0x800, 'n': 5})
        self.assertEqual(load16(mem, 0x100, 40), [0xBEEF] * 40)
        self.assertEqual(load16(mem, 0x100 + 80, 1), [0])


class SolarFillRow(unittest.TestCase):
    def test_rows(self):
        asm = extract_asm(os.path.join(ROOT, 'main', 'scene', 'solar_sail.c'), 'fill_row(')
        rng = random.Random(19)
        for color in [0, 0xFFFF, 0xF800, 0x07E0, 0x001F] + [rng.getrandbits(16) for _ in range(128)]:
            mem = bytearray([0xA5] * 4096)
            store16(mem, 0x802, [color])  # Scalar broadcast needs only 2-byte alignment.
            sim = Sim(mem)
            sim.run(asm, {'out': 0x100, 'c': 0x802, 'n': LCD_W // 8})
            self.assertEqual(load16(mem, 0x100, LCD_W), [color] * LCD_W)
            self.assertEqual(sim.ar['out'], 0x100 + LCD_W * 2)
            self.assertEqual(mem[0xF0:0x100], bytes([0xA5] * 16))
            self.assertEqual(mem[0x100 + LCD_W * 2:0x110 + LCD_W * 2], bytes([0xA5] * 16))


GARDEN = os.path.join(ROOT, 'main', 'scene', 'garden.c')
# main/scene/garden.c, garden_dither and GARDEN_M3.
DKX, DKY, DKC, DKM, M3 = 18453, 26253, 17872, 42589, 21846


class GardenRow(unittest.TestCase):
    """The three garden kernels against the lane model beside them in garden.c.

    The reference below is a transcription of garden_octave_lanes and
    garden_pixels, not of garden_row_scalar: those two are an approximation of
    the scalar loop by design (reciprocals instead of divisions, two floors
    where there was one), and the size of that approximation is measured
    separately, in tools/test_garden.c, against the scalar loop itself. What is
    checked here is the other half -- that the assembly is that lane model, with
    those registers, reading those constants in that order.

    Row parameters are drawn at random from the ranges garden_pixels_row can
    actually produce for y in 0..134, so the kernels are exercised well past the
    one frame a fixed row would give.
    """

    @staticmethod
    def recip(d, sh):
        return ((1 << sh) + d - 1) // d

    @staticmethod
    def runs(p, step):
        """The block runs garden_octave_row cuts the row into."""
        out, b = [], 0
        while b < 30:
            g = b * 8 * step + p
            n = min((255 - (g & 255)) // (step * 8) + 1, 30 - b)
            out.append((b, n, g & 255, g >> 8))
            b += n
        return out

    @classmethod
    def dens_ref(cls, vc, vf, p6, p5):
        dens = [0] * 240
        for v, mask, p, step, first in ((vc, 3, p6, 4, True), (vf, 7, p5, 8, False)):
            for b, n, rc0, cc in cls.runs(p, step):
                a = v[cc & mask]
                da = v[(cc + 1) & mask] - a
                ddb = v[(cc + 2) & mask] - v[(cc + 1) & mask] - da
                for i in range(n):
                    for l in range(8):
                        rc = rc0 + step * (i * 8 + l)
                        t = rc & 255
                        fx = ((t * t) * (768 - 2 * t)) >> 16
                        nm = -1 if rc > 255 else 0
                        lo = a + (da & nm)
                        hi = lo + da + (ddb & nm)
                        d = (lo * (256 - fx) + hi * fx) >> 8
                        x = (b + i) * 8 + l
                        dens[x] = 3 * d if first else dens[x] + d
        return dens

    @staticmethod
    def pixels_ref(dens, r):
        out = []
        for x in range(LCD_W):
            u = max(-r['width'], min(r['width'], x - r['center']))
            q = 256 - ((((u * u) & 0xFFFF) * r['mww']) >> 14)
            S = dens[x]
            amb = r['ay'] + ((S * 1366) >> 16)
            k = r['kb'] + ((S * 820) >> 16)
            hz = 320 + S
            sun = (((q * k) & 0xFFFF) * q) >> 16
            for at, w, m, gain in ((r['at0'], r['w0'], r['mw0'], 4),
                                   (r['at1'], r['w1'], r['mw1'], 7)):
                d = max(-w, min(w, x - at))
                sh = 256 - ((((d * d) & 0xFFFF) * m) >> 14)
                sun += (((((sh * gain) & 0xFFFF) * sh) >> 8) * hz) >> 18
            h = ((x * DKX) ^ r['dy']) & 0xFFFF
            dd = ((((h * h) >> 17) * DKM) >> 16) & 3
            cr = ((amb * 128 + sun * 640) >> 8) + dd + 10
            cg = ((amb * 192 + sun * 384) >> 8) + dd + 22
            cb = ((amb + ((sun * M3) >> 16) + dd + 28) * 32) >> 8
            out.append((((cr * 256) & 0xF800) | ((cg * 8) & 0x07E0)) | cb)
        return out

    KV, RCV, XV, DENS, ROW, K = 0x1000, 0x2000, 0x2100, 0x3000, 0x4000, 0x5000

    def broadcast(self, mem, values):
        """garden_broadcast, run rather than emulated, so the table the loops
        walk is the one the device would have."""
        store16(mem, self.K, values)
        Sim(mem).run(extract_asm(GARDEN, 'garden_broadcast('),
                     {'k': self.K, 'kv': self.KV, 'ks': 0, 'kp': 0, 'nk': len(values)})

    def octave(self, mem, name, env, n, rc0, step, a, da, ddb, dens):
        k = extract_constants(GARDEN, name, dict(env, a=a, da=da, ddb=ddb))
        self.broadcast(mem, k)
        store16(mem, self.RCV, [(rc0 + step * i) & 0xFFFF for i in range(8)])
        ar = {'kp': 0, 'kv': self.KV, 'rp': self.RCV, 'dens': dens, 'dp': dens,
              'n': n, 'sh8': 8, 'sh16': 16, 'zero': 0}
        sim = Sim(mem)
        sim.run(extract_asm(GARDEN, name), ar)
        self.assertEqual(sim.ar['dens'], dens + n * 16)

    def test_row(self):
        rng = random.Random(31)
        for _ in range(24):
            y = rng.randrange(135)
            width, w0, w1 = 58 + y // 3, 10 + y // 13, 10 + y // 8
            r = {'center': rng.randrange(60, 200), 'width': width,
                 'mww': self.recip(width * width, 22),
                 'w0': w0, 'mw0': self.recip(w0 * w0, 22),
                 'w1': w1, 'mw1': self.recip(w1 * w1, 22),
                 'ay': 20 + y // 15, 'kb': 17 + rng.randint(-5, 5),
                 'dy': (y * DKY + DKC) & 0xFFFF}
            r['at0'], r['at1'] = r['center'] - 18, r['center'] + 27
            vc = [rng.randrange(256) for _ in range(4)]
            vf = [rng.randrange(256) for _ in range(8)]
            p6, p5 = rng.randrange(1024), rng.randrange(2048)

            mem = bytearray(1 << 16)
            env = dict(center=r['center'], width=width, mww=r['mww'],
                       at0=r['at0'], w0=w0, mw0=r['mw0'],
                       at1=r['at1'], w1=w1, mw1=r['mw1'],
                       ay=r['ay'], kb=r['kb'], dy=r['dy'],
                       GARDEN_DKX=DKX, GARDEN_DKM=DKM, GARDEN_M3=M3)
            for v, mask, p, step, name in ((vc, 3, p6, 4, 'garden_coarse_pie('),
                                           (vf, 7, p5, 8, 'garden_fine_pie(')):
                for b, n, rc0, cc in self.runs(p, step):
                    a = v[cc & mask]
                    da = v[(cc + 1) & mask] - a
                    ddb = v[(cc + 2) & mask] - v[(cc + 1) & mask] - da
                    self.octave(mem, name, env, n, rc0, step, a, da, ddb,
                                self.DENS + b * 16)
            dens = self.dens_ref(vc, vf, p6, p5)
            self.assertEqual(load16(mem, self.DENS, LCD_W), [d & 0xFFFF for d in dens])

            self.broadcast(mem, extract_constants(GARDEN, 'garden_pixels_pie(', env))
            store16(mem, self.XV, list(range(8)))
            # Both spellings of the constant walk, against the same reference.
            # Fusion changes when a constant is fetched and never which one, so
            # anything but identical output means the transformation is not the
            # one GARDEN_PIE_FUSE claims -- and the failure it is most likely to
            # catch is a load that moved past another and swapped two constants,
            # which no amount of reading the diff would show.
            for fuse in (True, False):
                store16(mem, self.XV, list(range(8)))
                sim = Sim(mem)
                sim.run(extract_asm(GARDEN, 'garden_pixels_pie(', fuse),
                        {'kp': 0, 'kv': self.KV, 'xp': self.XV, 'dens': self.DENS,
                         'row': self.ROW, 'cnt': 30,
                         'sh8': 8, 'sh14': 14, 'sh16': 16, 'sh17': 17, 'sh18': 18,
                         'zero': 0})
                self.assertEqual(load16(mem, self.ROW, LCD_W), self.pixels_ref(dens, r),
                                 'fused' if fuse else 'unfused')
                self.assertEqual((sim.ar['row'], sim.ar['dens'], sim.ar['cnt']),
                                 (self.ROW + LCD_W * 2, self.DENS + LCD_W * 2, 0))



if __name__ == '__main__':
    unittest.main()
