#!/bin/bash
# Find the --fail-alloc numbers under which a corpus file reproduces its
# expected output on a given variant. For re-pinning the allocation-numbered
# regressions when a build changes how many allocations come before them
# (F1's ROM atoms remove ~540 at startup). Compares exactly as run.sh does:
# output minus #info / "vmrun: note:" lines, plus "exit=<code>". WSL:
#   bash tools/vmtest/floor/sweep_fail_alloc.sh VARIANT FILE FROM TO
set -uo pipefail
cd "$(dirname "$0")/.."
variant=$1 name=$2 from=$3 to=$4
bin=$PWD/../../.cache/vmtest/vmrun-$variant
exp=expected/$name.txt
for ((n = from; n <= to; n++)); do
  raw=$(cd corpus && VMTEST_START_MARKER=1 timeout 60 "$bin" --profile host --fail-alloc "$n" "$name.js" 2>&1)
  code=$?
  got=$(printf '%s\nexit=%s\n' "$raw" "$code" | grep -vE '^(#info|vmrun: note:)')
  [ "$got" = "$(cat "$exp")" ] && echo "MATCH $n"
done
echo "swept $from..$to"
