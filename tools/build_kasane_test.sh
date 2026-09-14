#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
QJS=components/quickjs-ng/quickjs-ng
OUT=${OUT:-/tmp/test-pocket-kasane}
CACHE=${CACHE:-/tmp/qjs-host}
mkdir -p "$CACHE"
for f in dtoa libregexp libunicode quickjs quickjs-vm; do
  if [ ! -f "$CACHE/$f.o" ] || [ "$QJS/$f.c" -nt "$CACHE/$f.o" ]; then
    gcc -c -O1 -g -w -D_GNU_SOURCE -I "$QJS" "$QJS/$f.c" -o "$CACHE/$f.o"
  fi
done
gcc -std=gnu11 ${CFLAGS:--O1 -g -fsanitize=address,undefined} -Wall -Wextra -Werror \
  -fno-omit-frame-pointer \
  -I "$QJS" -I tools/hostshim -I main -I main/pocket -I main/ui -I main/ui/kasane \
  tools/test_pocket_kasane.c tools/hostshim/pocket_api_stub.c \
  main/pocket/pocket_kasane.c main/ui/kasane/ksn_core.c main/ui/kasane/ksn_view.c \
  main/ui/kasane/ksn_cache.c main/ui/kasane/ksn_modal.c main/ui/kasane/ksn_render.c \
  "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" "$CACHE/quickjs.o" \
  "$CACHE/quickjs-vm.o" \
  -Wl,--wrap=calloc -lm -o "$OUT"
echo "built $OUT"
