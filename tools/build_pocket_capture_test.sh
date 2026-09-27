#!/usr/bin/env bash
# tools/test_pocket_capture.c, built against the real quickjs-ng under ASan/UBSan.
# The same arrangement as tools/build_pocket_text_test.sh: QuickJS is compiled
# without the sanitizers and cached, and the files under test are not.
#
# What is real here: main/pocket/pocket_capture.c, main/pocket/pocket_api.c
# (promise slots, cancel tokens, capabilities, the pump) and main/ui/paint.c,
# which draws the recording indicator. What is faked: the microphone and the
# clock, both in the test file itself.
set -euo pipefail
cd "$(dirname "$0")/.."
QJS=components/quickjs-ng/quickjs-ng
OUT=${OUT:-/tmp/test-pocket-capture}
CACHE=${CACHE:-/tmp/qjs-host}
mkdir -p "$CACHE"
# main/ui/paint.c compiles against the generated 5x7 face, as the other host
# tests get it.
python3 tools/make_font.py "$CACHE/gen" >/dev/null
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
    -fno-omit-frame-pointer \
    -I "$QJS" -I "$CACHE/gen" -I tools/hostshim -I main -I main/hal -I main/pocket \
    -I main/ui -I main/text -I main/scene \
    tools/test_pocket_capture.c main/pocket/pocket_capture.c \
    main/pocket/pocket_api.c main/ui/paint.c \
    tools/hostshim/hostshim_board.c \
    "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" \
    "$CACHE/quickjs.o" "$CACHE/quickjs-vm.o" \
    -lm -o "$OUT"
echo "built $OUT"
