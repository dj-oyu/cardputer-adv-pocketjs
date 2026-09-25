#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
QJS=components/quickjs-ng/quickjs-ng
OUT=${OUT:-/tmp/kasane-qjs-utf8-cost}
CACHE=${CACHE:-/tmp/qjs-kasane-utf8-cost}
mkdir -p "$CACHE"
DEFS="-DQUICKJS_NG_BUILD -D_GNU_SOURCE -DCONFIG_POCKET_VM_SEGFRAMES=1 -DCONFIG_POCKET_VM_FLATCALLS=1"
for f in dtoa libregexp libunicode quickjs quickjs-vm; do
  if [ ! -f "$CACHE/$f.o" ] || [ "$QJS/$f.c" -nt "$CACHE/$f.o" ] ||
     [ -n "$(find "$QJS" -name '*.h' -newer "$CACHE/$f.o" -print -quit)" ]; then
    gcc -std=gnu11 -O1 -g -w $DEFS -I "$QJS" -I components/pocketjs_guest/include \
        -c "$QJS/$f.c" -o "$CACHE/$f.o"
  fi
done
gcc -std=gnu11 -O2 -g -Wall -Wextra -Werror -I "$QJS" \
    tools/kasane_contract/test_qjs_utf8_cost.c \
    "$CACHE"/{dtoa,libregexp,libunicode,quickjs,quickjs-vm}.o -lm -o "$OUT"
"$OUT"
