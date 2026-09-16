#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
variant=${1:-asan-tco}
case "$variant" in
  asan-tco|asan-yield|o2-tco|o2-yield) ;;
  *) echo "expected a yield-enabled variant" >&2; exit 2 ;;
esac
bash tools/vmtest/build.sh "$variant"
out=${VMTEST_OUT:-$PWD/.cache/vmtest}
qjs=components/quickjs-ng/quickjs-ng
flags=(-O2 -g)
if [[ $variant == asan-* ]]; then
  flags=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
gcc -std=gnu11 "${flags[@]}" -Wall -Wextra -Werror -I "$qjs" \
  tools/vmtest/tco_oom.c "$out/obj-$variant/"*.o -lm -lpthread -ldl -o "$out/tco-oom-$variant"
"$out/tco-oom-$variant"
