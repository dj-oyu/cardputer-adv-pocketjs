#!/bin/bash
# Fresh-context cost, touched-builtin cost and full-materialization cost on
# the device layout (docs/vm/rom-intrinsics.md sec.2). Needs floor32.sh first
# (the -m32 objects in .cache/vmtest32). WSL only.
#   wsl -e bash tools/vmtest/floor/rom_walk.sh
set -e
cd "$(dirname "$0")/../../.."
SP=tools/vmtest/floor
F=$(bash tools/vmtest/m32_sysroot.sh)
OUT=$PWD/.cache/vmtest32
QJS=components/quickjs-ng/quickjs-ng
obj=$OUT/obj-o2
gcc -std=gnu11 -O2 $F -w -DQUICKJS_NG_BUILD -D_GNU_SOURCE -DQJS_DISABLE_DUMPS -I "$OUT/include" -I components/pocketjs_guest/include -I $QJS \
  "$SP/rom_walk.c" components/pocketjs_guest/src/vm_sched.c components/pocketjs_guest/src/vm_clock.c \
  $obj/libregexp.o $obj/libunicode.o $obj/dtoa.o $obj/quickjs-vm.o -lm -lpthread -ldl -o "$OUT/rom_walk"
"$OUT/rom_walk"
for s in "$SP"/rom_touch/*.js; do echo; "$OUT/rom_walk" "$s" | grep -A3 TOUCH; done
