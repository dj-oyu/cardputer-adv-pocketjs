#!/usr/bin/env bash
# G1 (docs/vm-L2-design.md sec.1.3): does C stack use per JS call depend on
# depth? deep_recursion.js's "max_depth" only says how many levels overflow
# the stack -- a 100 B/level implementation and a 1 KB/level implementation
# both eventually overflow, just at different depths. That is not the
# completion condition (sec.7 #1: "C stack use for pure JS deep calls is not
# proportional to depth"). This script IS the判定手段 that sec.1.1 says does
# not exist yet: it measures bytes of C stack consumed PER RECURSION LEVEL at
# two depths, N and 2N, and checks whether that per-level figure holds
# steady (today: yes, because JS_CallInternal recurses in C for every JS
# call -- that IS "proportional to depth", and this must report so honestly)
# or drops as depth grows (the L2 goal: a flattened call loop uses O(1) C
# stack regardless of JS depth, so total stack stays roughly fixed and the
# average per level falls as N grows).
#
#   tools/vmtest/stack_probe.sh                # depth 2000 vs 4000, o2 build
#   tools/vmtest/stack_probe.sh 5000            # depth 5000 vs 10000
#   tools/vmtest/stack_probe.sh 5000 asan       # pick the variant explicitly
#
# Machine-readable line on stdout (last line), for scripts/CI:
#   G1 depth=N bytes_per_call=A depth2=2N bytes_per_call2=B ratio=B/A verdict=PROPORTIONAL|NOT_PROPORTIONAL
#
# Exit: 0 if the measurement ran and produced a verdict, 1 if vmrun is
# missing or a run failed. The VERDICT is reported, not encoded as pass/fail
# exit status -- "PROPORTIONAL" on the current tree is the CORRECT answer
# (see docs/vm-L2-design.md sec.1.1 row #1), not a test failure. A caller
# that wants "has L2 met condition #1" checks the verdict string.
set -uo pipefail
cd "$(dirname "$0")"
HERE=$(pwd)
OUT=${VMTEST_OUT:-$HERE/../../.cache/vmtest}
N=${1:-2000}
variant=${2:-o2}
VMRUN=$OUT/vmrun-$variant
[ -x "$VMRUN" ] || { echo "missing $VMRUN; run tools/vmtest/build.sh $variant" >&2; exit 1; }

N2=$((N * 2))
depth_snippet=$(mktemp "${TMPDIR:-/tmp}/vmtest-probe-depth.XXXXXX.js")
trap 'rm -f "$depth_snippet"' EXIT

run_at() {
  local depth=$1
  printf 'var __VMTEST_PROBE_DEPTH = %d;\n' "$depth" > "$depth_snippet"
  # --stack-limit generous: this measures C stack, not the JS-visible limit
  # that deep_recursion.js exercises: the run must not hit JS_SetMaxStackSize
  # before reaching the requested depth, or the sample is truncated silently.
  # `ulimit -s unlimited` too: that raises the OS's real thread stack (the
  # thing G1 is actually measuring), which JS_SetMaxStackSize cannot touch --
  # confirmed the hard way, a depth of 20000 with only --stack-limit raised
  # segfaults (real C stack overflow, correctly, at ~672 B/level * 20000 =~
  # 13 MiB against the default 8 MiB thread stack). Skipped for asan: ASan
  # fixes its shadow memory to one address range at startup, and an unlimited
  # stack rlimit moves where the kernel puts the initial stack enough to
  # collide with it ("Shadow memory range interleaves"), which is an ASan
  # limitation, not a G1 result -- keep asan runs inside the default 8 MiB.
  local stack_cmd="ulimit -s unlimited"
  case "$variant" in asan*) stack_cmd=: ;; esac   # asan and asan-alloca alike
  (eval "$stack_cmd"; cd corpus && "$VMRUN" --profile host --stack-limit 512M --stack-probe \
      --include "$depth_snippet" ../stack_probe.js) 2>&1
}

out1=$(run_at "$N") || { echo "vmrun failed at depth=$N:" >&2; echo "$out1" >&2; exit 1; }
out2=$(run_at "$N2") || { echo "vmrun failed at depth=$N2:" >&2; echo "$out2" >&2; exit 1; }

line1=$(printf '%s\n' "$out1" | grep '^#info stack_probe ')
line2=$(printf '%s\n' "$out2" | grep '^#info stack_probe ')
[ -n "$line1" ] || { echo "no #info stack_probe line at depth=$N; full output:" >&2; echo "$out1" >&2; exit 1; }
[ -n "$line2" ] || { echo "no #info stack_probe line at depth=$N2; full output:" >&2; echo "$out2" >&2; exit 1; }

bpc1=$(printf '%s\n' "$line1" | sed -n 's/.*bytes_per_call=\([0-9.]*\).*/\1/p')
bpc2=$(printf '%s\n' "$line2" | sed -n 's/.*bytes_per_call=\([0-9.]*\).*/\1/p')
calls1=$(printf '%s\n' "$line1" | sed -n 's/.*calls=\([0-9]*\).*/\1/p')
calls2=$(printf '%s\n' "$line2" | sed -n 's/.*calls=\([0-9]*\).*/\1/p')

echo "depth=$N   calls=$calls1 bytes_per_call=$bpc1"
echo "depth=$N2  calls=$calls2 bytes_per_call=$bpc2"

# A ratio near 1.0 means the per-level cost did not change when depth
# doubled: that IS "proportional to depth" (total = per_level * depth, and
# per_level itself is depth-independent). A ratio well below 1.0 means the
# average is being diluted by growing depth without growing total stack in
# step -- the flattened-C-stack shape L2 is meant to produce. 0.85 is not a
# principled constant; it is "further from 1.0 than build-to-build i-cache
# alignment noise" (CLAUDE.md notes +-15% on unrelated kernels), so this does
# not misfire on measurement jitter alone.
ratio=$(awk -v a="$bpc1" -v b="$bpc2" 'BEGIN { if (a == 0) print "nan"; else printf "%.4f", b / a }')
verdict="PROPORTIONAL"
awk -v r="$ratio" 'BEGIN { exit !(r != "nan" && r < 0.85) }' && verdict="NOT_PROPORTIONAL"

echo "G1 depth=$N bytes_per_call=$bpc1 depth2=$N2 bytes_per_call2=$bpc2 ratio=$ratio verdict=$verdict"
