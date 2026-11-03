#!/usr/bin/env bash
# Builds tools/test_stress_app.c: apps/stress/stress.js against the real
# QuickJS, pocket.kasane and pocket.kasane.shiki (WSL, from the repo root).
# The app's Shiki programs (apps/stress/stress_prog.js) become the built-in
# table the firmware build makes (tools/kasane_ir/emit_shiki_rom.mjs); Node 16+.
#   bash tools/build_stress_app_test.sh && /tmp/test-stress-app
set -euo pipefail
cd "$(dirname "$0")/.."
CACHE=${CACHE:-/tmp/qjs-kasane-host}
mkdir -p "$CACHE"
node tools/kasane_ir/emit_shiki_rom.mjs "$CACHE/stress_shiki_rom.c" stress=apps/stress/stress_prog.js >/dev/null
OUT=${OUT:-/tmp/test-stress-app} TEST_SOURCE=tools/test_stress_app.c CACHE="$CACHE" \
  EXTRA_SOURCES="main/pocket/pocket_memory.c main/pocket/pocket_shiki_api.c $CACHE/stress_shiki_rom.c" \
  bash tools/build_kasane_test.sh
