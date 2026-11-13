#!/usr/bin/env bash
# Builds and runs tools/vmtest/cfunc_join.c against the asan engine objects
# from tools/vmtest/build.sh (WSL). VMTEST_QJS / VMTEST_OUT as in build.sh.
set -euo pipefail
cd "$(dirname "$0")/../.."
QJS=${VMTEST_QJS:-components/quickjs-ng/quickjs-ng}
OUT=${VMTEST_OUT:-$(pwd)/.cache/vmtest}
bash tools/vmtest/build.sh asan >/dev/null
obj=$OUT/obj-asan
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DQUICKJS_NG_BUILD -D_GNU_SOURCE -I "$OUT/include" -I components/pocketjs_guest/include -I "$QJS" \
    tools/vmtest/cfunc_join.c components/pocketjs_guest/src/vm_sched.c \
    components/pocketjs_guest/src/vm_clock.c components/pocketjs_guest/src/block_cache.c \
    "$obj"/{dtoa,libregexp,libunicode,quickjs,quickjs-libc,quickjs-vm}.o \
    -lm -lpthread -ldl -o "$OUT/cfunc_join"
"$OUT/cfunc_join"
