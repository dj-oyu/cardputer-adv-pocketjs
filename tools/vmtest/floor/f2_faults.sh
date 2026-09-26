#!/bin/bash
# F2 negative controls (docs/vm/builtin-floor-plan.md sec.8): build broken
# copies of the engine and require the corpus to notice each fault. A
# detector is trusted only after it has been shown to fire. WSL:
#   bash tools/vmtest/floor/f2_faults.sh
set -uo pipefail
cd "$(dirname "$0")/../../.."
QJS=components/quickjs-ng/quickjs-ng
BASE=.cache/vmtest-faults-f2
status=0
for fault in no-reorder no-delete-mark no-proto-set-hook no-fastpath-check no-done-mark key-prefix key-sym-string; do
  dir=$BASE/$fault
  rm -rf "$dir"; mkdir -p "$dir/qjs"
  cp $QJS/*.c $QJS/*.h "$dir/qjs/"
  python3 tools/vmtest/floor/f2_faults.py "$fault" "$dir/qjs/quickjs.c" >/dev/null || { echo "$fault: patch failed"; status=1; continue; }
  VMTEST_QJS=$dir/qjs VMTEST_OUT=$PWD/$dir/out bash tools/vmtest/build.sh asan-lb >"$dir/build.log" 2>&1 \
    || { echo "$fault: build failed"; tail -3 "$dir/build.log"; status=1; continue; }
  line=$(VMTEST_OUT=$PWD/$dir/out bash tools/vmtest/run.sh --variant asan-lb 2>&1 | grep '^corpus')
  if [[ $line == *" 0 failed"* ]]; then
    echo "BAD  $fault: not detected: $line"; status=1
  else
    echo "OK   $fault: $line"
  fi
done
exit $status
