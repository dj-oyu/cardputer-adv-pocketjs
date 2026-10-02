#!/usr/bin/env bash
# The experiment remains default-off. Use separate object caches for both flags.
set -euo pipefail
cd "$(dirname "$0")/../.."
base=${VMTEST_TYPED_PUT_OUT:-$PWD/.cache/vmtest}
for enabled in 0 1; do
    out="$base"
    [[ $enabled == 1 ]] && out="$base-on"
    VMTEST_OUT="$out" VMTEST_CFLAGS="-fno-pie -no-pie -DPOCKET_VM_TYPED_PUT_INT_FAST=$enabled" bash tools/vmtest/build.sh all
    for variant in o2 asan; do
        VMTEST_OUT="$out" bash tools/vmtest/run.sh --variant "$variant"
    done
    VMTEST_OUT="$out" bash tools/vmtest/run.sh --variant o2 --force-yield
done
