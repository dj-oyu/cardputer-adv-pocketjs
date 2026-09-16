"""Keep the lazy JS scene controller identical in firmware and host tests."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
source = (root / 'apps/kasane/create_scene.js').read_bytes()
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
(out / 'kasane_scene_js.h').write_text(
    '#pragma once\nstatic const char KSN_SCENE_FACTORY[] = {\n' +
    ',\n'.join(','.join(str(b) for b in source[i:i+32]) for i in range(0, len(source), 32)) +
    ',0\n};\n', encoding='utf-8')
