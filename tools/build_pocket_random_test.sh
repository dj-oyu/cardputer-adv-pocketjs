#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
QJS=components/quickjs-ng/quickjs-ng
CACHE=${CACHE:-/tmp/qjs-host}
mkdir -p "$CACHE"
for f in dtoa libregexp libunicode quickjs; do
  if [ ! -f "$CACHE/$f.o" ] || [ "$QJS/$f.c" -nt "$CACHE/$f.o" ]; then
    gcc -c -O1 -g -w -D_GNU_SOURCE -I "$QJS" "$QJS/$f.c" -o "$CACHE/$f.o"
  fi
done
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -I "$QJS" -I tools/hostshim -I main -I main/hal -I main/pocket \
  tools/test_pocket_random.c main/pocket/pocket_random.c main/pocket/pocket_api.c \
  tools/hostshim/hostshim_board.c \
  "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" "$CACHE/quickjs.o" \
  -lm -o /tmp/test-pocket-random
/tmp/test-pocket-random
