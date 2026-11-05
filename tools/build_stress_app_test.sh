#!/usr/bin/env bash
# Builds tools/test_stress_app.c: apps/stress/stress.js against the real
# QuickJS, pocket.kasane and pocket.kasane.shiki (WSL, from the repo root).
# The app's Shiki programs (apps/stress/stress_prog.js) become the built-in
# table the firmware build makes (tools/kasane_ir/emit_shiki_rom.mjs); Node 16+.
#   bash tools/build_stress_app_test.sh && /tmp/test-stress-app
#   M32=1 bash tools/build_stress_app_test.sh && /tmp/test-stress-app-m32
# M32=1: the device's 8 B JSValue and 4 B pointers (tools/vmtest/m32_sysroot.sh),
# with ASan/UBSan; the 64-bit host lays objects out differently from the device.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ -n "${M32:-}" ]; then
  F=$(bash tools/vmtest/m32_sysroot.sh)
  export QJS_CFLAGS="$F" CFLAGS="$F -O1 -g -fsanitize=address,undefined"
  CACHE=${CACHE:-/tmp/qjs-kasane-host-m32}
  OUT=${OUT:-/tmp/test-stress-app-m32}
fi
CACHE=${CACHE:-/tmp/qjs-kasane-host}
mkdir -p "$CACHE"
node tools/kasane_ir/emit_shiki_rom.mjs "$CACHE/stress_shiki_rom.c" stress=apps/stress/stress_prog.js >/dev/null
OUT=${OUT:-/tmp/test-stress-app} TEST_SOURCE=tools/test_stress_app.c CACHE="$CACHE" \
  EXTRA_SOURCES="main/pocket/pocket_memory.c main/pocket/pocket_shiki_api.c $CACHE/stress_shiki_rom.c" \
  bash tools/build_kasane_test.sh
