#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
QJS=components/quickjs-ng/quickjs-ng
OUT=${OUT:-/tmp/test-pocket-input}
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
python3 - "$CACHE/pocket_sub_impl.inc" <<'PY'
from pathlib import Path
import sys
s=Path('main/pocket/pocket_api.c').read_text()
a=s.index('// ----------------------------------------------------------- subscriptions')
b=s.index('// ------------------------------------------------------- async completions',a)
Path(sys.argv[1]).write_text(s[a:b])
PY

gcc -std=gnu11 ${CFLAGS:--O1 -g -fsanitize=address,undefined} -Wall -Wextra -Werror \
  -fno-omit-frame-pointer \
  -I "$QJS" -I "$CACHE" -I tools/hostshim -I main -I main/pocket \
  tools/test_pocket_input.c tools/hostshim/pocket_api_stub.c main/pocket/pocket_input.c \
  "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" "$CACHE/quickjs.o" \
  "$CACHE/quickjs-vm.o" \
  -lm -o "$OUT"
echo "built $OUT"
