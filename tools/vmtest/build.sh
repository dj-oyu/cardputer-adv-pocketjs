#!/usr/bin/env bash
# Builds the host runner vmrun from the vendored quickjs-ng (WSL only).
#
#   tools/vmtest/build.sh           # asan (default)
#   tools/vmtest/build.sh o2        # -O2, for timing
#   tools/vmtest/build.sh all
#   tools/vmtest/build.sh asan-alloca   # CONFIG_POCKET_VM_SEGFRAMES off: frames on the C stack
#   tools/vmtest/build.sh o2-alloca     # (the pre-L2a path; spec sec.12 "revertible")
#   tools/vmtest/build.sh all-alloca
#   tools/vmtest/build.sh asan-recur    # segframes, JS calls still recurse in C (L2a; FLATCALLS off)
#   tools/vmtest/build.sh asan-flat     # segframes + CONFIG_POCKET_VM_FLATCALLS (L2b)
#   tools/vmtest/build.sh all-recur / all-flat
#
# Three paths (spec sec.12 / design H5): "-alloca" is the same compiler flags
# without the L2a define; "-recur" and "-flat" pin the L2b switch off / on.
# The PLAIN variants (asan / o2) build what main/Kconfig.projbuild ships by
# default -- see the two defaults below, which must be kept equal to the
# Kconfig -- so that every gate run without a suffix is a gate on the
# firmware's path. run.sh --variant / stack_probe.sh N VARIANT / test262.py
# --variant / budget_probe.sh VARIANT accept any of the six names.
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
  # CONFIG_POCKET_VM_SEGFRAMES defaults to y in main/Kconfig.projbuild. The
  # host has no sdkconfig (the stub below is empty), so a default-y switch
  # has to be passed by hand or the host would silently test the OTHER path
  # from the one the firmware ships. Passed as -D, not written into the stub:
  # the stub is shared by every variant and the -alloca ones must not see it.
  local segframes="-DCONFIG_POCKET_VM_SEGFRAMES=1"
  # CONFIG_POCKET_VM_FLATCALLS: default n in main/Kconfig.projbuild (L2b,
  # docs/vm-L2-design.md sec.10). Same rule as above: the plain variant
  # mirrors the Kconfig default; "-flat" / "-recur" force it on / off.
  local flatcalls=""
  local base=${variant%-alloca}; base=${base%-recur}; base=${base%-flat}
  case "$variant" in
    *-alloca) segframes="" ;;
    *-recur) flatcalls="" ;;
    *-flat) flatcalls="-DCONFIG_POCKET_VM_FLATCALLS=1" ;;
  esac
  case "$base" in
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
  # GUEST: from L1 on the harness LINKS the firmware's scheduler
  # (components/pocketjs_guest/src/vm_sched.c, vm_clock.c) instead of copying
  # it, so a change to the drain cannot pass here and fail there. Those two
  # files deliberately include no esp headers; nothing else from that component
  # is host-compilable.
  local GUEST=components/pocketjs_guest
  local defs="-DQUICKJS_NG_BUILD -D_GNU_SOURCE $segframes $flatcalls -I $OUT/include -I $GUEST/include"
  local objs=()
  # quickjs-vm: the L2 harness hooks (forced yield at opcode safepoints, G5
  # gap recorder) that vmrun reaches through its weak symbols. Not upstream,
  # so it is a separate object rather than a change inside quickjs.c.
  for f in dtoa libregexp libunicode quickjs quickjs-libc quickjs-vm; do
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
      tools/vmtest/vmrun.c "$GUEST/src/vm_sched.c" "$GUEST/src/vm_clock.c" \
      "${objs[@]}" -lm -lpthread -ldl -o "$OUT/vmrun-$variant"
  echo "built $OUT/vmrun-$variant"
}

case "${1:-asan}" in
  all) build_variant asan; build_variant o2 ;;
  all-alloca) build_variant asan-alloca; build_variant o2-alloca ;;
  all-recur) build_variant asan-recur; build_variant o2-recur ;;
  all-flat) build_variant asan-flat; build_variant o2-flat ;;
  *) build_variant "${1:-asan}" ;;
esac
