"""Bake the home screen's constant scene tables into flash.

sine and the first wave tables were built once by shell.c's build_tables() and
never written again. build_tables() reads nothing -- no tilt, no clock, no
setting -- so all three are functions of literals, and 1,484 B of .bss was
being spent on values a compiler could have known. Emitted as `const` they land
in .rodata: flash, reached through the instruction cache, costing no DRAM.

Same shape as tools/make_sfx.py, and for the same reason. The arithmetic is a
transcription of build_tables(), not a re-derivation of it: f32() rounds after
every operation so the float steps happen in the same order and at the same
precision the device used to use. Two libm implementations do not have to
return the same sinf, so tools/test_scene_tables.py measures what is
left over rather than asserting it is zero.

The fourth table build_tables() filled, ocean_cols plane 2 (|x-160|), is not
here: it is constant too, but it lives inside the ocean's 16-byte-aligned
column block and cannot be split out of it. It stays a 240-store loop on the
rebuild path, which is where the rest of that block is filled anyway.

tools/pie/test_kernels.py imports wave_lut() from here rather than rebuilding
it, so the tables the kernel simulation compares against are the same values
the device holds. wave_lut is no longer a transcription of anything: it was
redrawn for the neon LEVEL WAVE and is defined here, not in C.
"""
import struct
import sys
import math
from pathlib import Path

LCD_H = 135


def f32(x):
    """Round to single precision, as every step of the C reference does."""
    return struct.unpack('<f', struct.pack('<f', x))[0]


def sinf(x):
    return f32(math.sin(f32(x)))


def sine():
    """sine[i] = (int16_t)(sinf(i*6.2831853f/256)*256), truncating toward zero."""
    return [int(f32(sinf(f32(f32(i * f32(6.2831853)) / f32(256))) * f32(256)))
            for i in range(256)]


# LEVEL WAVE's three glowing lines (main/scene/wave.c): each is a sum of
# gaussians across the line, as (sigma in pixels, (r, g, b) at the centre).
# 0 and 2 are the aurora in the sky, 1 is the lit edge of the water surface.
# A wide halo under a narrow core is what makes a line read as light rather
# than as paint; where two cores cross, the sum clips to white.
WAVE_LAYERS = (
    ((16.0, (28, 18, 80)), (1.1, (70, 90, 190))),     # far aurora: violet haze, pale thread
    ((5.5, (0, 70, 95)), (0.85, (150, 255, 255))),    # water surface: cyan filament
    ((13.0, (85, 6, 75)), (1.3, (255, 110, 210))),    # near aurora: magenta
)
# The row kernel measures distance in quarter pixels and clamps it to 255, so
# each table is 256 entries and the last one is zero ("further than 63.75 px").
WAVE_STEPS = 4
WAVE_LIMIT = 255
# Values are channel*128: 255*128 = 32640, so the kernel's saturating 16-bit add
# clips a sum of lights at 255.99 instead of wrapping (wave.c's row comment).
WAVE_SCALE = 128


def wave_light(layer):
    """[(r, g, b)] * 256 for one line, in channel*128 units."""
    out = []
    for i in range(WAVE_LIMIT + 1):
        d = i / WAVE_STEPS
        rgb = [0.0, 0.0, 0.0]
        if i < WAVE_LIMIT:
            for sigma, colour in WAVE_LAYERS[layer]:
                k = math.exp(-d * d / (2 * sigma * sigma))
                for c in range(3):
                    rgb[c] += colour[c] * k
        out.append(tuple(min(255 * WAVE_SCALE, int(round(v * WAVE_SCALE))) for v in rgb))
    return out


def wave_lut():
    """Six tables of 256 words, in the order the row kernel walks them:
    blue|green<<16 for line 0, red for line 0, then lines 1 and 2 the same.

    One pointer walks all six by `addmi 1024`: 256 words is exactly 1 KiB, so
    the step is a multiple of 256 and fits addmi's immediate. That is why the
    distance clamps at 255 rather than 256 -- a 257th entry would need a
    second address register per table, and the kernel has none to spare.
    """
    out = []
    for l in range(3):
        light = wave_light(l)
        out.append([b | (g << 16) for r, g, b in light])
        out.append([r for r, g, b in light])
    return out


# The water under LEVEL WAVE, by depth below the local surface in rows, in
# channel*128 and blue, green, red order (the light plane's). Red is absorbed
# first and green next, so the transmitted light runs from white-cyan under
# the surface to deep blue; the body of the water darkens the same way.
WATER_BODY = (46.0, 26.0, 1.0)        # b, g, r just under the surface
# Beer-Lambert: a shaft loses a fixed fraction of itself per row, red first.
# Short enough that the rays visibly die out before the bottom of the screen
# (50-90 rows of water): by 20 rows blue is at 1/e, green near 1/5, red gone,
# and by 60 rows even blue is down to 5%.
WATER_REACH = (20.0, 12.0, 6.0)       # rows to fall to 1/e, b, g, r
WATER_SUN = (240.0, 215.0, 120.0)     # b, g, r of one full ray at depth 0


def water():
    body, light = [], []
    for d in range(LCD_H + 1):
        body.append([int(WAVE_SCALE * (WATER_BODY[c] * (0.25 + 0.75 * math.exp(-d / 70.0))
                                       + (6 if c == 0 else 0))) for c in range(3)])
        # 0.85 at the surface rather than 0.55 so the top stays bright while
        # the depth takes the light away much faster.
        light.append([int(WAVE_SCALE * WATER_SUN[c] * 0.85 * math.exp(-d / WATER_REACH[c]))
                      for c in range(3)])
    return body, light


def rows(values, per_line=16):
    return ',\n'.join(','.join(str(v) for v in values[i:i + per_line])
                      for i in range(0, len(values), per_line))


def emit(name, values, ctype, dims=None):
    """One table, with the declared shape kept so every index in the kernels
    reads unchanged. Nested tables get their inner braces written out rather
    than relying on brace elision: the firmware builds with -Wmissing-braces
    promoted to an error, so elision is a build failure here, not a style."""
    if dims is None:
        return f'static const {ctype} {name}[{len(values)}] = {{\n{rows(values)}\n}};\n'
    shape = ''.join(f'[{d}]' for d in dims)
    inner = ',\n'.join('{\n' + rows(row) + '\n}' for row in values)
    return f'static const {ctype} {name}{shape} = {{\n{inner}\n}};\n'


def generate(out):
    out.mkdir(parents=True, exist_ok=True)
    text = ('// Generated by tools/make_scene_tables.py. Do not edit.\n'
            '#pragma once\n#include <stdint.h>\n')
    text += emit('sine', sine(), 'int16_t')
    # The kernel's lookups need the 4-byte alignment uint32_t already gives.
    text += emit('wave_lut', wave_lut(), 'uint32_t', (6, 256))
    body, light = water()
    text += emit('wave_water_body', body, 'int16_t', (LCD_H + 1, 3))
    text += emit('wave_water_light', light, 'int16_t', (LCD_H + 1, 3))
    (out / 'scene_tables.h').write_text(text, encoding='utf-8')


if __name__ == '__main__':
    generate(Path(sys.argv[1]))
