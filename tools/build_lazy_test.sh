#!/usr/bin/env bash
# Builds tools/test_lazy_namespace.c against the quickjs-ng the firmware links.
# Host only; nothing here is flashed. Run from tools/ under WSL:
#
#   wsl -e bash -lc "cd tools && ./build_lazy_test.sh && ./test_lazy_namespace"
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
qjs="$here/../managed_components/espressif__quickjs-ng/quickjs-ng"
[ -d "$qjs" ] || { echo "quickjs-ng not found; run tools/prepare_dependencies.py" >&2; exit 1; }

# One object per source, cached, because quickjs.c alone is close to a minute.
out="$here/.lazytest"
mkdir -p "$out"
flags="-O1 -g -I$qjs -DCONFIG_VERSION=\"host-test\" -Wno-unused-parameter"
for src in quickjs libregexp libunicode cutils dtoa xsum; do
    [ -f "$qjs/$src.c" ] || continue
    if [ ! -f "$out/$src.o" ] || [ "$qjs/$src.c" -nt "$out/$src.o" ]; then
        echo "cc $src.c"
        # shellcheck disable=SC2086
        gcc $flags -c "$qjs/$src.c" -o "$out/$src.o"
    fi
done
echo "cc test_lazy_namespace.c"
# shellcheck disable=SC2086
gcc $flags "$here/test_lazy_namespace.c" "$out"/*.o -lm -lpthread \
    -o "$here/test_lazy_namespace"
echo "built $here/test_lazy_namespace"
