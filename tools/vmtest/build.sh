#!/usr/bin/env bash
# Builds the host runner vmrun from the vendored quickjs-ng (WSL only).
#
#   tools/vmtest/build.sh           # asan (default)
#   tools/vmtest/build.sh o2        # -O2, for timing
#   tools/vmtest/build.sh all
#   tools/vmtest/build.sh asan-reloc  # + CONFIG_POCKET_VM_RELOC (L3a), for --force-reloc
#
# The engine has one path: the VM levels (L1 scheduling, L2 segment frames,
# flat calls and yield, stripped function source) and the F/R lines (flash
# atoms, lazy builtins and intrinsics, the small-block cache) are no longer
# build options (docs/vm/backlog.md, 2026-09-27), so the host builds exactly
# what the firmware ships. The variants that pinned one of them off (-alloca,
# -recur, -noyield, -keepsrc, -norom, ...) went with them; commit 5db834f is
# the last tree that has them.
#
# Unlike tools/build_pocket_text_test.sh, the asan variant instruments QuickJS
# itself: the code under test from L1 on IS quickjs.c, so a use-after-free in a
# modified call path has to be reported, not just one in the driver. Objects
# are cached per variant and rebuilt when the vendored source is newer.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
# VMTEST_QJS: build a copy of the engine instead (the negative controls,
# tools/vmtest/floor/f1_faults.sh, build deliberately broken copies this way;
# pair it with its own VMTEST_OUT so no object is shared with the real one).
QJS=${VMTEST_QJS:-components/quickjs-ng/quickjs-ng}
OUT=${VMTEST_OUT:-$ROOT/.cache/vmtest}

build_variant() {
  local variant=$1 cflags
  # L3a: the explicit move API. Default n in the Kconfig and nothing in the
  # firmware calls it, so it is never in a plain variant -- this suffix is
  # the only way it is built, and --force-reloc is the only thing that calls it.
  local reloc=""
  local base=${variant%-reloc}
  [ "$base" != "$variant" ] && reloc="-DCONFIG_POCKET_VM_RELOC=1"
  case "$base" in
    asan) cflags="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined" ;;
    o2)   cflags="-O2 -g" ;;
    *) echo "unknown variant $variant (asan, o2, either with -reloc)" >&2; exit 2 ;;
  esac
  # VMTEST_CFLAGS: extra flags for every compile and the link, e.g. the -m32
  # sysroot flags from tools/vmtest/m32_sysroot.sh (with VMTEST_OUT pointing at
  # a separate cache so 32- and 64-bit objects never mix).
  cflags="$cflags ${VMTEST_CFLAGS:-}"
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
  local defs="-DQUICKJS_NG_BUILD -D_GNU_SOURCE $reloc -I $OUT/include -I $GUEST/include"
  local objs=() compile_pids=()
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
      compile_pids+=("$!")
    fi
    objs+=("$obj/$f.o")
  done
  # Bare wait returns success even if a compiler failed; never link stale objects.
  local compile_failed=0 pid
  for pid in "${compile_pids[@]}"; do
    wait "$pid" || compile_failed=1
  done
  if ((compile_failed)); then
    echo "compile failed [$variant]; link skipped" >&2
    return 1
  fi
  gcc -std=gnu11 $cflags -Wall -Wextra -Werror $defs -I "$QJS" \
      tools/vmtest/vmrun.c "$GUEST/src/vm_sched.c" "$GUEST/src/vm_clock.c" "$GUEST/src/block_cache.c" \
      "${objs[@]}" -lm -lpthread -ldl -o "$OUT/vmrun-$variant"
  echo "built $OUT/vmrun-$variant"
}

case "${1:-asan}" in
  all) build_variant asan; build_variant o2 ;;
  *) build_variant "${1:-asan}" ;;
esac
