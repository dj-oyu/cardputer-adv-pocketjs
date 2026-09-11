#!/usr/bin/env bash
# run_all.sh — drives every (allocator x trace) combination for the ledger.
# WSL only. Traces must already exist (tools/vmtest/run.sh --trace, plus
# apps/vmprobe/*.js run through vmrun --trace directly - see
# docs/vm-ledger/06-allocator-baseline.md for the exact commands).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
ROOT=../..
BIN=$ROOT/.cache/vmalloc/vmalloc_replay-o2
TRACE_DIRS=("$ROOT/.cache/vmtest/traces" "$ROOT/.cache/vmalloc/apptraces")
OUT=$ROOT/.cache/vmalloc/results.txt
DEVICE_POOL=163840   # 160 KiB, main/app_session.c's JS_SetMemoryLimit
: > "$OUT"

# bench_*.js (tools/vmtest/corpus/README table: "時間計測用") loop enough
# iterations to produce multi-million-line traces; a bisection there is
# O(log(range)) full replays of the whole trace and not what L2a segment
# sizing needs (it wants the workload *shapes* in the rest of the corpus,
# not a throughput microbenchmark's memory footprint). They still get a
# fixed-pool run, just sampled coarsely instead of every op.
is_bench() { case "$(basename "$1")" in bench_*) return 0 ;; *) return 1 ;; esac; }

for allocator in tlsf estalloc naive; do
  for dir in "${TRACE_DIRS[@]}"; do
    for t in "$dir"/*.trace; do
      [ -f "$t" ] || continue
      if is_bench "$t"; then
        # ~2,000 samples per trace, not a fixed stride: a fixed 4001 sampled
        # the four ~2.5k-line bench traces only at op 0 and after teardown, so
        # their peak_used was the empty pool (0.48K) -- never observed at all.
        every=$(( $(wc -l < "$t") / 2000 + 1 ))
        "$BIN" --allocator "$allocator" --pool "$DEVICE_POOL" --sample-every "$every" "$t" >> "$OUT"
      else
        "$BIN" --allocator "$allocator" --bisect "$t" >> "$OUT"
        "$BIN" --allocator "$allocator" --pool "$DEVICE_POOL" --sample-every 1 "$t" >> "$OUT"
      fi
    done
  done
done
echo "wrote $OUT"
wc -l "$OUT"
