#!/usr/bin/env bash
# Builds and runs tools/vmtest/tdz_elision.c (WSL). Extra args go to the
# binary (--print: counts only). VMTEST_QJS / VMTEST_OUT as in build.sh, to
# print the counts of an unmodified engine copy.
set -euo pipefail
cd "$(dirname "$0")/../.."
QJS=${VMTEST_QJS:-components/quickjs-ng/quickjs-ng}
OUT=${VMTEST_OUT:-$(pwd)/.cache/vmtest}
mkdir -p "$OUT/tdz" "$OUT/include"
[ -f "$OUT/include/sdkconfig.h" ] || echo "/* vmtest host stub: no CONFIG_* set */" > "$OUT/include/sdkconfig.h"
defs="-DQUICKJS_NG_BUILD -D_GNU_SOURCE -I $OUT/include -I components/pocketjs_guest/include -I $QJS"
objs=()
for f in dtoa libregexp libunicode quickjs-vm; do
  gcc -std=gnu11 -c -O1 -g -w $defs "$QJS/$f.c" -o "$OUT/tdz/$f.o"
  objs+=("$OUT/tdz/$f.o")
done
# quickjs.c is included by the test (-w: it is not warning-clean upstream).
gcc -std=gnu11 -O1 -g -w -fsanitize=address,undefined $defs tools/vmtest/tdz_elision.c \
    components/pocketjs_guest/src/block_cache.c "${objs[@]}" -lm -lpthread -ldl -o "$OUT/tdz/tdz_elision"
"$OUT/tdz/tdz_elision" tools/vmtest/corpus/tdz_elision.js "$@"
