#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
QJS=components/quickjs-ng/quickjs-ng
OUT=${OUT:-/tmp/test-pocket-kasane}
CACHE=${CACHE:-/tmp/qjs-kasane-host}
mkdir -p "$CACHE"
# Match the shipping VM path. Keep this cache separate from harnesses with
# different defines, and invalidate it on header or build-script changes.
DEFS="-DQUICKJS_NG_BUILD -D_GNU_SOURCE -DCONFIG_POCKET_VM_SEGFRAMES=1 -DCONFIG_POCKET_VM_FLATCALLS=1"
for f in dtoa libregexp libunicode quickjs quickjs-vm; do
  if [ ! -f "$CACHE/$f.o" ] || [ "$QJS/$f.c" -nt "$CACHE/$f.o" ] || [ "$0" -nt "$CACHE/$f.o" ] \
     || [ -n "$(find "$QJS" -name '*.h' -newer "$CACHE/$f.o" -print -quit)" ]; then
    gcc -std=gnu11 -c -O1 -g -w $DEFS -I "$QJS" -I components/pocketjs_guest/include \
        "$QJS/$f.c" -o "$CACHE/$f.o"
  fi
done
gcc -std=gnu11 ${CFLAGS:--O1 -g -fsanitize=address,undefined} -Wall -Wextra -Werror \
  -fno-omit-frame-pointer \
  -I "$QJS" -I tools/hostshim -I main -I main/pocket -I main/ui -I main/ui/kasane \
  tools/test_pocket_kasane.c tools/hostshim/pocket_api_stub.c \
  main/pocket/pocket_kasane.c main/ui/kasane/ksn_runtime.c main/ui/kasane/ksn_core.c main/ui/kasane/ksn_view.c \
  main/ui/kasane/ksn_cache.c main/ui/kasane/ksn_modal.c main/ui/kasane/ksn_render.c \
  "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" "$CACHE/quickjs.o" \
  "$CACHE/quickjs-vm.o" \
  -Wl,--wrap=calloc -Wl,--wrap=free -lm -o "$OUT"
echo "built $OUT"
