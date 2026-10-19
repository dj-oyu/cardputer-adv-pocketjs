"""Compose the opt-in runner chunk with original or diagnostic chunk lists."""
from pathlib import Path
import sys

from games.derby_runner_batch import instrument


def generate(source: Path, out: Path):
    out.mkdir(parents=True, exist_ok=True)
    lines = ['app local.derby']
    found = False
    for line in (source / 'chunks.txt').read_text().splitlines():
        cols = line.split()
        if len(cols) != 2 or cols[0] == 'app' or line.lstrip().startswith('#'):
            continue
        path = (source / cols[1]).resolve()
        if cols[0] == 'pan':
            target = out / 'derby_pan.js'
            target.write_bytes(instrument(path.read_text(encoding='utf-8')).encode('utf-8'))
            path = target
            found = True
        lines.append(cols[0] + ' ' + str(path))
    if not found:
        raise ValueError('PAN chunk not found')
    (out / 'chunks.txt').write_bytes(('\n'.join(lines) + '\n').encode('utf-8'))


if __name__ == '__main__':
    generate(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
