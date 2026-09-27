#!/usr/bin/env bash
# Builds tools/test_js_ledger.c against the real quickjs-ng. Same cached-object
# trick as tools/build_pocket_text_test.sh: the vendor code is not sanitized,
# the file under test is.
set -euo pipefail
cd "$(dirname "$0")/.."
QJS=components/quickjs-ng/quickjs-ng
OUT=${OUT:-/tmp/test-js-ledger}
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
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -I "$QJS" \
    tools/test_js_ledger.c \
    "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" "$CACHE/quickjs.o" "$CACHE/quickjs-vm.o" \
    -lm -o "$OUT"
echo "built $OUT"
