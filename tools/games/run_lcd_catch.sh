#!/usr/bin/env bash
# LCD CATCH on the host: the real QuickJS, pocket.kasane and pocket.input.keys
# (tools/games/test_lcd_catch_host.c). WSL/Linux, from anywhere in the repo:
#   bash tools/games/run_lcd_catch.sh             # scripted play, PASS/FAIL
#   bash tools/games/run_lcd_catch.sh --sheet     # also docs/apps/lcd-catch-preview.png
set -euo pipefail
cd "$(dirname "$0")/../.."
CACHE=${CACHE:-/tmp/qjs-lcdcatch-host}
mkdir -p "$CACHE"
python3 - "$CACHE/pocket_sub_impl.inc" <<'PY'
from pathlib import Path
import sys
s=Path('main/pocket/pocket_api.c').read_text()
a=s.index('// ----------------------------------------------------------- subscriptions')
b=s.index('// ------------------------------------------------------- async completions',a)
Path(sys.argv[1]).write_text(s[a:b])
PY
OUT=${OUT:-/tmp/test-lcd-catch} TEST_SOURCE=tools/games/test_lcd_catch_host.c CACHE="$CACHE" \
  EXTRA_SOURCES="main/pocket/pocket_input.c main/hal/keymap.c main/hal/keystate.c" \
  bash tools/build_kasane_test.sh >/dev/null
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:abort_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}
if [ "${1:-}" != "--sheet" ]; then exec "${OUT:-/tmp/test-lcd-catch}"; fi
SHOTS=$(mktemp -d)
LCD_PPM="$SHOTS" "${OUT:-/tmp/test-lcd-catch}"
python3 - "$SHOTS" docs/apps/lcd-catch-preview.png <<'PY'
# Panels in script order, 4 per row, 1:1 pixels, a 2 px grey gutter.
import struct, sys, zlib
from pathlib import Path
files = sorted(Path(sys.argv[1]).glob('*.ppm'))
head = b'P6\n240 135\n255\n'
imgs = [f.read_bytes()[len(head):] for f in files]
cols, w, h, gap = 4, 240, 135, 2
rows = (len(imgs) + cols - 1) // cols
W, H = cols * w + (cols - 1) * gap, rows * h + (rows - 1) * gap
grey = bytes((0x30, 0x30, 0x30))
raw = bytearray()
for y in range(H):
    raw.append(0)
    r, sy = divmod(y, h + gap)
    for c in range(cols):
        i = r * cols + c
        raw += grey * w if sy >= h or i >= len(imgs) else imgs[i][sy * w * 3:(sy + 1) * w * 3]
        if c + 1 < cols: raw += grey * gap
colours = set()
for im in imgs:
    colours |= {im[i:i + 3] for i in range(0, len(im), 3)}
chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
Path(sys.argv[2]).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
                              + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))
print('panels:', ' '.join(f.stem for f in files))
print(f'wrote {sys.argv[2]} ({W}x{H}); distinct colours across panels: {len(colours)}')
PY
rm -rf "$SHOTS"
