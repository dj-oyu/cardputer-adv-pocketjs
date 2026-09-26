#!/bin/bash
# F1 negative controls (docs/vm/builtin-floor-plan.md sec.8, N1a-N1c): build
# deliberately broken copies of the engine and require the corpus to notice
# each fault -- and NOT to notice the one change that must be harmless (a
# table that lacks the context-created names). A detector is trusted only
# after it has been shown to fire. WSL:
#   bash tools/vmtest/floor/f1_faults.sh
set -uo pipefail
cd "$(dirname "$0")/../../.."
QJS=components/quickjs-ng/quickjs-ng
BASE=.cache/vmtest-faults
GEN=.cache/vmtest-floor/gen/gen_rom_atoms
[ -x "$GEN" ] || bash tools/vmtest/floor/gen_rom_atoms.sh --check >/dev/null
status=0
run_case() {  # name expect(FAIL|PASS) [fault]
  local name=$1 expect=$2 fault=${3:-}
  local dir=$BASE/$name
  rm -rf "$dir"; mkdir -p "$dir/qjs"
  cp $QJS/*.c $QJS/*.h "$dir/qjs/"
  if [ -n "$fault" ]; then
    python3 tools/vmtest/floor/f1_faults.py "$fault" "$dir/qjs/quickjs.c" >/dev/null || { echo "$name: patch failed"; status=1; return; }
  fi
  if [ "$name" = stale-table ]; then
    ROM_NO_EXTRAS=1 "$GEN" "$dir/qjs/quickjs-rom-atoms.h" >/dev/null
  fi
  VMTEST_QJS=$dir/qjs VMTEST_OUT=$PWD/$dir/out bash tools/vmtest/build.sh asan-rom >"$dir/build.log" 2>&1 \
    || { echo "$name: build failed"; tail -3 "$dir/build.log"; status=1; return; }
  local line fails f pinned
  line=$(VMTEST_OUT=$PWD/$dir/out bash tools/vmtest/run.sh --variant asan-rom 2>&1 | grep '^corpus')
  # The files pinned to an allocation number ("// vmrun-rom[-...]-flags:") are
  # re-pinned for the real table; a table with fewer names shifts them, so a
  # failure there says nothing about stale-table and is left out of its
  # verdict. Every other file has to pass.
  fails=$(sed -n 's/.*failed (\(.*\))$/\1/p' <<< "$line")
  local got=PASS
  for f in $fails; do
    pinned=$(head -n10 "tools/vmtest/corpus/$f.js" | grep -cE '^// vmrun-rom(-[a-z-]+)?-flags:')
    if [ "$name" != stale-table ] || [ "$pinned" = 0 ]; then got=FAIL; fi
  done
  if [ "$got" = "$expect" ]; then
    echo "OK   $name: $line"
  else
    echo "BAD  $name: expected $expect, got: $line"; status=1
  fi
}
run_case stale-table     PASS                   # N1a: fallback to heap atoms
run_case no-rom-find     FAIL no-rom-find       # identity
run_case no-value-branch FAIL no-value-branch   # N1b: a forgotten site
run_case no-numeric-flag FAIL no-numeric-flag   # N1c: the baked answer
exit $status
