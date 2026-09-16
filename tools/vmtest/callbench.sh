#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
variant=${1:-o2-callbench}
out=${VMTEST_OUT:-$PWD/.cache/vmtest}
flags=(-O2 -g)
if [[ $variant == asan-callbench ]]; then
  flags=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined)
elif [[ $variant != o2-callbench ]]; then
  echo "expected o2-callbench or asan-callbench" >&2; exit 2
fi
gcc -std=gnu11 "${flags[@]}" -DCONFIG_POCKET_VM_CALLBENCH=1 -Wall -Wextra -Werror \
  -I components/quickjs-ng/quickjs-ng tools/vmtest/callbench.c \
  "$out/obj-$variant/"*.o -lm -lpthread -ldl -o "$out/callbench-$variant"
"$out/callbench-$variant" "${@:2}"
