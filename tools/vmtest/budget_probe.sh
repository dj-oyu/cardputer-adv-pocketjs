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

# Which guard the shipped configuration lets answer depends on the build:
# "#info vmstack flat=1" (CONFIG_POCKET_VM_FLATCALLS, L2b) means a JS-to-JS
# call consumes no C stack, so under run.sh's limits only the budget can end
# a deep recursion (budget_hits>0 is then the proof that the budget took the
# C-stack guard's place); flat=0 means C recursion remains and the C-stack
# guard fires first (budget_hits=0). Read from the binary, not from the
# variant name, so a plain "o2" is judged by what it was built as.
: > "$work/empty.js"
flat=$(cd "$work" && "$VMRUN" --stats --profile host empty.js 2>&1 | sed -n 's/.*vmstack flat=\([01]\).*/\1/p')
if [ "$flat" = 1 ]; then
  shipped_hits='>0'
  echo "# both guards at the shipped value (what run.sh runs): flat calls (L2b), so the"
  echo "# C-stack guard cannot see JS depth and the budget must be the one that answers"
else
  shipped_hits=0
  echo "# both guards at the shipped value (what run.sh runs): while C recursion remains,"
  echo "# the C-stack guard answers first and the budget is not consulted to the end"
fi
check deep_recursion         match "$shipped_hits" 0   --profile host
check deep_recursion_device  match "$shipped_hits" 0   --profile device
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

# D38 (docs/vm-L2-design.md sec.12.2): an async function's SYNCHRONOUS
# recursion (`async function dive() { depth++; await dive(); }`, every level
# the first stretch of the next) is the one deep recursion the budget does
# not answer. On the flat builds each level is a flat async frame -- no C
# frame, no segment block -- so the budget never sees it (budget_hits must
# be 0) and the descent ends when the 160 KiB guest heap does. Measured
# (2026-09-13, host), that end is messier than "an InternalError reaches the
# catch": the error object cannot be built either (the reason is null), and
# some levels' own await cannot register its reaction on the way out, so
# their promises are left unhandled (exit 2). The probe file's header has
# the full account. The diffed lines fix what is stable: the synchronous
# try sees nothing, which class the outer async catch sees, and the exit
# code. The depth and the unhandled count are info (they move with every
# byte the allocator's layout moves). On -recur each level is the upstream
# C chain and async_func_resume's C-stack test answers first; that variant
# is held to what was MEASURED when this check was written
# (expected/deep_async_recursion-recur.txt: RangeError, exit 0), never to an
# assumption.
#
# The probe is ../deep_async_recursion.js, not a corpus file: the right
# answer differs by build, and under --profile device it is a creeping OOM,
# the shape in which upstream's build_backtrace use-after-free shows
# (known/oom_backtrace_uaf.js, README.md). So on an asan variant an ASan
# report from build_backtrace is RECORDED as that known bug, not counted as
# a failure and not counted as a check: the o2 variants are the ones this
# check binds.
check_async() {
  # Assigned one per line: `local a=x b=$a` expands $a before a is set.
  local name=deep_async_recursion
  local raw=$work/$name.raw
  local txt=$work/$name.txt
  local exp
  if [ "$flat" = 1 ]; then exp=expected/$name.txt; else exp=expected/$name-recur.txt; fi
  (eval "$stack_cmd"; timeout 300 "$VMRUN" --stats --profile device "$name.js") > "$raw" 2>&1
  echo "exit=$?" >> "$raw"
  # The unhandled-rejection report lines are counted, not diffed.
  grep -vE '^(#info|vmrun: note:|E pocketjs_guest: Unhandled Promise rejection:)' "$raw" > "$txt"
  local hits depth unhandled oomn match
  hits=$(sed -n 's/.*budget_hits=\([0-9]*\).*/\1/p' "$raw")
  depth=$(sed -n 's/^#info max_depth=\([0-9]*\).*/\1/p' "$raw" | head -n1)
  unhandled=$(grep -c '^E pocketjs_guest: Unhandled Promise rejection:' "$raw")
  # OOM canary (quickjs.h JS_TakeOOMCanary): on flat the descent ends in heap
  # exhaustion, and the canary is the positive evidence of that. The output
  # alone cannot carry it: whether the outer catch even manages to print
  # ("caught null" when this check was written, nothing once D42/D43 moved
  # the frame segments' bytes -- docs/vm-L2-design.md sec.13.6) depends on how
  # many bytes the exhausted heap happens to leave, so expected/ binds only
  # what does not move -- the sync try is never reached, exit 2 -- and the
  # canary binds the cause.
  oomn=$(sed -n 's/^#info oom count=\([0-9]*\).*/\1/p' "$raw" | head -n1)
  if grep -q 'AddressSanitizer' "$raw" && grep -q 'build_backtrace' "$raw"; then
    printf 'note %-24s ASan report in build_backtrace: known/oom_backtrace_uaf reproduced (D38), not counted %s[--profile device]\n' \
      "$name" "${depth:+depth=$depth }"
    return
  fi
  if diff -q "$exp" "$txt" > /dev/null; then match=match; else match=differ; fi
  checks=$((checks + 1))
  # On flat (deep_async_recursion.txt) the canary must have fired at least
  # once, in the SAME run whose output matched. On -recur
  # (deep_async_recursion-recur.txt, RangeError -- the C-stack guard answers
  # first, sec.12.2) whether the heap was also under pressure is not part of
  # what this check binds; the count is recorded, not gated.
  local want_oom=0
  [ "$flat" = 1 ] && want_oom='>0'
  local oom_ok=1
  case "$want_oom" in 0) : ;; '>0') [ "${oomn:-0}" -gt 0 ] || oom_ok=0 ;; esac
  if [ "$match" = match ] && [ "${hits:-x}" = 0 ] && [ $oom_ok = 1 ]; then
    printf 'ok   %-24s %-7s budget_hits=%-3s unhandled=%-3s oom=%-3s %s[--profile device, vs %s]\n' \
      "$name" "$match" "${hits:--}" "$unhandled" "${oomn:--}" "${depth:+depth=$depth }" "$exp"
  else
    failed=$((failed + 1))
    printf 'FAIL %-24s %-7s budget_hits=%-3s unhandled=%-3s oom=%-3s %s[--profile device, vs %s]  wanted match hits=0 oom=%s\n' \
      "$name" "$match" "${hits:--}" "$unhandled" "${oomn:--}" "${depth:+depth=$depth }" "$exp" "$want_oom"
    [ "$match" = match ] || diff "$exp" "$txt" | head -n 8
  fi
}
echo "# D38: an async function's synchronous recursion is ended by the heap, not the budget"
check_async

verdict=OK; [ $failed = 0 ] || verdict=FAIL
echo "D10 variant=$variant flat=${flat:-?} checks=$checks failed=$failed verdict=$verdict"
[ $failed = 0 ]
