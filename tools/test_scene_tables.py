"""Diff the baked sine table against the C that used to build it at boot, and
range-check the LEVEL WAVE tables the row kernel reads.

tools/make_scene_tables.py transcribes shell.c's build_tables() into Python.
Python's math.sin and math.exp are double routines rounded to float; the device
called sinf and expf. Those are allowed to differ by an ulp, and after the
multiply and the truncation an ulp can move an entry by one.

One is invisible in a sine table driving a 5/6/5 panel. The point of this
script is that nobody has to take that on faith: it compiles the original
expressions with the host's own libm and prints how far apart the two actually
are, the way tools/test_sfx.py does for the click tables.

    python tools/test_scene_tables.py

Exits non-zero if any entry differs by more than one, which would mean the
transcription is wrong rather than the libm.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_scene_tables as gen  # noqa: E402

# build_tables(), verbatim, so the comparison is against the code that ran.
REFERENCE = r'''
#include <math.h>
#include <stdio.h>
#include <stdint.h>
int main(void) {
    for(int i=0;i<256;i++) printf("%d\n",(int)(int16_t)(sinf(i*6.2831853f/256)*256));
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'ref.c'
        exe = Path(tmp) / 'ref'
        src.write_text(REFERENCE, encoding='utf-8')
        cc = ['cc', '-O2', '-o', str(exe), str(src), '-lm']
        print('$ ' + ' '.join(cc))
        subprocess.run(cc, check=True)
        got = [int(v) for v in subprocess.run(
            [str(exe)], check=True, capture_output=True, text=True).stdout.split()]

    want = gen.sine()
    if len(want) != len(got):
        raise SystemExit(f'{len(want)} baked values against {len(got)} from C')

    worst, worst_at, differing = 0, None, 0
    for i, (a, b) in enumerate(zip(want, got)):
        d = abs(a - b)
        if d:
            differing += 1
        if d > worst:
            worst, worst_at = d, i
    print(f'sine: {len(want)} entries, {differing} differ, '
          f'worst |delta| = {worst}' + (f' at [{worst_at}]' if worst else ''))

    # The LEVEL WAVE tables (wave_lut, wave_water_*) have no C predecessor to
    # diff against: they were redrawn in the generator itself. What can still
    # go wrong is the range the row kernel depends on, so check that.
    lut = gen.wave_lut()
    assert len(lut) == 6 and all(len(t) == 256 for t in lut)
    for t in lut:
        assert t[255] == 0, 'the clamped distance must read as no light'
        for v in t:
            assert (v & 0xFFFF) <= 32640 and (v >> 16) <= 32640
    body, light = gen.water()
    assert all(0 <= v <= 32767 for row in body + light for v in row)
    print('wave tables: in range for the saturating row (<= 255*128 per entry)')
    if worst > 1:
        raise SystemExit(f'baked table differs from the C reference by {worst}: '
                         'that is a transcription error, not a libm difference')
    print('SCENE_TABLES_OK')


if __name__ == '__main__':
    main()
