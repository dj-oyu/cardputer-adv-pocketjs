#!/usr/bin/env bash
# tools/test_pocket_text.c, built against the real quickjs-ng under ASan/UBSan.
# QuickJS itself is compiled without the sanitizers (60k lines of vendor code,
# and the object is cached between runs); the file under test is not, which is
# what decides whether a read of a freed session is reported.
set -euo pipefail
cd "$(dirname "$0")/.."
QJS=components/quickjs-ng/quickjs-ng
OUT=${OUT:-/tmp/test-pocket-text}
CACHE=${CACHE:-/tmp/qjs-host}
mkdir -p "$CACHE"
# main/ui/paint.c compiles against the generated 5x7 face, the same way
# tools/test_codeedit.c gets it.
python3 tools/make_font.py "$CACHE/gen" >/dev/null
for f in dtoa libregexp libunicode quickjs; do
  if [ ! -f "$CACHE/$f.o" ] || [ "$QJS/$f.c" -nt "$CACHE/$f.o" ]; then
    gcc -c -O1 -g -w -D_GNU_SOURCE -I "$QJS" "$QJS/$f.c" -o "$CACHE/$f.o"
  fi
done
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer \
    -I "$QJS" -I "$CACHE/gen" -I tools/hostshim -I main -I main/hal -I main/pocket -I main/ui \
    -I main/text -I main/scene \
    tools/test_pocket_text.c tools/hostshim/pocket_api_stub.c \
    tools/hostshim/hostshim.c main/pocket/pocket_text.c main/text/textfield.c \
    main/ui/paint.c \
    "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" \
    "$CACHE/quickjs.o" \
    -lm -o "$OUT"
echo "built $OUT"
