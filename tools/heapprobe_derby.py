"""DERBY WATCH as one script, for the evaluation heap probe's variants.

POCKET_HEAPPROBE only (docs/vm/spread-eval-oom.md, main/CMakeLists.txt). The
probe's spread, flat, split3/4 and load3/4 variants measure one program
evaluated as one script or cut into N; they were made from DERBY WATCH as it
was before it became chunks (apps/heapprobe/derby_v0, frozen at e15e6eb). That
copy drew with the float VM, which Shiki v2.0 removed, so the variants are now
made from the shipped game instead:

  - the entry (apps/derby/derby_watch.js) up to its pocket.app.load() line,
    then every chunk of apps/derby/chunks.txt in its order, the prog chunk with
    its @shiki functions removed as the firmware build does
    (tools/kasane_ir/lower_plans.mjs; the programs are in the built-in table);
  - the demo chunk's lazy pocket.app.load('demo') becomes a no-op, since the
    demo is already in the script;
  - all of it inside `(function () { 'use strict'; ... })();` and indented two
    spaces, the shape tools/heapprobe_split.py unwraps and cuts at top-level
    statements. The chunks' top-level names are already distinct (they share
    one realm when loaded), so one function scope holds them all.

  python tools/heapprobe_derby.py NODE OUT.js
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / 'apps/derby'
LOAD = "  pocket.app.load('prog'), pocket.app.load('view'), pocket.app.load('scene'), pocket.app.load('pan'), pocket.app.load('play');"


def chunks():
    out = []
    for raw in (APP / 'chunks.txt').read_text(encoding='utf-8').splitlines():
        words = raw.split('#', 1)[0].split()
        if len(words) == 2 and words[0] not in ('app', 'option'):
            out.append((words[0], APP / words[1]))
    return out


def main():
    node, out = sys.argv[1], Path(sys.argv[2])
    entry = (APP / 'derby_watch.js').read_text(encoding='utf-8').split('\n')
    at = entry.index(LOAD)
    if entry[at - 1] != "if (typeof pocket !== 'undefined')":
        sys.exit('heapprobe_derby: the entry no longer ends with its chunk loads')
    parts = [entry[:at - 1]]
    for name, path in chunks():
        text = path.read_text(encoding='utf-8')
        if name == 'prog':
            with tempfile.TemporaryDirectory() as tmp:
                lowered = Path(tmp) / path.name
                subprocess.run([node, str(ROOT / 'tools/kasane_ir/lower_plans.mjs'), '--rom', 'derby',
                                '--file', str(path), str(lowered)], check=True)
                text = lowered.read_text(encoding='utf-8')
        if name == 'play':
            if text.count("pocket.app.load('demo')") != 1:
                sys.exit("heapprobe_derby: play no longer loads the demo chunk once")
            text = text.replace("pocket.app.load('demo')", '0')
        parts.append(text.split('\n'))
    body = ['(function () {', "  'use strict';", "  if (typeof pocket === 'undefined') return;"]
    for lines in parts:
        body += ['  ' + l if l.strip() else '' for l in lines]
    body.append('})();')
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text('\n'.join(body) + '\n', encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
