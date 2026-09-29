#!/usr/bin/env bash
# Builds tools/test_keytest_app.c: apps/keytest/keytest.js against the real
# QuickJS, pocket.kasane and pocket.input.keys (WSL, from the repo root).
#   bash tools/build_keytest_app_test.sh && /tmp/test-keytest-app
set -euo pipefail
cd "$(dirname "$0")/.."
CACHE=${CACHE:-/tmp/qjs-kasane-host}
mkdir -p "$CACHE"
python3 - "$CACHE/pocket_sub_impl.inc" <<'PY'
from pathlib import Path
import sys
s=Path('main/pocket/pocket_api.c').read_text()
a=s.index('// ----------------------------------------------------------- subscriptions')
b=s.index('// ------------------------------------------------------- async completions',a)
Path(sys.argv[1]).write_text(s[a:b])
PY
OUT=${OUT:-/tmp/test-keytest-app} TEST_SOURCE=tools/test_keytest_app.c CACHE="$CACHE" \
  EXTRA_SOURCES="main/pocket/pocket_input.c main/hal/keymap.c main/hal/keystate.c ${KASANE_EXTRA:-}" \
  bash tools/build_kasane_test.sh
