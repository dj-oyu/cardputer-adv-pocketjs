#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
variant=${1:-asan}
mode=${2:---all}
case "$mode" in
  --functional-only) args=("$mode") ;;
  --all) args=() ;;
  *) echo "usage: $0 [asan|o2] [--functional-only|--all]" >&2; exit 2 ;;
esac
out=${VMTEST_OUT:-$PWD/.cache/vmtest}
flags=(-O2 -g)
if [[ $variant == asan ]]; then
  flags=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
VMTEST_CFLAGS="${VMTEST_CFLAGS:-} -fno-pie -no-pie" bash tools/vmtest/build.sh "$variant"
qjs=components/quickjs-ng/quickjs-ng
objs=()
for n in dtoa libregexp libunicode quickjs-libc quickjs-vm; do objs+=("$out/obj-$variant/$n.o"); done
gcc -std=gnu11 "${flags[@]}" -fno-pie -no-pie -w -DQUICKJS_NG_BUILD -D_GNU_SOURCE \
    -I "$out/include" -I "$qjs" -I components/pocketjs_guest/include \
    tools/vmtest/test_array_trim.c "${objs[@]}" -lm -lpthread -ldl -o "$out/array-trim-$variant"
"$out/array-trim-$variant" "${args[@]}"
