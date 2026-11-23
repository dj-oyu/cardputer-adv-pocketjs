#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
QJS=components/quickjs-ng/quickjs-ng
CACHE=${CACHE:-/tmp/qjs-kasane-host}
OUT=${OUT:-/tmp/test-kobako-memory}
mkdir -p "$CACHE"
DEFS=(-DQUICKJS_NG_BUILD -DQJS_LEAN_LAZY_BUILTINS=1 -D_GNU_SOURCE)
for f in dtoa libregexp libunicode quickjs quickjs-vm; do
  if [ ! -f "$CACHE/$f.o" ] || [ "$QJS/$f.c" -nt "$CACHE/$f.o" ] ||
     [ -n "$(find "$QJS" -name '*.h' -newer "$CACHE/$f.o" -print -quit)" ]; then
    gcc -std=gnu11 -c -O1 -g -w "${DEFS[@]}" -I "$QJS" \
      -I components/kobako_guest/include "$QJS/$f.c" -o "$CACHE/$f.o"
  fi
done
gcc -std=gnu11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror \
  -fno-omit-frame-pointer -I "$QJS" -I tools/hostshim -I main/kobako \
  tools/test_kobako_memory.c tools/hostshim/kobako_api_stub.c \
  main/kobako/kobako_memory.c \
  "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" \
  "$CACHE/quickjs.o" "$CACHE/quickjs-vm.o" -lm -o "$OUT"
echo "built $OUT"
