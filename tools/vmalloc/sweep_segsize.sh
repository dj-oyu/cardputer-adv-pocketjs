#!/usr/bin/env bash
# sweep_segsize.sh — the numbers behind the standard segment size (spec
# sec.7: "標準セグメントサイズは L0 の使用量分布から決める"). For each
# candidate size, every non-bench trace is replayed on the -O2 build twice:
# a bisection for the smallest pool that fits, and a fixed device-sized pool
# (160 KiB, main/app_session.c's JS_SetMemoryLimit) that reports the
# internal-slack / external-fragmentation split at its worst samples.
#
#   bash tools/vmalloc/sweep_segsize.sh                 # 1024 2048 4096 8192 16384
#   bash tools/vmalloc/sweep_segsize.sh 2048 4096       # a subset
#
# Output: .cache/vmalloc/segsize.txt (replay.c lines, prefixed seg_size=N),
# and a per-size summary on stdout: sums over traces of min_pool, and the
# medians of peak internal slack / external fragmentation / malloc steps.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
ROOT=../..
BIN=${VMALLOC_BIN:-$ROOT/.cache/vmalloc/vmalloc_replay-o2}
TRACES=$ROOT/.cache/vmtest/traces
OUT=$ROOT/.cache/vmalloc/segsize.txt
DEVICE_POOL=163840
sizes=("$@")
[ ${#sizes[@]} -eq 0 ] && sizes=(1024 2048 4096 8192 16384)
[ -x "$BIN" ] || { echo "missing $BIN; run tools/vmalloc/build.sh" >&2; exit 2; }
: > "$OUT"

for sz in "${sizes[@]}"; do
  for t in "$TRACES"/*.trace; do
    case "$(basename "$t")" in bench_*) continue ;; esac
    {
      printf 'seg_size=%s ' "$sz"; "$BIN" --allocator segment --seg-size "$sz" --bisect --bisect-max 8388608 "$t"
      printf 'seg_size=%s ' "$sz"; "$BIN" --allocator segment --seg-size "$sz" --pool "$DEVICE_POOL" --sample-every 1 "$t"
    } >> "$OUT"
  done
done

python3 - "$OUT" <<'EOF'
import re, sys, statistics
rows = {}
for line in open(sys.argv[1]):
    kv = dict(re.findall(r'(\w+)=(\S+)', line))
    sz = int(kv['seg_size'])
    r = rows.setdefault(sz, {'min_pool': [], 'slack': [], 'cached': [], 'frag': [], 'app_frag': [], 'app_slack': [], 'steps': [], 'ok': 0, 'n': 0, 'fail_above': 0})
    if 'min_pool' in kv:
        r['min_pool'].append(int(kv['min_pool']))
    elif 'bisect' in kv:
        r['fail_above'] += 1
    elif 'result' in kv:
        r['n'] += 1
        r['ok'] += kv['result'] == 'OK'
        r['slack'].append(int(kv['peak_seg_free_inside']))
        r['cached'].append(int(kv['peak_seg_cached']))
        r['frag'].append(int(kv['peak_external_frag']))
        r['app_frag'].append(int(kv['app_ext_frag']))
        r['app_slack'].append(int(kv['app_slack_inside']))
        r['steps'].append(int(kv['malloc_steps_max']))
print("(medians over traces at the 160 KiB pool; 'app_' = sampled before '# teardown' only, 'peak_' = whole trace incl. teardown)")
print("seg_size  sum_min_pool(KiB)  fits160K  med_app_slack_inside  med_app_ext_frag  med_peak_slack_inside  med_peak_ext_frag  med_peak_cached  med_malloc_steps_max  max_malloc_steps_max")
for sz in sorted(rows):
    r = rows[sz]
    print(f"{sz:8d}  {sum(r['min_pool'])/1024:17.1f}  {r['ok']:2d}/{r['n']:<5d}  {statistics.median(r['app_slack']):20.0f}  {statistics.median(r['app_frag']):16.0f}  {statistics.median(r['slack']):21.0f}  {statistics.median(r['frag']):17.0f}  {statistics.median(r['cached']):15.0f}  {statistics.median(r['steps']):20.0f}  {max(r['steps']):20d}")
    if r['fail_above']:
        print(f"          ({r['fail_above']} trace(s) did not fit the bisect maximum)")
EOF
echo "wrote $OUT"
