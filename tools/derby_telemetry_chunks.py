"""Generate an opt-in diagnostic chunk; shipping JS is untouched."""
from pathlib import Path
import sys

def generate(source: Path, out: Path):
    out.mkdir(parents=True, exist_ok=True)
    play = (source / 'derby_play.js').read_text()
    marker = '      if (need) { build(up); need = 0; } else V.patch(up);'
    assert play.count(marker) == 1, 'frame completion marker changed'
    # Existing native function, primitive numbers only: no JS object/array/function.
    call = "\n  S.view(0,0,t,0,0,0,0,F.seed,raceNo,scene==='pad'?0:scene==='gate'?1:scene==='race'?2:scene==='photo'?3:4,scene==='photo'?4:scene==='pad'||scene==='res'?1:cm,rs?rnd(rs.t/DT):0,scene==='race'||scene==='gate'?(slow?disp:1):-1,tier);"
    target = out / 'derby_play.js'
    target.write_text(play.replace(marker, marker + call))
    lines = ['app local.derby']
    for line in (source / 'chunks.txt').read_text().splitlines():
        cols = line.split()
        if len(cols) == 2 and cols[0] not in ('#', 'app') and not line.lstrip().startswith('#'):
            lines.append(cols[0] + ' ' + str(target if cols[0] == 'play' else source / cols[1]))
    (out / 'chunks.txt').write_text('\n'.join(lines) + '\n')

if __name__ == '__main__':
    generate(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
