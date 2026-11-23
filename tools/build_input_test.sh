#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
QJS=components/quickjs-ng/quickjs-ng
OUT=${OUT:-/tmp/test-kobako-input}
CACHE=${CACHE:-/tmp/qjs-kasane-host}
mkdir -p "$CACHE"
# Match the shipping VM path. Keep this cache separate from harnesses with
# different defines, and invalidate it on header or build-script changes.
DEFS="-DQUICKJS_NG_BUILD -DQJS_LEAN_LAZY_BUILTINS=1 -D_GNU_SOURCE"
for f in dtoa libregexp libunicode quickjs quickjs-vm; do
  if [ ! -f "$CACHE/$f.o" ] || [ "$QJS/$f.c" -nt "$CACHE/$f.o" ] || [ "$0" -nt "$CACHE/$f.o" ] \
     || [ -n "$(find "$QJS" -name '*.h' -newer "$CACHE/$f.o" -print -quit)" ]; then
    gcc -std=gnu11 -c -O1 -g -w $DEFS -I "$QJS" -I components/kobako_guest/include \
        "$QJS/$f.c" -o "$CACHE/$f.o"
  fi
done
python3 - "$CACHE/kobako_sub_impl.inc" <<'PY'
from pathlib import Path
import sys
s=Path('main/kobako/kobako_api.c').read_text()
a=s.index('// ----------------------------------------------------------- subscriptions')
b=s.index('// ------------------------------------------------------- async completions',a)
Path(sys.argv[1]).write_text(s[a:b])
PY

gcc -std=gnu11 ${CFLAGS:--O1 -g -fsanitize=address,undefined} -Wall -Wextra -Werror \
  -fno-omit-frame-pointer \
  -I "$QJS" -I "$CACHE" -I tools/hostshim -I main -I main/kobako -I main/hal -I main/ui/kasane \
  tools/test_kobako_input.c tools/hostshim/kobako_api_stub.c main/kobako/kobako_input.c \
  main/hal/keymap.c main/hal/keystate.c \
  "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" "$CACHE/quickjs.o" \
  "$CACHE/quickjs-vm.o" \
  -lm -o "$OUT"
echo "built $OUT"
