#!/usr/bin/env bash
# The Kasane API reorganisation's measure (docs/kasane/api-reorg.md sec.6.1,
# P0 item 5): the four r5sim workloads through fast.sh, then the firmware's
# size, as one row of docs/kasane/logs/api-reorg-perf.md.
#
#   tools/r5sim/reorg.sh [-a] [-n] [<build root>]     (default .cache/reorg)
#     -a  append the row to docs/kasane/logs/api-reorg-perf.md
#     -n  skip the firmware build (its columns read "-")
#
# Needs the ESP-IDF environment, Espressif's QEMU and Node 16+ (as fast.sh).
# QEMU runs with -icount shift=2,sleep=off: with sleep=on (fast.sh's default)
# the virtual clock follows the host's while the guest idles at boot, the
# tick interrupt lands at other points of the turns from run to run, and a
# scene of a few dozen turns moves by a few counts. With sleep=off two runs
# of one image print the same numbers, every one of them.
#
# The counts depend on the image's layout to a few hundredths of a percent
# (docs/kasane/logs/api-reorg-perf.md "読み方"): compare rows built from the
# same checkout path, and read a change below ~0.05% as layout, not work.
set -euo pipefail
APPEND=0 FW=1
while getopts an o; do
    case $o in a) APPEND=1 ;; n) FW=0 ;; *) exit 2 ;; esac
done
shift $((OPTIND - 1))
R=$(cd "$(dirname "$0")/../.." && pwd)
B=$(realpath -m "${1:-$R/.cache/reorg}")
mkdir -p "$B"
export R5SIM_ICOUNT=${R5SIM_ICOUNT:-shift=2,sleep=off}
export FAST_WAIT_TICKS=${FAST_WAIT_TICKS:-2400}
OUT="$B/out"; mkdir -p "$OUT"

run() {   # run <workload>: fast.sh's lines in $OUT/<workload>.txt
    "$R/tools/r5sim/fast.sh" "$B/$1" -DR5SIM_DEVICE=1 -DR5SIM_APP="$1" > "$OUT/$1.txt" 2>&1 ||
        { cat "$OUT/$1.txt" >&2; echo "reorg.sh: $1 failed" >&2; exit 1; }
    grep -q "R5SIM done frames=[0-9]* oom=0 fail=0 bad_present=0" "$OUT/$1.txt" ||
        { cat "$OUT/$1.txt" >&2; echo "reorg.sh: $1 did not finish cleanly" >&2; exit 1; }
}
for w in shiki shiki_v1 hud derby; do run "$w"; done

# field <file> <line pattern> <key>: the value of key= on the first such line.
field() { grep -E "$2" "$1" | head -1 | tr ' ' '\n' | sed -n "s/^$3=//p" | head -1; }
turn() { field "$OUT/$1.txt" "R5SIM ccount turns=" per_turn; }
alloc() { field "$OUT/$1.txt" "R5SIM alloc " per_turn; }
heap() { field "$OUT/$1.txt" "R5SIM device frames=" heap_used; }
scene() { field "$OUT/derby.txt" "R5SIM ccount scene=$1 " per_turn; }
flash() { grep -E "Flash Code" "$OUT/$1.txt" | head -1 | awk -F'│' '{gsub(/ /,"",$3);print $3}'; }

FW_DIRAM=- FW_FLASH=-
if [ "$FW" = 1 ]; then
    F="$B/fw"
    idf.py -C "$R" -B "$F" -DSDKCONFIG="$F/sdkconfig" build > "$F.build.log" 2>&1 ||
        { tail -30 "$F.build.log" >&2; echo "reorg.sh: firmware build failed" >&2; exit 1; }
    idf.py -C "$R" -B "$F" -DSDKCONFIG="$F/sdkconfig" size > "$OUT/fw_size.txt" 2>&1
    FW_DIRAM=$(grep -E "│ *DIRAM" "$OUT/fw_size.txt" | head -1 | awk -F'│' '{gsub(/ /,"",$3);print $3}')
    FW_FLASH=$(grep -E "│ *Flash Code" "$OUT/fw_size.txt" | head -1 | awk -F'│' '{gsub(/ /,"",$3);print $3}')
fi

SHA=$(git -C "$R" rev-parse --short HEAD)
git -C "$R" diff --quiet HEAD -- main components apps tools/r5sim 2>/dev/null || SHA="$SHA+dirty"
ROW="| $(date +%F) | \`$SHA\` | $(turn shiki) | $(turn shiki_v1) | $(turn hud) | $(turn derby) |"
ROW="$ROW $(scene wait) | $(scene wide) | $(scene field) | $(scene vision) | $(scene headon) |"
ROW="$ROW $(alloc shiki) / $(alloc shiki_v1) / $(alloc hud) / $(alloc derby) |"
ROW="$ROW $(heap shiki) / $(heap shiki_v1) / $(heap hud) / $(heap derby) |"
ROW="$ROW $(flash shiki) | $FW_DIRAM | $FW_FLASH |"
echo "$ROW"
if [ "$APPEND" = 1 ]; then
    echo "$ROW" >> "$R/docs/kasane/logs/api-reorg-perf.md"
    echo "appended to docs/kasane/logs/api-reorg-perf.md"
fi
