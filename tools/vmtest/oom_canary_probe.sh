#!/usr/bin/env bash
# OOM canary gate (JS_TakeOOMCanary, quickjs.h / JSMallocState.oom_*).
#
# A rejected allocation is otherwise invisible from outside QuickJS in two
# separate ways, and this checks both are closed:
#
#   1. quickjs.c's js_malloc_rt/js_calloc_rt/js_realloc_rt reject a request
#      against malloc_limit BEFORE ever calling the embedder's allocator (see
#      js_oom_canary_record). vmrun's own allocator stats ("#info alloc ...
#      fails=N") stay at 0 for a rejection of this kind -- it never reaches
#      them -- so an instrument that only counts the allocator's own NULL
#      returns misses most device OOMs (heap_limit is deliberately set at or
#      below the real budget).
#   2. Once JS_ThrowOutOfMemory's own allocation ALSO fails, the exception
#      the guest sees is a bare `null` -- indistinguishable, from inside a
#      catch block, from a script's own `throw null`.
#
# So this checks: an injected rejection is seen (both paths, #1 twice: the
# embedder-allocator path via --fail-alloc, the pure-accounting path via a
# tight --heap-limit with no injection), an UNinjected run of the same
# scripts reads 0, a script's own `throw null` reads 0 (the negative control
# that makes the positive controls mean something), and the whole vmtest
# corpus -- which run.sh already exercises every gate -- never trips it.
#
#   tools/vmtest/oom_canary_probe.sh          # o2 build
#   tools/vmtest/oom_canary_probe.sh asan     # asan build
#
# Last line, machine-readable:
#   oom_canary variant=V checks=N failed=M verdict=OK|FAIL
# Exit 0 only when every check holds.
set -uo pipefail
cd "$(dirname "$0")"
HERE=$(pwd)
OUT=${VMTEST_OUT:-$HERE/../../.cache/vmtest}
variant=${1:-o2}
VMRUN=$OUT/vmrun-$variant
[ -x "$VMRUN" ] || { echo "missing $VMRUN; run tools/vmtest/build.sh $variant" >&2; exit 1; }

checks=0 failed=0
work=$(mktemp -d "${TMPDIR:-/tmp}/vmtest-oom.XXXXXX")
trap 'rm -rf "$work"' EXIT

# check NAME WANT-OOM VMRUN-ARGS...
#   WANT-OOM: 0 | >0
check() {
  local name=$1 want=$2; shift 2
  local raw=$work/$name.raw
  (cd "$work" && timeout 60 "$VMRUN" --stats "$@") > "$raw" 2>&1
  local n
  n=$(sed -n 's/^#info oom count=\([0-9]*\).*/\1/p' "$raw" | head -n1)
  checks=$((checks + 1))
  local ok=1
  case "$want" in 0) [ "${n:-x}" = 0 ] || ok=0 ;; '>0') [ "${n:-0}" -gt 0 ] || ok=0 ;; esac
  if [ $ok = 1 ]; then
    printf 'ok   %-28s oom=%-4s (wanted %s) [%s]\n' "$name" "${n:--}" "$want" "$*"
  else
    failed=$((failed + 1))
    printf 'FAIL %-28s oom=%-4s (wanted %s) [%s]\n' "$name" "${n:--}" "$want" "$*"
    tail -n 12 "$raw"
  fi
}

# many.js allocates one fresh JSString per iteration (a.push("x".repeat(...))
# rather than growing one array's backing store), so it reaches tens of
# thousands of DISTINCT malloc calls (measured: ~60,800 at --profile host)
# instead of the low thousands an amortized-growth array gives -- --fail-alloc
# needs an attempt number the run actually reaches.
cat > "$work/many.js" <<'JS'
var a = [];
for (var i = 0; i < 20000; i++) a.push("x".repeat(i % 50) + i);
JS

# Positive control #1: the embedder-allocator path. --fail-alloc makes
# vmrun's OWN malloc/realloc return NULL once, deep into an ordinary script.
check fail_alloc_injected '>0' --profile host --fail-alloc 5000 "$work/many.js"
# Negative control: the identical script, uninjected, must read 0 -- proves
# the positive control above is the injection, not something else about the
# script.
check fail_alloc_absent 0 --profile host "$work/many.js"

# Positive control #2, the path an allocator-level counter cannot see at all:
# a --heap-limit tight enough that the malloc_limit accounting check in
# js_malloc_rt rejects the request before rt->mf.js_malloc is ever called.
# allocator_fails (from "#info alloc ... fails=N") must stay 0 -- proof the
# allocator genuinely never saw this one -- while oom must be >0.
cat > "$work/limit.js" <<'JS'
try { var x = []; for (var i = 0; i < 200000; i++) x.push(i); } catch (e) {}
JS
# 100K, not something smaller: JS_NewRuntime2 itself needs more than the
# ~80K the runtime's own bootstrap allocations use, and a limit under that
# makes vmrun exit 4 (runtime creation failed) before the script -- and the
# canary -- ever run (measured on this build; see the comment above).
raw=$work/limit_reject.raw
(cd "$work" && timeout 60 "$VMRUN" --stats --heap-limit 100K limit.js) > "$raw" 2>&1
n=$(sed -n 's/^#info oom count=\([0-9]*\).*/\1/p' "$raw" | head -n1)
allocator_fails=$(sed -n 's/.*fails=\([0-9]*\).*/\1/p' "$raw" | head -n1)
checks=$((checks + 1))
if [ "${n:-0}" -gt 0 ] && [ "${allocator_fails:-x}" = 0 ]; then
  printf 'ok   %-28s oom=%-4s allocator_fails=%-3s (accounting rejected it before the allocator ever saw it)\n' \
    limit_rejected_by_accounting "$n" "$allocator_fails"
else
  failed=$((failed + 1))
  printf 'FAIL %-28s oom=%-4s allocator_fails=%-3s (wanted oom>0 allocator_fails=0)\n' \
    limit_rejected_by_accounting "${n:--}" "${allocator_fails:--}"
  tail -n 12 "$raw"
fi

# Negative control that makes both positive controls mean something: a
# script's OWN `throw null` -- the exact shape an OOM degrades to once
# JS_ThrowOutOfMemory's own allocation also fails -- must read 0 when nothing
# was actually injected. Content alone cannot tell these apart; the canary is
# what does.
cat > "$work/thrownull.js" <<'JS'
try { throw null; } catch (e) { if (e !== null) throw new Error("bad catch"); }
JS
check throw_null_no_injection 0 --profile host "$work/thrownull.js"

# Negative control, corpus-wide: an ordinary run of the whole vmtest corpus
# (what run.sh already exercises every gate) must never trip the canary --
# EXCEPT the handful of files whose whole subject IS the device's 160 KiB
# limit (device-profile files with a memory/oom-shaped name, at the time this
# was written: gc_threshold_device, memory_device, seg_oom_boundary). Those
# already assert their own exact behaviour against expected/, byte for byte;
# asserting oom=0 for them too would be asserting they never hit the limit
# they exist to hit. Reruns the files rather than reusing run.sh's own .raw
# output, so this adds no new expected/ file and cannot go stale against one;
# only the new #info line is read, everything else is ignored.
oom_corpus_exceptions=" gc_threshold_device memory_device seg_oom_boundary "
corpus_bad=0
for f in corpus/*.js; do
  name=$(basename "$f" .js)
  case "$oom_corpus_exceptions" in *" $name "*) continue ;; esac
  flags=(--profile host)
  first=$(head -n1 "$f")
  if [[ "$first" == "// vmrun-flags:"* ]]; then
    read -r -a extra <<< "${first#// vmrun-flags:}"
    flags+=("${extra[@]}")
  fi
  out=$(cd corpus && timeout 60 "$VMRUN" --stats "${flags[@]}" "$name.js" 2>&1 |
    sed -n 's/^#info oom count=\([0-9]*\).*/\1/p' | head -n1)
  if [ "${out:-0}" != 0 ]; then
    corpus_bad=$((corpus_bad + 1))
    echo "  corpus regression: $name oom=$out" >&2
  fi
done
checks=$((checks + 1))
if [ $corpus_bad = 0 ]; then
  echo "ok   corpus (all files)         oom=0 for every file"
else
  failed=$((failed + 1))
  echo "FAIL corpus (all files)         $corpus_bad file(s) tripped the canary unexpectedly"
fi

verdict=OK; [ $failed = 0 ] || verdict=FAIL
echo "oom_canary variant=$variant checks=$checks failed=$failed verdict=$verdict"
[ $failed = 0 ]
