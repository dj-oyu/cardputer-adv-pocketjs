#!/usr/bin/env bash
# Builds tools/test_js_ledger.c against the real quickjs-ng. Same cached-object
# trick as tools/build_pocket_text_test.sh: the vendor code is not sanitized,
# the file under test is.
set -euo pipefail
cd "$(dirname "$0")/.."
QJS=managed_components/espressif__quickjs-ng/quickjs-ng
OUT=${OUT:-/tmp/test-js-ledger}
CACHE=${CACHE:-/tmp/qjs-host}
mkdir -p "$CACHE"
for f in dtoa libregexp libunicode quickjs; do
  if [ ! -f "$CACHE/$f.o" ] || [ "$QJS/$f.c" -nt "$CACHE/$f.o" ]; then
    gcc -c -O1 -g -w -D_GNU_SOURCE -I "$QJS" "$QJS/$f.c" -o "$CACHE/$f.o"
  fi
done
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -I "$QJS" \
    tools/test_js_ledger.c \
    "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" "$CACHE/quickjs.o" \
    -lm -o "$OUT"
echo "built $OUT"
