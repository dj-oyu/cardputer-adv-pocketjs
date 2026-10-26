#!/bin/bash
# Regenerate components/quickjs-ng/quickjs-ng/quickjs-rom-intrinsics.h, the
# hardened intrinsics as const data (docs/vm/rom-intrinsics.md R2). WSL:
#   wsl -e bash tools/vmtest/floor/gen_rom_intrinsics.sh          # write it
#   wsl -e bash tools/vmtest/floor/gen_rom_intrinsics.sh --check  # fail if stale
# The generator includes quickjs.c built as the firmware has it (F1-F3 on,
# no ROM intrinsics: it has to build the graph the eager way), plus the
# host-only resolve step. Linked without PIE: it names C functions by
# reading its own symbol table.
set -euo pipefail
cd "$(dirname "$0")/../../.."
QJS=components/quickjs-ng/quickjs-ng
OUT=.cache/vmtest-floor/genri
mkdir -p "$OUT/include"
[ -f "$OUT/include/sdkconfig.h" ] || echo "/* host stub */" > "$OUT/include/sdkconfig.h"
DEFS="-DQUICKJS_NG_BUILD -D_GNU_SOURCE -DQJS_DISABLE_DUMPS -DPOCKET_VM_IMMORTAL_TEST -DPOCKET_VM_GEN_ROM_INTRINSICS"
INC="-I $OUT/include -I components/pocketjs_guest/include -I $QJS"
for f in dtoa libregexp libunicode quickjs-vm; do
  gcc -std=gnu11 -c -O1 -g -w $DEFS $INC $QJS/$f.c -o "$OUT/$f.o"
done
gcc -std=gnu11 -O1 -g -w -no-pie -fno-pie $DEFS $INC \
  tools/vmtest/floor/gen_rom_intrinsics.c components/pocketjs_guest/src/vm_sched.c components/pocketjs_guest/src/vm_clock.c \
  "$OUT/dtoa.o" "$OUT/libregexp.o" "$OUT/libunicode.o" "$OUT/quickjs-vm.o" -lm -lpthread -ldl -o "$OUT/gen_rom_intrinsics"
nm --defined-only "$OUT/gen_rom_intrinsics" > "$OUT/syms.txt"
if [ "${1:-}" = "--check" ]; then
  "$OUT/gen_rom_intrinsics" "$OUT/quickjs-rom-intrinsics.h" "$OUT/syms.txt"
  diff -q "$OUT/quickjs-rom-intrinsics.h" $QJS/quickjs-rom-intrinsics.h
  diff -q "$OUT/quickjs-rom-intrinsics-defs.h" $QJS/quickjs-rom-intrinsics-defs.h
  echo "ROM_INTRINSICS_UP_TO_DATE"
elif [ "${1:-}" = "--stats" ]; then
  "$OUT/gen_rom_intrinsics"
else
  "$OUT/gen_rom_intrinsics" $QJS/quickjs-rom-intrinsics.h "$OUT/syms.txt"
fi
