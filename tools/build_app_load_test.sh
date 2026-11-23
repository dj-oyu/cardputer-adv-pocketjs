#!/usr/bin/env bash
# kobako.app.load() on the host (tools/test_app_load.c): the real
# kobako_app_load.c, kobako_api.c and app_registry.c on the real QuickJS,
# under ASan/UBSan. WSL/Linux only.
set -euo pipefail
cd "$(dirname "$0")/.."
QJS=components/quickjs-ng/quickjs-ng
CACHE=${CACHE:-/tmp/qjs-host}
mkdir -p "$CACHE"
# The object cache build_kobako_random_test.sh keeps (headers count too).
for f in dtoa libregexp libunicode quickjs quickjs-vm; do
  if [ ! -f "$CACHE/$f.o" ] || [ "$QJS/$f.c" -nt "$CACHE/$f.o" ] \
     || [ -n "$(find "$QJS" -name '*.h' -newer "$CACHE/$f.o" -print -quit)" ]; then
    gcc -c -O1 -g -w -DQUICKJS_NG_BUILD -DQJS_LEAN_LAZY_BUILTINS=1 -D_GNU_SOURCE -I "$QJS" -I components/kobako_guest/include \
      "$QJS/$f.c" -o "$CACHE/$f.o"
  fi
done
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -I "$QJS" -I tools/hostshim -I main -I main/hal -I main/kobako -I main/text \
  tools/test_app_load.c main/kobako/kobako_app_load.c main/kobako/kobako_api.c \
  main/kobako/app_registry.c tools/hostshim/app_chunks_host.c tools/hostshim/hostshim_board.c \
  "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" "$CACHE/quickjs.o" "$CACHE/quickjs-vm.o" \
  -lm -o /tmp/test-app-load
/tmp/test-app-load
