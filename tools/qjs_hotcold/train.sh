#!/usr/bin/env bash
# Train the hot/cold profile of quickjs.c (docs/vm/qjs-hotcold.md sec.4):
# build tools/r5sim with a symbol in front of every basic block
# (-DPOCKET_QJS_HOTCOLD=gen), run each workload in QEMU with a block trace,
# and write tools/qjs_hotcold/profile.json.
#
#   tools/qjs_hotcold/train.sh [<build root>]        (default .cache/hc_train)
#   WORKLOADS="shiki hud" tools/qjs_hotcold/train.sh   (a subset; default "shiki derby hud")
#
# Needs the ESP-IDF environment, Espressif's QEMU (QEMU=... to override) and
# Node 16+ (the Shiki workloads). The trace goes through a FIFO, not to disk
# (a full DERBY race is tens of GB as a file). About 20 minutes on 4 cores.
#
# Retrain when quickjs.c or its compile options change, when the compiler
# changes, or when the workloads do. A stale profile is not dangerous: a
# function whose instructions no longer match its hash is left as GCC wrote
# it (hc_cc.py prints how many).
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
B=$(realpath -m "${1:-$R/.cache/hc_train}")
QEMU=${QEMU:-$(ls -d "$HOME"/.espressif/tools/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa 2>/dev/null | head -1)}
WORKLOADS=${WORKLOADS:-shiki derby hud}
mkdir -p "$B"
RUNS=()
for w in $WORKLOADS; do
    D="$B/$w"
    idf.py -C "$R/tools/r5sim" -B "$D" -DSDKCONFIG="$D/sdkconfig" -DR5SIM_DEVICE=1 -DR5SIM_APP="$w" \
        -DPOCKET_QJS_HOTCOLD=gen build > "$D.build.log" 2>&1 || { tail -30 "$D.build.log"; exit 1; }
    (cd "$D" && python -m esptool --chip esp32s3 merge-bin --pad-to-size 8MB -o qemu_flash.bin @flash_args > /dev/null)
    rm -f "$D/serial.txt" "$D/trace.fifo"
    mkfifo "$D/trace.fifo"
    python "$R/tools/qjs_hotcold/hc_profile.py" run "$D/trace.fifo" "$D/r5sim.elf" "$D/run.json" &
    PP=$!
    "$QEMU" -machine esp32s3 -m 4M -drive file="$D/qemu_flash.bin",if=mtd,format=raw \
        -serial file:"$D/serial.txt" -monitor none -display none -icount shift=2,sleep=off \
        -d exec,nochain,in_asm -dfilter 0x42000000..0x43ffffff -D "$D/trace.fifo" &
    QP=$!
    for _ in $(seq 1 14400); do   # 0.5 s each
        grep -qE "R5SIM done|FAIL|Guru|abort" "$D/serial.txt" 2>/dev/null && break
        sleep 0.5
    done
    kill $QP 2>/dev/null; wait $QP 2>/dev/null || true
    wait $PP
    rm -f "$D/trace.fifo"
    grep -q "R5SIM done frames=[0-9]* oom=0 fail=0 bad_present=0" "$D/serial.txt" ||
        { grep -E "R5SIM|Guru|abort" "$D/serial.txt" | tail -5; echo "train.sh: $w did not finish cleanly" >&2; exit 1; }
    RUNS+=("$D/run.json")
done
# Every image compiles quickjs.c the same way; take the assembly of the first.
ASM=$(ls "$B"/${WORKLOADS%% *}/esp-idf/quickjs-ng/CMakeFiles/*/quickjs-ng/quickjs.c.obj.hc0.s | head -1)
python "$R/tools/qjs_hotcold/hc_profile.py" merge "$ASM" "$R/tools/qjs_hotcold/profile.json" "${RUNS[@]}" \
    --note "r5sim -DR5SIM_DEVICE=1, workloads: $WORKLOADS, $(git -C "$R" rev-parse --short HEAD)"
