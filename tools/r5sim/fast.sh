#!/usr/bin/env bash
# The fast measure (docs/vm/qjs-observe.md sec.9): build r5sim, run it in QEMU
# without the -d trace, print its R5SIM lines (the "ccount" line is the
# instruction count of turns 61..120 up to a constant). Seconds, not the
# half hour a trace and observe.py take.
#
#   tools/r5sim/fast.sh <build dir> [extra idf.py -D args...]
#   tools/r5sim/fast.sh .cache/r5dev -DR5SIM_DEVICE=1
#
# Needs the ESP-IDF environment (idf.py, esptool) and Espressif's QEMU
# (QEMU=... to override). Prints the image's flash/IRAM sizes too.
set -euo pipefail
D=$(realpath -m "$1"); shift
R=$(cd "$(dirname "$0")/../.." && pwd)
QEMU=${QEMU:-$(ls -d "$HOME"/.espressif/tools/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa 2>/dev/null | head -1)}
idf.py -C "$R/tools/r5sim" -B "$D" -DSDKCONFIG="$D/sdkconfig" "$@" build > "$D.fast-build.log" 2>&1 ||
    { tail -30 "$D.fast-build.log"; exit 1; }
(cd "$D" && python -m esptool --chip esp32s3 merge-bin --fill-flash-size 8MB -o qemu_flash.bin @flash_args > /dev/null)
rm -f "$D/fast-serial.txt"
"$QEMU" -machine esp32s3 -m 4M -drive file="$D/qemu_flash.bin",if=mtd,format=raw \
    -serial file:"$D/fast-serial.txt" -monitor none -display none -icount shift=2 &
QP=$!
for _ in $(seq 1 "${FAST_WAIT_TICKS:-600}"); do   # 0.5 s each
    grep -qE "R5SIM done|FAIL|Guru|abort" "$D/fast-serial.txt" 2>/dev/null && break
    sleep 0.5
done
kill $QP 2>/dev/null; wait $QP 2>/dev/null || true
grep -E "R5SIM|Guru|abort" "$D/fast-serial.txt" | tr -d '\r' | grep -v "STRESS f=" || true
idf.py -C "$R/tools/r5sim" -B "$D" -DSDKCONFIG="$D/sdkconfig" size 2>/dev/null |
    grep -E "Flash Code|IRAM|DIRAM|Total image" | head -6 || true
