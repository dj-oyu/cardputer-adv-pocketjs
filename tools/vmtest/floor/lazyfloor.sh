#!/bin/bash
# Heap walk after JS_NewContext and the savings of the two planned steps
# (docs/vm/builtin-floor-plan.md §6). Needs the -m32 objects that floor32.sh
# builds into .cache/vmtest32 (run floor32.sh first). lazyfloor.c #includes
# quickjs.c so it can walk rt->atom_array and rt->gc_obj_list from inside,
# hence quickjs.o is not linked here. WSL only.
#
#   wsl -e bash tools/vmtest/floor/lazyfloor.sh
set -e
cd "$(dirname "$0")/../../.."
SP=tools/vmtest/floor
F=$(bash tools/vmtest/m32_sysroot.sh)
OUT=$PWD/.cache/vmtest32
QJS=components/quickjs-ng/quickjs-ng
obj=$OUT/obj-o2
[ -f "$obj/quickjs-vm.o" ] || { echo "run tools/vmtest/floor/floor32.sh first (builds $obj)"; exit 1; }
# Same Kconfig-equivalent defines as build.sh's default (o2) variant, so the
# included quickjs.c has the same struct layout as the linked objects.
DEFS="-DQUICKJS_NG_BUILD -D_GNU_SOURCE -DCONFIG_POCKET_VM_SEGFRAMES=1 -DCONFIG_POCKET_VM_FLATCALLS=1 -DCONFIG_POCKET_VM_LAZY_INPUTS=1 -DCONFIG_POCKET_VM_YIELD=1 -DCONFIG_POCKET_VM_STRIP_FN_SOURCE=1"
gcc -std=gnu11 -O2 -w $F $DEFS -I "$OUT/include" -I components/pocketjs_guest/include -I $QJS \
  "$SP/lazyfloor.c" components/pocketjs_guest/src/vm_sched.c components/pocketjs_guest/src/vm_clock.c \
  $obj/libregexp.o $obj/libunicode.o $obj/dtoa.o $obj/quickjs-vm.o -lm -lpthread -ldl -o "$OUT/lazyfloor"
"$OUT/lazyfloor"
