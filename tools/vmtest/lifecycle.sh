#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
variant=${1:-asan}
out=${VMTEST_OUT:-$PWD/.cache/vmtest}
qjs=components/quickjs-ng/quickjs-ng
flags=(-O2 -g)
if [[ $variant == asan* ]]; then
  flags=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
gcc -std=gnu11 "${flags[@]}" -Wall -Wextra -Werror -I "$qjs" \
  tools/vmtest/lifecycle.c "$out/obj-$variant/"*.o -lm -lpthread -ldl -o "$out/lifecycle-$variant"
"$out/lifecycle-$variant"
