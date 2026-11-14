#!/usr/bin/env bash
# Run the block mover's fix-ups on live code (docs/vm/qjs-hotcold.md sec.6).
#
# A real profile only moves blocks the workloads never run, so its inverted
# branches and appended jumps sit on paths no test here takes. This gives
# every block of ~950 functions a random tier instead (selftest.py), links
# that quickjs.c into tools/r5sim, and runs STRESS, DERBY's demo race and
# LCD CATCH in QEMU: each must end as the plain image does -- same frames,
# no failure, the same allocations a turn, the same heap in use, and for
# DERBY the same race (its event lines).
#
#   tools/qjs_hotcold/torture.sh [<build root>] [<seeds, default "1 2">]
#
# Needs the ESP-IDF environment, Espressif's QEMU and Node 16+.
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
B=$(realpath -m "${1:-$R/.cache/hc_torture}")
SEEDS=${2:-1 2}
WORKLOADS=${WORKLOADS:-shiki derby hud}
export R5SIM_ICOUNT=shift=2,sleep=off FAST_WAIT_TICKS=2400
mkdir -p "$B"

# What must not move: everything but the instruction counts.
facts() { grep -E "R5SIM (done|device|alloc|derby)|DERBY" "$1" || true; }

for w in $WORKLOADS; do
    "$R/tools/r5sim/fast.sh" "$B/$w" -DR5SIM_DEVICE=1 -DR5SIM_APP="$w" > "$B/$w.txt" 2>&1 ||
        { cat "$B/$w.txt"; exit 1; }
    grep -q "R5SIM done frames=[0-9]* oom=0 fail=0 bad_present=0" "$B/$w.txt" || { cat "$B/$w.txt"; exit 1; }
done
for s in $SEEDS; do
    python "$R/tools/qjs_hotcold/selftest.py" "$B/shiki" --random 1 --seed "$s" --keep "$B/seed$s" | tail -1
    for w in $WORKLOADS; do
        D="$B/${w}_s$s"
        "$R/tools/r5sim/fast.sh" "$D" -DR5SIM_DEVICE=1 -DR5SIM_APP="$w" -DPOCKET_QJS_HOTCOLD=use \
            -DPOCKET_QJS_HOTCOLD_PROFILE="$B/seed$s/random0.json" > "$D.txt" 2>&1 || { cat "$D.txt"; exit 1; }
        grep -h "hc_cc:" "$D.fast-build.log" | head -1
        if diff <(facts "$B/$w.txt") <(facts "$D.txt") > "$D.diff"; then
            echo "torture: seed $s $w: same as the plain image ($(grep -E 'R5SIM ccount turns' "$D.txt" | tr -d '\r'))"
        else
            echo "torture: seed $s $w: DIFFERS from the plain image"; head -20 "$D.diff"; exit 1
        fi
    done
done
echo "torture: all runs match"
