#!/usr/bin/env bash
# verify_all.sh — the G6 gate (docs/vm-L2-design.md sec.1.3): replays every
# trace through the segment allocator under ASan+UBSan with --verify, then
# runs the fault modes as negative controls and REQUIRES each to be caught.
# A gate that only shows the good allocator passing has not shown that it
# can fail; the second half is what makes the first half mean something.
#
#   bash tools/vmalloc/verify_all.sh              # all traces, default segment size
#   bash tools/vmalloc/verify_all.sh --seg-size 8192
#   bash tools/vmalloc/verify_all.sh --quick      # skip bench_* (multi-million-line traces)
#
# Exit 0 only if every trace verifies clean AND every fault is detected.
# Results: .cache/vmalloc/verify.txt (one line per run, replay.c's format).
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
ROOT=../..
BIN=${VMALLOC_BIN:-$ROOT/.cache/vmalloc/vmalloc_replay-asan}
TRACES=$ROOT/.cache/vmtest/traces
OUT=$ROOT/.cache/vmalloc/verify.txt
CONTROL=$TRACES/closures.trace
# 96 MiB: capacity is not what this gate measures. The traces were captured
# under vmrun's 64 MiB host limit, and bench_promise really does peak at
# 16.67 MB live (measured: it failed a 16 MiB pool at op 4,249,328), while
# bench_sort grows arrays by realloc so old and new dedicated segments
# coexist. A capacity FAIL here would hide the verify result behind it.
POOL=${VMALLOC_POOL:-100663296}
quick=0
extra=()
while [ $# -gt 0 ]; do
  case "$1" in
    --quick) quick=1 ;;
    --seg-size|--seg-cache) extra+=("$1" "$2"); shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
[ -x "$BIN" ] || { echo "missing $BIN; run tools/vmalloc/build.sh" >&2; exit 2; }
[ -f "$CONTROL" ] || { echo "missing traces in $TRACES; run tools/vmtest/run.sh --trace" >&2; exit 2; }
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=0:halt_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
: > "$OUT"

fail=0
# ---- positive runs: every trace must verify clean and pass check().
for t in "$TRACES"/*.trace; do
  name=$(basename "$t")
  opts=(--sample-every 1)
  if [[ "$name" == bench_* ]]; then
    [ $quick = 1 ] && continue
    # bench_* traces run to millions of ops; a full sweep at every segment
    # event is O(live) and bench_promise has ~1,000 events over 80,000 live
    # blocks. Rate-limit event-driven sweeps to about 2,000 per trace (the
    # per-op pattern checks at free/realloc still run for every op) and
    # sample stats ~2,000 times instead of every op. A 2,500-line bench
    # trace gets gap 1, i.e. every event.
    lines=$(wc -l < "$t")
    opts=(--verify-gap $(( lines / 2000 + 1 )) --sample-every $(( lines / 2000 + 1 )))
  fi
  line=$("$BIN" --allocator segment --pool "$POOL" --verify "${opts[@]}" "${extra[@]}" "$t" 2>>"$OUT.stderr")
  rc=$?
  echo "$line" >> "$OUT"
  if [ $rc -ne 0 ] || [[ "$line" != *"verify=OK"* ]] || [[ "$line" != *"check=1"* ]]; then
    echo "FAIL  $name (rc=$rc): $line"; fail=1
  else
    echo "ok    $name: $(echo "$line" | grep -o 'seg_added=[0-9]* seg_returned=[0-9]* seg_reused=[0-9]* seg_dedicated_added=[0-9]* verify=OK verify_sweeps=[0-9]*')"
  fi
done

# ---- negative controls: each injected fault must be reported, by --verify
# (exit 3), by check() (exit 2), or by the sanitizer (abort, rc>=128 or 1).
# Swept after EVERY op here (--verify-every 1) so the report names the op
# that broke the reference instead of a later crash inside the allocator.
for fault in early-return overlap misalign pool-overlap compact; do
  line=$("$BIN" --allocator segment --pool "$POOL" --sample-every 0 --verify --verify-every 1 --fault "$fault" "${extra[@]}" "$CONTROL" 2>"$OUT.fault-$fault.stderr")
  rc=$?
  echo "fault=$fault rc=$rc $line" >> "$OUT"
  if [ $rc -eq 0 ]; then
    echo "MISSED fault=$fault: the gate did not detect it"; fail=1
  else
    how="rc=$rc"
    [ $rc -eq 3 ] && how="verify: $(echo "$line" | grep -o 'first_error=.*')"
    [ $rc -eq 2 ] && how="check(): $(tail -n1 "$OUT.fault-$fault.stderr")"
    [ $rc -eq 1 ] && how="sanitizer: $(grep -m1 -o 'AddressSanitizer: [a-z-]*\|runtime error: .*' "$OUT.fault-$fault.stderr")"
    echo "caught fault=$fault ($how)"
  fi
done

[ $fail = 0 ] && echo "G6: all traces verify clean; all faults caught" || echo "G6: FAILED"
exit $fail
