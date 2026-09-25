#!/usr/bin/env bash
# Runs tools/vmtest/corpus/*.js through vmrun and diffs against expected/.
#
#   tools/vmtest/run.sh                    # asan build, all files
#   tools/vmtest/run.sh --variant o2       # the -O2 build
#   tools/vmtest/run.sh --force-yield      # L1+: yield at every checkpoint; output must not change
#   tools/vmtest/run.sh --budget-jobs 3    # L1: cut every drain after 3 jobs; output must not change
#   tools/vmtest/run.sh --fair             # L1: fair ordering (CONFIG_POCKET_VM_FAIR)
#   tools/vmtest/run.sh --trace            # also write allocator traces
#   tools/vmtest/run.sh --bless            # (re)write expected/ -- only for NEW files, see README
#   tools/vmtest/run.sh closures generators  # a subset, by basename
#   VMTEST_VMRUN_FLAGS="--vm-seg-size 88" tools/vmtest/run.sh   # L2a: sweep the segment size
#
# --fair diffs against expected-fair/<name>.txt when that file exists and
# against expected/<name>.txt when it does not: the whole corpus must come out
# byte-identical in both modes EXCEPT the handful of files written to show the
# difference, which have one expected file per mode.
# A "-keepsrc" variant (function source text kept) does the same with
# expected-keepsrc/<name>.txt.
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
variant=asan bless=0 force_yield=0 trace=0 budget_jobs= fair=0 fy_fault=
names=()
while [ $# -gt 0 ]; do
  case "$1" in
    --variant) variant=$2; shift ;;
    --bless) bless=1 ;;
    --force-yield) force_yield=1 ;;
    --force-yield-fault) force_yield=1; fy_fault=$2; shift ;;
    --fair) fair=1 ;;
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

pass=0 fail=0 skipped=0
failed=() skipped_names=()
for name in "${names[@]}"; do
  src=corpus/$name.js
  [ -f "$src" ] || { echo "no such corpus file: $src" >&2; fail=$((fail+1)); failed+=("$name"); continue; }
  # "// vmrun-skip-variants: V... -- reason" in the first 5 lines: the file is
  # not run on those builds, and says so on every run instead of silently
  # passing. For cases whose subject is an exact allocation sequence that a
  # build variant structurally changes -- e.g. a --fail-alloc attempt number
  # on the *-alloca builds, which allocate no frame segment and so number
  # every later allocation one lower. Not for hiding a real difference: the
  # reason is printed, and the skip is counted in the summary.
  skip_line=$(head -n5 "$src" | grep -m1 '^// vmrun-skip-variants:' || true)
  if [ -n "$skip_line" ]; then
    skip_spec=${skip_line#// vmrun-skip-variants:}
    skip_list=${skip_spec%%--*}
    skip_reason=${skip_spec#*--}
    [ "$skip_reason" = "$skip_spec" ] && skip_reason=" (no reason given)"
    if [[ " $skip_list " == *" $variant "* ]]; then
      skipped=$((skipped+1)); skipped_names+=("$name")
      echo "SKIP $name ($variant):$skip_reason"
      continue
    fi
  fi
  flags=(--profile host)
  extra=()
  first=$(head -n1 "$src")
  if [[ "$first" == "// vmrun-flags:"* ]]; then
    read -r -a extra <<< "${first#// vmrun-flags:}"
    flags+=("${extra[@]}")
  fi
  # A "-keepsrc" variant parses one js_strndup per function more than the
  # shipping build, so a --fail-alloc attempt number shifts by the functions
  # defined before its target. "// vmrun-keepsrc-flags: ..." in the first 5
  # lines is appended after the above (last one wins) for those variants.
  if [[ $variant == *-keepsrc ]]; then
    keep_line=$(head -n5 "$src" | grep -m1 '^// vmrun-keepsrc-flags:' || true)
    if [ -n "$keep_line" ]; then
      read -r -a extra <<< "${keep_line#// vmrun-keepsrc-flags:}"
      flags+=("${extra[@]}")
    fi
  fi
  # Same for a "-rom" variant (F1, builtin names in flash): ~440 fewer
  # allocations before the program starts, plus one per builtin name the
  # program turns into a string value. "// vmrun-rom-flags: ..." carries the
  # re-pinned number, found by aligning the two allocator traces after
  # "# ready" (the same-size allocation, not just the same output).
  if [[ $variant == *-rom* ]]; then
    rom_line=$(head -n5 "$src" | grep -m1 '^// vmrun-rom-flags:' || true)
    if [ -n "$rom_line" ]; then
      read -r -a extra <<< "${rom_line#// vmrun-rom-flags:}"
      flags+=("${extra[@]}")
    fi
  fi
  # Appended LAST so they beat a per-file "// vmrun-flags:" budget: the
  # point of these two is to re-run the WHOLE corpus at a chosen budget and
  # require the same bytes out (docs/vm/vm-L1-design.md sec.7 invariants 1-5).
  if [ -n "$fy_fault" ]; then flags+=(--force-yield-fault "$fy_fault")
  elif [ $force_yield = 1 ]; then flags+=(--force-yield)
  fi
  [ -n "$budget_jobs" ] && flags+=(--budget-jobs "$budget_jobs")
  [ $fair = 1 ] && flags+=(--fair)
  # ...unless the file says its budget is part of the case AND we are in fair
  # ordering. Under fair ordering a file whose subject IS where host events
  # meet the queue has an output that is a function of where the budget falls;
  # a sweep that moved the budget would be asking it a different question. The
  # same files are budget-independent under compat ordering -- there the host
  # event always arrives after the drain, wherever the drain was cut -- so the
  # compat sweep still sweeps them. Such a file says "// vmrun-pin-budget" on
  # its second line and gets its own flags back, last.
  if [ $fair = 1 ] && [ "$(sed -n 2p "$src")" = "// vmrun-pin-budget" ] &&
     [ ${#extra[@]} -gt 0 ]; then
    flags+=("${extra[@]}")
  fi
  # VMTEST_VMRUN_FLAGS: extra vmrun flags for a sweep that must not change
  # the bytes out, appended after everything else so they win. Added for
  # L2a: `VMTEST_VMRUN_FLAGS="--vm-seg-size 88" run.sh` runs the corpus with
  # frame segments of 88 bytes, which puts a segment boundary under nearly
  # every call (docs/vm/vm-L2-design.md sec.1.1 #6); the expected files are
  # the same, because the segment size is not allowed to be observable.
  [ -n "${VMTEST_VMRUN_FLAGS:-}" ] && read -r -a envflags <<< "$VMTEST_VMRUN_FLAGS" && flags+=("${envflags[@]}")
  [ $trace = 1 ] && flags+=(--trace "$OUT/traces/$name.trace")
  raw=$OUT/actual-$variant/$name.raw
  # cwd = corpus/ so every label and stack frame names the file by basename.
  #
  # TWO GUARDS AGAINST AN ENVIRONMENT FAILURE BEING RECORDED AS A VM FAILURE.
  # This container's ASan runtime fails to START on roughly 1 run in 4: gcc
  # 12.2's libasan8 on the WSL2 6.18 kernel leaves the process spinning on a
  # signal it cannot report, printing "AddressSanitizer:DEADLYSIGNAL" forever
  # (~13M lines/s -- one 300 s timeout left a 3.9 GB .raw). It is not the VM:
  # a two-line `int main(){puts("hi");}` built with -fsanitize=address hangs the
  # same way, 16 times in 60, while the same program without ASan is 60 for 60.
  # So (a) anything that looks like that startup hang is retried, and (b) every
  # .raw is capped -- real ASan reports and real output are orders of magnitude
  # under the cap. VMTEST_HANG_RETRIES=0 turns the retry off.
  attempts=0
  while :; do
    (cd corpus && export VMTEST_START_MARKER=1 && exec timeout 300 "$VMRUN" "${flags[@]}" "$name.js") 2>&1 \
      | head -c "${VMTEST_MAX_RAW:-4194304}" > "$raw"
    code=${PIPESTATUS[0]}
    attempts=$((attempts + 1))
    # A VM timeout/crash after main is a failure, not an ASan startup retry.
    if [[ "$variant" == asan* ]] && ! grep -q '^#info vmrun-start$' "$raw" && \
       { [ $code -eq 124 ] || [ $code -eq 141 ] || grep -q '^AddressSanitizer:DEADLYSIGNAL' "$raw"; }; then
      if [ "$attempts" -lt "${VMTEST_HANG_RETRIES:-5}" ]; then
        echo "  retry $name (asan startup hang, attempt $attempts)" >&2
        continue
      fi
      echo "  note: $name hit the asan startup hang on all $attempts attempts" >&2
    fi
    break
  done
  echo "exit=$code" >> "$raw"
  grep -E '^(#info|vmrun: note:)' "$raw" | sed "s/^/$name: /" >> "$info"
  grep -vE '^(#info|vmrun: note:)' "$raw" > "$OUT/actual-$variant/$name.txt"
  # L2c gate (design sec.12.9, D22r): under --force-yield a file must pass
  # TWO checks. The rule -- every safepoint this run could yield at got a
  # stop, and every stop a resume -- read off this run's own #info lines, the
  # same for every file. AND the existing byte-identity against expected/
  # below: stopping and resuming must not change what the program prints
  # (docs/vm/vm-L1-design.md sec.7 invariants; first-version 12.7 "--force-yield
  # でバイト一致"). Until the VM can resume both fail, and each reason is shown.
  rule_ok=1 rule_reason=
  if [ $force_yield = 1 ] && [ $bless = 0 ]; then
    sp=$(grep -o 'safepoints_yieldable=[0-9]*' "$raw" | tail -n1 | cut -d= -f2); sp=${sp:-0}
    st=$(grep -o ' stops=[0-9]*' "$raw" | tail -n1 | cut -d= -f2); st=${st:-0}
    rs=$(grep -o ' resumes=[0-9]*' "$raw" | tail -n1 | cut -d= -f2); rs=${rs:-0}
    rule_reason="safepoints_yieldable=$sp stops=$st resumes=$rs"
    if [ "$sp" -gt 0 ] && { [ "$st" -eq 0 ] || [ "$rs" -ne "$st" ]; }; then
      rule_ok=0
    fi
  fi
  exp=expected/$name.txt
  # One expected file per mode, but only where the modes genuinely differ.
  [ $fair = 1 ] && [ -f "expected-fair/$name.txt" ] && exp=expected-fair/$name.txt
  # Same rule for CONFIG_POCKET_VM_STRIP_FN_SOURCE: expected/ is the shipping
  # default (no source text); a "-keepsrc" variant reads expected-keepsrc/
  # for the files whose output is a function's source.
  [[ $variant == *-keepsrc ]] && [ -f "expected-keepsrc/$name.txt" ] && exp=expected-keepsrc/$name.txt
  if [ $bless = 1 ]; then
    cp "$OUT/actual-$variant/$name.txt" "$exp"
    echo "blessed $name"
    continue
  fi
  bytes_ok=0
  [ -f "$exp" ] && diff -u "$exp" "$OUT/actual-$variant/$name.txt" > "$OUT/actual-$variant/$name.diff" && bytes_ok=1
  if [ $bytes_ok = 1 ] && [ $rule_ok = 1 ]; then
    pass=$((pass+1))
    echo "PASS $name${rule_reason:+ ($rule_reason)}"
  else
    fail=$((fail+1))
    failed+=("$name")
    echo "FAIL $name${rule_reason:+ ($rule_reason$([ $rule_ok = 0 ] && echo ': rule')$([ $bytes_ok = 0 ] && echo ': bytes'))}"
    if [ $bytes_ok = 0 ]; then
      [ -f "$exp" ] && head -n 40 "$OUT/actual-$variant/$name.diff" || echo "  (no expected/$name.txt)"
    fi
  fi
done
[ $bless = 1 ] && exit 0
echo "corpus [$variant$([ $force_yield = 1 ] && echo ,force-yield)$([ $fair = 1 ] && echo ,fair)${budget_jobs:+,budget-jobs=$budget_jobs}]: $pass passed, $fail failed${failed[*]:+ (${failed[*]})}${skipped_names[*]:+, $skipped skipped (${skipped_names[*]})}"
echo "info: $info"
[ $trace = 1 ] && echo "traces: $OUT/traces/"
[ $fail -eq 0 ]
