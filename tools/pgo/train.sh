#!/bin/bash
# Trains the QuickJS profile (docs/vm/qjs-pgo.md): builds the instrumented
# r5sim image, runs STRESS in QEMU, and writes the counters to tools/pgo/qjs/.
#
#   . $IDF_PATH/export.sh            # ESP-IDF v6.0.1, the firmware's toolchain
#   tools/pgo/train.sh [build dir]   # default: build_pgo_gen
#
# Needs qemu-system-xtensa (Espressif's, esp_develop_9.2.2) on PATH or in
# $QEMU. Linux or WSL. The .gcda format is tied to the compiler version: train
# with the same GCC (esp-15.2.0) that builds the firmware.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
build="$(realpath -m "${1:-$repo/build_pgo_gen}")"
qemu="${QEMU:-qemu-system-xtensa}"
out="$repo/tools/pgo/qjs"
gcov_tool="xtensa-esp32s3-elf-gcov-tool"

idf.py -C "$repo/tools/r5sim" -B "$build" -DSDKCONFIG="$build/sdkconfig" -DR5SIM_PGO=generate \
    "-DSDKCONFIG_DEFAULTS=$repo/tools/r5sim/sdkconfig.defaults;$repo/tools/r5sim/sdkconfig.pgo_gen.defaults" \
    build
cd "$build"
python -m esptool --chip esp32s3 merge-bin --fill-flash-size 8MB -o qemu_flash.bin @flash_args
rm -f serial.txt
# -icount: the virtual clock follows the instruction count, as in the trace
# runs (docs/vm/turn-icache.md 3.4.1). -m 4M: PSRAM for the counters.
"$qemu" -machine esp32s3 -m 4M -drive file=qemu_flash.bin,if=mtd,format=raw \
    -serial file:serial.txt -monitor none -display none -icount shift=2 &
pid=$!
for _ in $(seq 1 600); do
    sleep 1
    grep -q "R5SIM done" serial.txt 2>/dev/null && break
    kill -0 $pid 2>/dev/null || break
done
kill $pid 2>/dev/null || true
wait $pid 2>/dev/null || true
grep -q "R5SIM done frames=120 oom=0 fail=0" serial.txt || {
    echo "train.sh: the run did not finish cleanly:" >&2; tail -5 serial.txt >&2; exit 1; }
grep -q "GCOV end" serial.txt || { echo "train.sh: no counter dump" >&2; exit 1; }

# The dump names each file by the training build's object path; merge-stream
# writes there (and would add to a file already present, so clear it first).
obj_dir="$build/esp-idf/quickjs-ng/CMakeFiles/__idf_quickjs-ng.dir"
find "$obj_dir" -name '*.gcda' -delete
python - serial.txt gcov.stream <<'EOF'
import sys
data = bytearray()
on = False
for line in open(sys.argv[1], errors='replace'):
    line = line.strip()
    if line == 'GCOV begin': on = True; continue
    if line == 'GCOV end': break
    if on and line.startswith('GCOV '):
        data += bytes.fromhex(line[5:])
open(sys.argv[2], 'wb').write(data)
print(f'{len(data)} bytes of gcda stream')
EOF
"$gcov_tool" merge-stream < gcov.stream
rm -rf "$out"
# .ids: ident, line, file (relative to the component) and name of every
# function, from the .gcno notes -- what tools/pgo/rekey.py needs to key the
# profile for another tree.
comp="$repo/components/quickjs-ng"
(cd "$obj_dir" && find . -name '*.gcda' | while read -r f; do
    mkdir -p "$out/$(dirname "$f")"; cp "$f" "$out/$f"
    xtensa-esp32s3-elf-gcov-dump "${f%.gcda}.gcno" | python -c '
import re, sys
comp = sys.argv[1].rstrip("/") + "/"
pat = re.compile(r"FUNCTION ident=(\d+),.*`([^\x27]+)\x27 (\S+?):(\d+):\d+-")
for line in sys.stdin:
    m = pat.search(line)
    if m:
        ident, name, path, ln = m.groups()
        if not path.startswith(comp):
            sys.exit(f"train.sh: {name} is defined outside the component: {path}")
        print(ident, ln, path[len(comp):], name)
' "$comp" > "$out/$f.ids"
done)
grep "R5SIM done" serial.txt
find "$out" -name '*.gcda' -printf '%P %s B\n' | sort
