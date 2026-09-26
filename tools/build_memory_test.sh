#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
QJS=components/quickjs-ng/quickjs-ng
CACHE=${CACHE:-/tmp/qjs-kasane-host}
OUT=${OUT:-/tmp/test-pocket-memory}
mkdir -p "$CACHE"
DEFS=(-DQUICKJS_NG_BUILD -D_GNU_SOURCE -DCONFIG_POCKET_VM_SEGFRAMES=1
      -DCONFIG_POCKET_VM_FLATCALLS=1 -DCONFIG_POCKET_VM_LAZY_INPUTS=1
      -DCONFIG_POCKET_VM_YIELD=1 -DCONFIG_POCKET_VM_STRIP_FN_SOURCE=1
      -DCONFIG_POCKET_VM_ROM_ATOMS=1 -DCONFIG_POCKET_VM_LAZY_BUILTINS=1)
for f in dtoa libregexp libunicode quickjs quickjs-vm; do
  if [ ! -f "$CACHE/$f.o" ] || [ "$QJS/$f.c" -nt "$CACHE/$f.o" ] ||
     [ -n "$(find "$QJS" -name '*.h' -newer "$CACHE/$f.o" -print -quit)" ]; then
    gcc -std=gnu11 -c -O1 -g -w "${DEFS[@]}" -I "$QJS" \
      -I components/pocketjs_guest/include "$QJS/$f.c" -o "$CACHE/$f.o"
  fi
done
gcc -std=gnu11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror \
  -fno-omit-frame-pointer -I "$QJS" -I tools/hostshim -I main/pocket \
  tools/test_pocket_memory.c tools/hostshim/pocket_api_stub.c \
  main/pocket/pocket_memory.c \
  "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" \
  "$CACHE/quickjs.o" "$CACHE/quickjs-vm.o" -lm -o "$OUT"
echo "built $OUT"
