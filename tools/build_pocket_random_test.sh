#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
QJS=components/quickjs-ng/quickjs-ng
CACHE=${CACHE:-/tmp/qjs-host}
mkdir -p "$CACHE"
# quickjs-vm: the VM hooks quickjs.c calls (L2c is not optional any more).
# Headers count too: this cache is shared across scripts and trees.
for f in dtoa libregexp libunicode quickjs quickjs-vm; do
  if [ ! -f "$CACHE/$f.o" ] || [ "$QJS/$f.c" -nt "$CACHE/$f.o" ] \
     || [ -n "$(find "$QJS" -name '*.h' -newer "$CACHE/$f.o" -print -quit)" ]; then
    gcc -c -O1 -g -w -DQUICKJS_NG_BUILD -D_GNU_SOURCE -I "$QJS" -I components/pocketjs_guest/include \
      "$QJS/$f.c" -o "$CACHE/$f.o"
  fi
done
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -I "$QJS" -I tools/hostshim -I main -I main/hal -I main/pocket \
  tools/test_pocket_random.c main/pocket/pocket_random.c main/pocket/pocket_api.c \
  tools/hostshim/hostshim_board.c \
  "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" "$CACHE/quickjs.o" "$CACHE/quickjs-vm.o" \
  -lm -o /tmp/test-pocket-random
/tmp/test-pocket-random
