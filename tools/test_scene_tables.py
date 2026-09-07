"""Diff the baked scene tables against the C that used to build them at boot.

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
    const float widths[]={18,5,24};const float brightness[]={14,32,21};
    for(int l=0;l<3;l++)for(int d=0;d<64;d++)
        printf("%d\n",(int)(uint8_t)(brightness[l]*expf(-d*d/(2*widths[l]*widths[l]))));
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

    want = gen.sine() + [v for row in gen.softness() for v in row]
    if len(want) != len(got):
        raise SystemExit(f'{len(want)} baked values against {len(got)} from C')

    worst, worst_at, differing = 0, None, 0
    for i, (a, b) in enumerate(zip(want, got)):
        d = abs(a - b)
        if d:
            differing += 1
        if d > worst:
            worst, worst_at = d, i
    table = 'sine' if worst_at is not None and worst_at < 256 else 'softness'
    print(f'sine+softness: {len(want)} entries, {differing} differ, '
          f'worst |delta| = {worst}' + (f' in {table}[{worst_at}]' if worst else ''))

    # wave_lut is packed from softness by integer arithmetic alone, so it is
    # exact once softness is; saying so beats leaving it unmentioned.
    print('wave_lut: integer packing of softness, no float of its own')
    if worst > 1:
        raise SystemExit(f'baked table differs from the C reference by {worst}: '
                         'that is a transcription error, not a libm difference')
    print('SCENE_TABLES_OK')


if __name__ == '__main__':
    main()
