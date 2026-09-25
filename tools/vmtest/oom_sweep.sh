#!/usr/bin/env bash
# Fail every allocation attempt, one run per attempt, and require that
# nothing ASan or UBSan can see goes wrong on the way out (WSL only).
#
#   tools/vmtest/oom_sweep.sh                      # closures + generators, 4800 points each
#   tools/vmtest/oom_sweep.sh -n 3000 a.js b.js    # other files, other depth
#   VARIANT=asan-keepsrc tools/vmtest/oom_sweep.sh
#
# Why every attempt rather than a sample: the bugs this caught live at single
# points. A 15-point sweep over closures.js found one (--fail-alloc 1500); the
# dense one found 71 on the same file, in five different places, and 133 on
# generators.js. Most are in the parser, so the useful depth is roughly the
# number of allocations the file makes before its first frame runs --
# `vmrun --trace` counts them (closures.js: ~4,690 in total).
#
# What it checks is memory safety on the out-of-memory path, not the answer:
# a run that fails with InternalError "out of memory" is the expected result.
# A timeout counts as a hit, because a hang on OOM is its own bug.
#
# Exit status: 0 when no point produced a report, 1 otherwise. The report
# lines are "file<TAB>N<TAB>kind<TAB>first quickjs.c frame"; re-run one with
#   ASAN_OPTIONS=detect_leaks=0 .cache/vmtest/vmrun-asan --fail-alloc N FILE
set -uo pipefail
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
OUT=${VMTEST_OUT:-$ROOT/.cache/vmtest}
VARIANT=${VARIANT:-asan}
N=4800
JOBS=${JOBS:-$(nproc)}
if [[ ${1:-} == -n ]]; then N=$2; shift 2; fi
FILES=("$@")
if [[ ${#FILES[@]} -eq 0 ]]; then
  FILES=(tools/vmtest/corpus/closures.js tools/vmtest/corpus/generators.js)
fi

bash tools/vmtest/build.sh "$VARIANT" >/dev/null || exit 2
V=$OUT/vmrun-$VARIANT

one() {
  local v=$1 f=$2 n=$3 out rc kind site
  out=$(ASAN_OPTIONS=detect_leaks=0 timeout 30 "$v" --fail-alloc "$n" "$f" 2>&1 >/dev/null)
  rc=$?
  kind=$(printf '%s\n' "$out" | grep -oE 'AddressSanitizer: [a-z-]+|runtime error' | head -1)
  if [[ -z $kind ]]; then
    # 124 is timeout(1); >128 is a signal (an assert's abort is 134).
    [[ $rc -eq 124 || $rc -gt 128 ]] && kind="rc=$rc" || return 0
  fi
  site=$(printf '%s\n' "$out" | grep -oE 'in [A-Za-z_0-9]+ [^ ]*quickjs\.c:[0-9]+' | head -1)
  printf '%s\t%s\t%s\t%s\n' "$(basename "$f")" "$n" "$kind" "$site"
}
export -f one

hits=$(mktemp)
for f in "${FILES[@]}"; do
  seq 1 "$N" | xargs -P "$JOBS" -I{} bash -c 'one "$@"' _ "$V" "$f" {} >> "$hits"
  printf '%-28s %5d points, %d reports\n' "$(basename "$f")" "$N" \
    "$(grep -c "^$(basename "$f")	" "$hits" || true)"
done
total=$(wc -l < "$hits")
if [[ $total -gt 0 ]]; then
  echo "--- by site ---"
  cut -f3,4 "$hits" | sort | uniq -c | sort -rn
  echo "--- first reports ---"
  head -20 "$hits"
fi
rm -f "$hits"
echo "oom_sweep [$VARIANT]: $total reports"
[[ $total -eq 0 ]]
