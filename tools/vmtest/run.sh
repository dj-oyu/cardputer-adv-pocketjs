#!/usr/bin/env bash
# Runs tools/vmtest/corpus/*.js through vmrun and diffs against expected/.
#
#   tools/vmtest/run.sh                    # asan build, all files
#   tools/vmtest/run.sh --variant o2       # the -O2 build
#   tools/vmtest/run.sh --force-yield      # L1+: yield at every checkpoint; output must not change
#   tools/vmtest/run.sh --budget-jobs 3    # L1: cut every drain after 3 jobs; output must not change
#   tools/vmtest/run.sh --trace            # also write allocator traces
#   tools/vmtest/run.sh --bless            # (re)write expected/ -- only for NEW files, see README
#   tools/vmtest/run.sh closures generators  # a subset, by basename
#
# Per-file flags come from a first-line "// vmrun-flags: ..." comment and are
# appended after the default "--profile host", so they override it.
# Lines starting with "#info" (measurements) and "vmrun: note:" (driver
# notices) are excluded from the diff and collected in $OUT/info-<variant>.txt.
# The exit code of vmrun is appended as a final "exit=N" line and IS diffed.
set -uo pipefail
cd "$(dirname "$0")"
HERE=$(pwd)
OUT=${VMTEST_OUT:-$HERE/../../.cache/vmtest}
variant=asan bless=0 force_yield=0 trace=0 budget_jobs=
names=()
while [ $# -gt 0 ]; do
  case "$1" in
    --variant) variant=$2; shift ;;
    --bless) bless=1 ;;
    --force-yield) force_yield=1 ;;
    --budget-jobs) budget_jobs=$2; shift ;;
    --trace) trace=1 ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) names+=("$1") ;;
  esac
  shift
done
VMRUN=$OUT/vmrun-$variant
[ -x "$VMRUN" ] || { echo "missing $VMRUN; run tools/vmtest/build.sh $variant" >&2; exit 2; }
# LSan on: a VM level that leaks a frame or a JSValue on some path fails here.
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=0:halt_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
mkdir -p "$OUT/actual-$variant" "$OUT/traces"
info=$OUT/info-$variant.txt
: > "$info"

if [ ${#names[@]} -eq 0 ]; then
  for f in corpus/*.js; do names+=("$(basename "$f" .js)"); done
fi

pass=0 fail=0
failed=()
for name in "${names[@]}"; do
  src=corpus/$name.js
  [ -f "$src" ] || { echo "no such corpus file: $src" >&2; fail=$((fail+1)); failed+=("$name"); continue; }
  flags=(--profile host)
  first=$(head -n1 "$src")
  if [[ "$first" == "// vmrun-flags:"* ]]; then
    read -r -a extra <<< "${first#// vmrun-flags:}"
    flags+=("${extra[@]}")
  fi
  # Appended LAST so they beat a per-file "// vmrun-flags:" budget: the
  # point of these two is to re-run the WHOLE corpus at a chosen budget and
  # require the same bytes out (docs/vm-L1-design.md sec.7 invariants 1-5).
  [ $force_yield = 1 ] && flags+=(--force-yield)
  [ -n "$budget_jobs" ] && flags+=(--budget-jobs "$budget_jobs")
  [ $trace = 1 ] && flags+=(--trace "$OUT/traces/$name.trace")
  raw=$OUT/actual-$variant/$name.raw
  # cwd = corpus/ so every label and stack frame names the file by basename.
  (cd corpus && timeout 300 "$VMRUN" "${flags[@]}" "$name.js") > "$raw" 2>&1
  code=$?
  echo "exit=$code" >> "$raw"
  grep -E '^(#info|vmrun: note:)' "$raw" | sed "s/^/$name: /" >> "$info"
  grep -vE '^(#info|vmrun: note:)' "$raw" > "$OUT/actual-$variant/$name.txt"
  exp=expected/$name.txt
  if [ $bless = 1 ]; then
    cp "$OUT/actual-$variant/$name.txt" "$exp"
    echo "blessed $name"
    continue
  fi
  if [ -f "$exp" ] && diff -u "$exp" "$OUT/actual-$variant/$name.txt" > "$OUT/actual-$variant/$name.diff"; then
    pass=$((pass+1))
    echo "PASS $name"
  else
    fail=$((fail+1))
    failed+=("$name")
    echo "FAIL $name"
    [ -f "$exp" ] && head -n 40 "$OUT/actual-$variant/$name.diff" || echo "  (no expected/$name.txt)"
  fi
done
[ $bless = 1 ] && exit 0
echo "corpus [$variant$([ $force_yield = 1 ] && echo ,force-yield)${budget_jobs:+,budget-jobs=$budget_jobs}]: $pass passed, $fail failed${failed[*]:+ (${failed[*]})}"
echo "info: $info"
[ $trace = 1 ] && echo "traces: $OUT/traces/"
[ $fail -eq 0 ]
