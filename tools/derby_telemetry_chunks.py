"""Generate an opt-in diagnostic chunk; shipping JS is untouched."""
from pathlib import Path
import sys

from derby_compare_chunks import instrument as compare_view

def instrument(play: str):
    play = play.replace('\r\n', '\n')
    marker = '      if (need) { build(up); need = 0; } else V.patch(up);'
    assert play.count(marker) == 1, 'frame completion marker changed'
    # Existing native function, primitive numbers only: no JS object/array/function.
    call = "\n  S.view(0,0,t,0,0,0,0,F.seed,raceNo,scene==='pad'?0:scene==='gate'?1:scene==='race'?2:scene==='photo'?3:4,scene==='photo'?4:scene==='pad'||scene==='res'?1:cm,rs?rnd(rs.t/DT):0,scene==='race'||scene==='gate'?(slow?disp:1):-1,tier);"
    return play.replace(marker, marker + call)

def generate(source: Path, out: Path, fixed_compare: bool = False):
    out.mkdir(parents=True, exist_ok=True)
    target = out / 'derby_play.js'
    target.write_bytes(instrument((source / 'derby_play.js').read_text()).encode('utf-8'))
    view_target = out / 'derby_view.js'
    if fixed_compare:
        view_target.write_bytes(compare_view((source / 'derby_view.js').read_text()).encode('utf-8'))
    else:
        view_target.unlink(missing_ok=True)
    lines = ['app local.derby']
    for line in (source / 'chunks.txt').read_text().splitlines():
        cols = line.split()
        if len(cols) == 2 and cols[0] not in ('#', 'app') and not line.lstrip().startswith('#'):
            path = target if cols[0] == 'play' else view_target if fixed_compare and cols[0] == 'view' else source / cols[1]
            lines.append(cols[0] + ' ' + str(path))
    (out / 'chunks.txt').write_text('\n'.join(lines) + '\n')

if __name__ == '__main__':
    if len(sys.argv) not in (3, 4) or len(sys.argv) == 4 and sys.argv[3] != '--compare-fixed':
        raise SystemExit('usage: derby_telemetry_chunks.py SOURCE OUT [--compare-fixed]')
    generate(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), len(sys.argv) == 4)
