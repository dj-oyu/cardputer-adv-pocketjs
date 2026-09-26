#!/bin/bash
# Negative controls for the cap back-off (quickjs.c js_trigger_gc,
# docs/vm/gc-cap-backoff.md): build broken copies of the engine and require
# the corpus (gc_threshold_near_limit) to notice each. WSL:
#   bash tools/vmtest/gc_faults.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
QJS=components/quickjs-ng/quickjs-ng
BASE=.cache/vmtest-faults-gc
status=0
apply() {   # fault file -> patched copy
  case $1 in
    no-stale-floor) python3 - "$2" <<'PY'
import sys; p=sys.argv[1]; s=open(p).read()
a="        if (rt->malloc_state.malloc_size >= floor &&\n            rt->malloc_state.malloc_size < floor + budget)"
assert s.count(a)==1; open(p,'w').write(s.replace(a,"        if (rt->malloc_state.malloc_size < floor + budget)"))
PY
    ;;
    no-headroom-cap) python3 - "$2" <<'PY'
import sys; p=sys.argv[1]; s=open(p).read()
a="        if (room < budget)\n            budget = room;"
assert s.count(a)==1; open(p,'w').write(s.replace(a,"        (void)room;"))
PY
    ;;
  esac
}
for fault in no-stale-floor no-headroom-cap; do
  dir=$BASE/$fault
  rm -rf "$dir"; mkdir -p "$dir/qjs"
  cp $QJS/*.c $QJS/*.h "$dir/qjs/"
  apply $fault "$dir/qjs/quickjs.c" || { echo "$fault: patch failed"; status=1; continue; }
  VMTEST_QJS=$dir/qjs VMTEST_OUT=$PWD/$dir/out bash tools/vmtest/build.sh o2 >"$dir/build.log" 2>&1 \
    || { echo "$fault: build failed"; status=1; continue; }
  line=$(VMTEST_OUT=$PWD/$dir/out bash tools/vmtest/run.sh --variant o2 2>&1 | grep '^corpus')
  if [[ $line == *" 0 failed"* ]]; then echo "BAD  $fault: not detected: $line"; status=1
  else echo "OK   $fault: $line"; fi
done
exit $status
