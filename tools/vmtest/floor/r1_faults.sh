#!/bin/bash
# R1 negative controls (docs/vm/rom-intrinsics.md sec.9 R1, N-R1a): build
# broken copies of the engine and require vmrun --harden --immortal to fail
# on each. A detector is trusted only after it has been shown to fire. WSL:
#   bash tools/vmtest/floor/r1_faults.sh
set -uo pipefail
cd "$(dirname "$0")/../../.."
QJS=components/quickjs-ng/quickjs-ng
BASE=.cache/vmtest-faults-r1
# Cases that pass under --harden --immortal on the real engine (the r1 gate),
# so a failure here is the fault and nothing else.
CASES="rom_immortal promise_chain generators bench_sort"
status=0
for fault in free-upstream gc-decref no-realm-fallback; do
  dir=$BASE/$fault
  rm -rf "$dir"; mkdir -p "$dir/qjs"
  cp $QJS/*.c $QJS/*.h "$dir/qjs/"
  python3 tools/vmtest/floor/r1_faults.py "$fault" "$dir/qjs/quickjs.c" >/dev/null || { echo "$fault: patch failed"; status=1; continue; }
  VMTEST_QJS=$dir/qjs VMTEST_OUT=$PWD/$dir/out bash tools/vmtest/build.sh asan >"$dir/build.log" 2>&1 \
    || { echo "$fault: build failed"; tail -3 "$dir/build.log"; status=1; continue; }
  line=$(VMTEST_OUT=$PWD/$dir/out VMTEST_VMRUN_FLAGS="--harden --immortal" \
         bash tools/vmtest/run.sh --variant asan $CASES 2>&1 | grep '^corpus')
  if [[ $line == *" 0 failed"* ]]; then
    echo "BAD  $fault: not detected: $line"; status=1
  else
    echo "OK   $fault: $line"
  fi
done
exit $status
