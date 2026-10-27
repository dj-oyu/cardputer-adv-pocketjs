#!/bin/bash
# The --fail-alloc corpus cases aim at one allocation each, and the attempt
# number drifts whenever anything before it allocates differently (an atom
# added, a builtin made lazy). A drifted number still passes -- it fails some
# other allocation and may print the same "caught" line -- so the case can
# silently stop guarding its bug (the five atoms Iterator.zip added on
# 2026-10-27 moved all three by 5-7 attempts). This script puts each fix
# back the way it was broken, in a copy of the engine, and requires the
# number run.sh uses to make ASan report there. WSL:
#   bash tools/vmtest/oom_targets.sh
# If one fails, re-find the number: sweep --fail-alloc on the broken copy
# (.cache/vmtest-oomtargets/<fault>/out/vmrun-asan) for the first attempt
# that reports, check the fixed build still prints the expected output, and
# write it on the "// vmrun-rom-lb-li-flags:" line with the same number of
# digits (a shorter line shortens the source, and that alone moves it).
set -uo pipefail
cd "$(dirname "$0")/../.."
QJS=components/quickjs-ng/quickjs-ng
BASE=.cache/vmtest-oomtargets
status=0

# fault name | corpus case | anchor (fixed) | replacement (broken)
apply() {
  python3 - "$1" "$2" <<'PY'
import sys
name, path = sys.argv[1], sys.argv[2]
FAULTS = {
    # js_create_resolving_functions leaves a freed value in resolving_funcs[0]
    "resolving-functions": (
        "                JS_FreeValue(ctx, resolving_funcs[0]);\n                resolving_funcs[0] = JS_UNDEFINED;\n",
        "                JS_FreeValue(ctx, resolving_funcs[0]);\n"),
    # js_promise_withResolvers clears its local only after a successful define
    "with-resolvers": (
        "        *pval = JS_UNDEFINED; // consumed by JS_DefinePropertyValue\n        if (ret < 0) {\n            goto exception;\n        }\n",
        "        if (ret < 0) {\n            goto exception;\n        }\n        *pval = JS_UNDEFINED;\n"),
}
old, new = FAULTS[name]
s = open(path, encoding="utf-8").read()
assert s.count(old) == 1, f"{name}: anchor found {s.count(old)} times"
open(path, "w", encoding="utf-8").write(s.replace(old, new))
PY
}

check() {   # fault case
  local fault=$1 case=$2 dir=$BASE/$1
  local flags
  # as run.sh builds them: the first line's flags, then the
  # "// vmrun-rom-lb-li-flags:" line's, which come later and win
  flags="$(head -n1 tools/vmtest/corpus/$case.js | sed -n 's#^// vmrun-flags:##p')"
  flags="$flags $(head -n20 tools/vmtest/corpus/$case.js | sed -n 's#^// vmrun-rom-lb-li-flags:##p')"
  local out
  # captured first: with pipefail, grep -q quitting early would SIGPIPE the
  # run and turn a hit into a miss
  out=$(cd tools/vmtest && ASAN_OPTIONS=detect_leaks=0 timeout 30 ../../$dir/out/vmrun-asan --profile host $flags corpus/$case.js 2>&1)
  if grep -q AddressSanitizer <<< "$out"; then
    echo "OK   $case ($flags) fires on the $fault fault"
  else
    echo "BAD  $case ($flags): the $fault fault does not fire -- the number has drifted"
    status=1
  fi
}

for fault in resolving-functions with-resolvers; do
  dir=$BASE/$fault
  rm -rf "$dir"; mkdir -p "$dir/qjs"
  cp $QJS/*.c $QJS/*.h "$dir/qjs/"
  apply "$fault" "$dir/qjs/quickjs.c" >/dev/null || { echo "$fault: patch failed"; status=1; continue; }
  VMTEST_QJS=$dir/qjs VMTEST_OUT=$PWD/$dir/out bash tools/vmtest/build.sh asan >"$dir/build.log" 2>&1 \
    || { echo "$fault: build failed"; tail -3 "$dir/build.log"; status=1; continue; }
done
check resolving-functions oom_resolving_functions
check resolving-functions oom_resolving_functions_module
check with-resolvers oom_with_resolvers
exit $status
