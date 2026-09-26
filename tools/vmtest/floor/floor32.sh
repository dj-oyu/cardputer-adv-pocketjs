#!/bin/bash
# Per-intrinsic startup floor on the device layout (docs/vm/builtin-floor-plan.md §2).
# Builds the vmtest objects -m32 -malign-double (tools/vmtest/m32_sysroot.sh:
# 8 B JSValue, 4 B pointers, 8-aligned doubles, i.e. the Xtensa layout) into
# .cache/vmtest32 and links floor32.c against them. WSL only.
#
#   wsl -e bash tools/vmtest/floor/floor32.sh [VARIANT]   # default o2, e.g. o2-li
set -e
V=${1:-o2}
cd "$(dirname "$0")/../../.."
SP=tools/vmtest/floor
F=$(bash tools/vmtest/m32_sysroot.sh)
OUT=$PWD/.cache/vmtest32
mkdir -p "$OUT"
VMTEST_OUT=$OUT VMTEST_CFLAGS="$F" bash tools/vmtest/build.sh "$V" > "$OUT/floor32_build.log" 2>&1 || { tail -20 "$OUT/floor32_build.log"; exit 1; }
QJS=components/quickjs-ng/quickjs-ng
obj=$OUT/obj-$V
gcc -std=gnu11 -O2 $F -DQUICKJS_NG_BUILD -D_GNU_SOURCE -I "$OUT/include" -I components/pocketjs_guest/include -I $QJS \
  "$SP/floor32.c" components/pocketjs_guest/src/vm_sched.c components/pocketjs_guest/src/vm_clock.c \
  $obj/quickjs.o $obj/libregexp.o $obj/libunicode.o $obj/dtoa.o $obj/quickjs-vm.o -lm -lpthread -ldl -o "$OUT/floor32"
"$OUT/floor32"
