#!/usr/bin/env bash
# Builds the host runner vmrun from the vendored quickjs-ng (WSL only).
#
#   tools/vmtest/build.sh           # asan (default)
#   tools/vmtest/build.sh o2        # -O2, for timing
#   tools/vmtest/build.sh all
#
# Unlike tools/build_pocket_text_test.sh, the asan variant instruments QuickJS
# itself: the code under test from L1 on IS quickjs.c, so a use-after-free in a
# modified call path has to be reported, not just one in the driver. Objects
# are cached per variant and rebuilt when the vendored source is newer.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
QJS=components/quickjs-ng/quickjs-ng
OUT=${VMTEST_OUT:-$ROOT/.cache/vmtest}

build_variant() {
  local variant=$1 cflags
  case "$variant" in
    asan) cflags="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined" ;;
    o2)   cflags="-O2 -g" ;;
    *) echo "unknown variant $variant" >&2; exit 2 ;;
  esac
  local obj=$OUT/obj-$variant
  mkdir -p "$obj" "$OUT/include"
  # quickjs-vmprobe.h includes sdkconfig.h to see CONFIG_POCKET_VM_PROBE. The
  # host has no IDF config; an empty one means every probe is compiled out,
  # which is the shipping default. Written only if absent so objects are not
  # rebuilt for nothing.
  [ -f "$OUT/include/sdkconfig.h" ] || echo "/* vmtest host stub: no CONFIG_* set */" > "$OUT/include/sdkconfig.h"
  # QUICKJS_NG_BUILD/_GNU_SOURCE: the same defines components/quickjs-ng/CMakeLists.txt passes.
  local defs="-DQUICKJS_NG_BUILD -D_GNU_SOURCE -I $OUT/include"
  local objs=()
  for f in dtoa libregexp libunicode quickjs quickjs-libc; do
    # Headers count too: a changed quickjs-*.h (the VM levels add some) must
    # not leave a stale object linked against a new layout.
    if [ ! -f "$obj/$f.o" ] || [ "$QJS/$f.c" -nt "$obj/$f.o" ] || [ "$0" -nt "$obj/$f.o" ] \
       || [ -n "$(find "$QJS" -name '*.h' -newer "$obj/$f.o" -print -quit)" ]; then
      echo "  cc [$variant] $f.c"
      gcc -std=gnu11 -c $cflags -w $defs -I "$QJS" "$QJS/$f.c" -o "$obj/$f.o" &
    fi
    objs+=("$obj/$f.o")
  done
  wait
  gcc -std=gnu11 $cflags -Wall -Wextra -Werror $defs -I "$QJS" \
      tools/vmtest/vmrun.c "${objs[@]}" -lm -lpthread -ldl -o "$OUT/vmrun-$variant"
  echo "built $OUT/vmrun-$variant"
}

case "${1:-asan}" in
  all) build_variant asan; build_variant o2 ;;
  *) build_variant "${1:-asan}" ;;
esac
