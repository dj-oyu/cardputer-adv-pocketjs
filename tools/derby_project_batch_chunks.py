"""Compose an opt-in projection PAN chunk with ordinary/diagnostic chunks."""
from pathlib import Path
import sys
from games.derby_project_batch import instrument

def generate(source:Path,out:Path):
    out.mkdir(parents=True,exist_ok=True);lines=['app local.derby'];found=False
    for line in (source/'chunks.txt').read_text().splitlines():
        cols=line.split()
        if len(cols)!=2 or cols[0]=='app' or line.lstrip().startswith('#'):continue
        path=(source/cols[1]).resolve()
        if cols[0]=='pan':
            target=out/'derby_pan.js';target.write_text(instrument(path.read_text()));path=target;found=True
        lines.append(cols[0]+' '+str(path))
    if not found:raise ValueError('PAN chunk not found')
    (out/'chunks.txt').write_text('\n'.join(lines)+'\n')

if __name__=='__main__':generate(Path(sys.argv[1]).resolve(),Path(sys.argv[2]).resolve())
