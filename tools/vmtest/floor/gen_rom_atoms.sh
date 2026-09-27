#!/bin/bash
# Regenerate components/quickjs-ng/quickjs-ng/quickjs-rom-atoms{,-defs}.h from
# this tree's JS_NewContext (docs/vm/builtin-floor-plan.md sec.5.1). WSL:
#   wsl -e bash tools/vmtest/floor/gen_rom_atoms.sh          # write the headers
#   wsl -e bash tools/vmtest/floor/gen_rom_atoms.sh --check  # fail if they are stale
# The generator is built with POCKET_VM_GEN_ROM_ATOMS, which turns F1-F3 off
# in quickjs.c (the only build that does): the table has to
# describe the engine as it is before names move to flash.
set -euo pipefail
cd "$(dirname "$0")/../../.."
QJS=components/quickjs-ng/quickjs-ng
OUT=.cache/vmtest-floor/gen
mkdir -p "$OUT/include"
[ -f "$OUT/include/sdkconfig.h" ] || echo "/* host stub */" > "$OUT/include/sdkconfig.h"
DEFS="-DQUICKJS_NG_BUILD -D_GNU_SOURCE -DPOCKET_VM_GEN_ROM_ATOMS"
for f in dtoa libregexp libunicode quickjs-vm; do
  gcc -std=gnu11 -c -O1 -w $DEFS -I "$OUT/include" -I components/pocketjs_guest/include -I $QJS $QJS/$f.c -o "$OUT/$f.o"
done
gcc -std=gnu11 -O1 -w $DEFS -I "$OUT/include" -I components/pocketjs_guest/include -I $QJS \
  tools/vmtest/floor/gen_rom_atoms.c components/pocketjs_guest/src/vm_sched.c components/pocketjs_guest/src/vm_clock.c \
  "$OUT/dtoa.o" "$OUT/libregexp.o" "$OUT/libunicode.o" "$OUT/quickjs-vm.o" -lm -lpthread -ldl -o "$OUT/gen_rom_atoms"
if [ "${1:-}" = "--check" ]; then
  "$OUT/gen_rom_atoms" "$OUT/quickjs-rom-atoms.h"
  diff -q "$OUT/quickjs-rom-atoms.h" $QJS/quickjs-rom-atoms.h
  diff -q "$OUT/quickjs-rom-atoms-defs.h" $QJS/quickjs-rom-atoms-defs.h
  echo "ROM_ATOMS_UP_TO_DATE"
else
  "$OUT/gen_rom_atoms" $QJS/quickjs-rom-atoms.h
fi
