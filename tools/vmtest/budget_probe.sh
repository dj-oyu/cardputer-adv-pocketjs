#!/usr/bin/env bash
# D10 (docs/vm-L2-design.md sec.9): is the frame-segment byte budget the
# thing that stops a deep recursion, and does it stop it BEFORE the heap does?
#
# run.sh cannot answer that. Under run.sh every file runs with both guards at
# the same value (JS_SetMaxStackSize sets the C-stack limit and the budget
# together), and while JS_CallInternal still recurses in C the C-stack guard
# fires first -- deep_recursion.txt matches with budget_hits=0. A budget that
# did nothing at all would pass run.sh today, and the first L2b commit that
# removes the C recursion would then turn every RangeError in the corpus into
# an InternalError. So this script raises the C-stack limit out of the way
# (--stack-limit 512M) and sets the budget alone (--vm-budget), which is the
# situation L2b creates, and checks each of the three fixed expectations:
#
#   expected/deep_recursion.txt         host,   RangeError at depth > 1000
#   expected/deep_recursion_device.txt  device, RangeError, also through map()
#   expected/seg_oom_boundary.txt       device, InternalError: the heap, not the budget
#
# with the shipped values (host 7 MiB, device 20 KiB), then shows the same
# three going wrong when the budget is off, too small, or above the heap.
# "Which guard answered" is read from "#info vmstack ... budget_hits=N" (the
# budget refused N pushes) and "... seg_refused=N" (the runtime refused N
# pushes for want of a segment: the heap answered); a run in which the budget
# did its job has seg_refused=0. seg_oom_boundary's InternalError is not a
# segment refusal -- it is an ordinary allocation, mid-chain -- so for it the
# expected-file match is the evidence and seg_refused is not consulted.
#
#   tools/vmtest/budget_probe.sh           # o2 build
#   tools/vmtest/budget_probe.sh asan      # asan build (no ulimit change, see below)
#
# Last line, machine-readable:
#   D10 variant=V checks=N failed=M verdict=OK|FAIL
# Exit 0 only when every check holds. Segment-frame variants only: the
# -alloca builds have no budget (vmrun prints a note and the flag is ignored).
set -uo pipefail
cd "$(dirname "$0")"
HERE=$(pwd)
OUT=${VMTEST_OUT:-$HERE/../../.cache/vmtest}
variant=${1:-o2}
VMRUN=$OUT/vmrun-$variant
[ -x "$VMRUN" ] || { echo "missing $VMRUN; run tools/vmtest/build.sh $variant" >&2; exit 1; }
case "$variant" in *-alloca) echo "the $variant build keeps frames on the C stack: no budget to probe" >&2; exit 1 ;; esac
# The host 7 MiB budget alone means ~54,000 JS levels (136 B host frame each),
# and every one of them is still a C frame (528 B) until L2b: ~28 MiB of C
# stack against the default 8 MiB rlimit. Lift it for o2, exactly as
# stack_probe.sh does; not for asan (its shadow-memory layout collides with an
# unlimited stack), where the host case runs at 512 KiB instead: still > 1000
# levels, and ~3,800 levels * ~900 B of ASan C frame stays inside 8 MiB
# (1 MiB was tried and is a real ASan stack-overflow at ~7,700 levels).
host_budget=7M
case "$variant" in
  asan*) stack_cmd=:; host_budget=512K ;;
  *) stack_cmd="ulimit -s unlimited" ;;
esac

checks=0 failed=0
work=$(mktemp -d "${TMPDIR:-/tmp}/vmtest-budget.XXXXXX")
trap 'rm -rf "$work"' EXIT

# check NAME EXPECT-MATCH EXPECT-BUDGET-HITS EXPECT-SEG-REFUSED FLAGS...
#   EXPECT-MATCH:        match | differ   (diff against expected/NAME.txt)
#   EXPECT-BUDGET-HITS:  0 | >0
#   EXPECT-SEG-REFUSED:  0 | >0 | -   (- = not consulted)
check() {
  local name=$1 want_match=$2 want_hits=$3 want_fails=$4; shift 4
  local raw=$work/$name.raw txt=$work/$name.txt
  (eval "$stack_cmd"; cd corpus && timeout 300 "$VMRUN" --stats "$@" "$name.js") > "$raw" 2>&1
  echo "exit=$?" >> "$raw"
  grep -vE '^(#info|vmrun: note:)' "$raw" > "$txt"
  local hits fails match
  hits=$(sed -n 's/.*budget_hits=\([0-9]*\).*/\1/p' "$raw")
  fails=$(sed -n 's/.*seg_refused=\([0-9]*\).*/\1/p' "$raw")
  if diff -q "expected/$name.txt" "$txt" > /dev/null; then match=match; else match=differ; fi
  local ok=1
  [ "$match" = "$want_match" ] || ok=0
  case "$want_hits" in 0) [ "${hits:-x}" = 0 ] || ok=0 ;; '>0') [ "${hits:-0}" -gt 0 ] || ok=0 ;; esac
  case "$want_fails" in 0) [ "${fails:-x}" = 0 ] || ok=0 ;; '>0') [ "${fails:-0}" -gt 0 ] || ok=0 ;; esac
  checks=$((checks + 1))
  local depth
  depth=$(sed -n 's/^#info max_depth=\([0-9]*\).*/\1/p' "$raw" | head -n1)
  if [ $ok = 1 ]; then
    printf 'ok   %-24s %-7s budget_hits=%-3s seg_refused=%-3s %s[%s]\n' "$name" "$match" "${hits:--}" "${fails:--}" "${depth:+depth=$depth }" "$*"
  else
    failed=$((failed + 1))
    printf 'FAIL %-24s %-7s budget_hits=%-3s seg_refused=%-3s %s[%s]  wanted %s hits=%s refused=%s\n' \
      "$name" "$match" "${hits:--}" "${fails:--}" "${depth:+depth=$depth }" "$*" "$want_match" "$want_hits" "$want_fails"
    [ "$match" = "$want_match" ] || diff "expected/$name.txt" "$txt" | head -n 8
  fi
}

echo "# both guards at the shipped value (what run.sh runs): while C recursion remains,"
echo "# the C-stack guard answers first and the budget is not consulted to the end"
check deep_recursion         match 0 0   --profile host
check deep_recursion_device  match 0 0   --profile device
check seg_oom_boundary       match 0 -   --profile device

echo "# budget alone at the shipped value (C-stack guard lifted: the L2b situation)"
check deep_recursion         match '>0' 0   --profile host   --stack-limit 512M --vm-budget $host_budget
check deep_recursion_device  match '>0' 0   --profile device --stack-limit 512M --vm-budget 20K
check seg_oom_boundary       match 0   -   --profile device --stack-limit 512M --vm-budget 20K

echo "# negative controls: the budget off, too small, or above the heap"
check deep_recursion_device  differ 0   '>0' --profile device --stack-limit 512M --vm-budget 0
check deep_recursion         differ '>0' 0   --profile host   --stack-limit 512M --vm-budget 100K
check seg_oom_boundary       differ '>0' 0   --profile device --stack-limit 512M --vm-budget 480
check deep_recursion_device  differ 0   '>0' --profile device --stack-limit 512M --vm-budget 200K

verdict=OK; [ $failed = 0 ] || verdict=FAIL
echo "D10 variant=$variant checks=$checks failed=$failed verdict=$verdict"
[ $failed = 0 ]
