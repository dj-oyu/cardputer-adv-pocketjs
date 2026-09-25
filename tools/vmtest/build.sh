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
#   tools/vmtest/build.sh asan-yield   # flat calls + the L2c body (the default since 2026-09-23)
#   tools/vmtest/build.sh asan-noyield # the pre-L2c path: no suspend, no frame guard
#   tools/vmtest/build.sh all-yield
#   tools/vmtest/build.sh o2-keepsrc   # any variant + "-keepsrc": function source text kept (upstream toString)
#
# Three paths (spec sec.12 / design H5): "-alloca" is the same compiler flags
# without the L2a define; "-recur" and "-flat" pin the L2b switch off / on.
# The PLAIN variants (asan / o2) build what main/Kconfig.projbuild ships by
# default -- see the three defaults below, which must be kept equal to the
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
# VMTEST_QJS: build a copy of the engine instead (the negative controls,
# tools/vmtest/floor/f1_faults.sh, build deliberately broken copies this way;
# pair it with its own VMTEST_OUT so no object is shared with the real one).
QJS=${VMTEST_QJS:-components/quickjs-ng/quickjs-ng}
OUT=${VMTEST_OUT:-$ROOT/.cache/vmtest}

build_variant() {
  local variant=$1 cflags
  # CONFIG_POCKET_VM_SEGFRAMES defaults to y in main/Kconfig.projbuild. The
  # host has no sdkconfig (the stub below is empty), so a default-y switch
  # has to be passed by hand or the host would silently test the OTHER path
  # from the one the firmware ships. Passed as -D, not written into the stub:
  # the stub is shared by every variant and the -alloca ones must not see it.
  local segframes="-DCONFIG_POCKET_VM_SEGFRAMES=1"
  # CONFIG_POCKET_VM_FLATCALLS: default y in main/Kconfig.projbuild (L2b,
  # docs/vm/vm-L2-design.md sec.9). Same rule as above: the plain variant
  # mirrors the Kconfig default; "-flat" / "-recur" force it on / off.
  local flatcalls="-DCONFIG_POCKET_VM_FLATCALLS=1"
  # Match the validated firmware default; -eager retains the old return path.
  local lazy="-DCONFIG_POCKET_VM_LAZY_INPUTS=1"
  # CONFIG_POCKET_VM_YIELD: default y in main/Kconfig.projbuild since
  # 2026-09-23. Same rule as the two above -- the plain variant is the
  # firmware's path -- so "-noyield" is what builds the pre-L2c one, and the
  # variants that drop FLATCALLS have to drop this with it (the Kconfig makes
  # it depend on FLATCALLS, and the two cannot be mixed here either).
  local yield="-DCONFIG_POCKET_VM_YIELD=1"
  # CONFIG_POCKET_VM_STRIP_FN_SOURCE: default y (no function source text kept,
  # Function.prototype.toString prints the name-only fallback). A trailing
  # "-keepsrc" on ANY variant builds the upstream behaviour instead; it is
  # peeled off first so the suffix rules below see the rest unchanged.
  local strip="-DCONFIG_POCKET_VM_STRIP_FN_SOURCE=1"
  if [[ $variant == *-keepsrc ]]; then strip=""; fi
  # CONFIG_POCKET_VM_ROM_ATOMS (F1, docs/vm/builtin-floor-plan.md): builtin
  # names in flash, default y since 2026-09-25, so the plain variant has it
  # like the three above. "-norom" anywhere in the name builds the heap-atom
  # path instead; "-rom" is still accepted (it is now the default) so the F1
  # gate's names keep working. Both are removed before the rules below.
  local rom="-DCONFIG_POCKET_VM_ROM_ATOMS=1"
  if [[ $variant == *-norom* ]]; then rom=""; fi
  # CONFIG_POCKET_VM_LAZY_BUILTINS (F2): builtin function lists stay in
  # flash until a name is touched. Default n while F2 is being built; "-lb"
  # anywhere in the name turns it on.
  local lazyb=""
  if [[ $variant == *-lb* ]]; then lazyb="-DCONFIG_POCKET_VM_LAZY_BUILTINS=1"; fi
  local core=${variant%-keepsrc}
  core=${core//-norom/}
  core=${core//-rom/}
  core=${core//-lb/}
  local base=${core%-alloca}; base=${base%-recur}; base=${base%-flat}; base=${base%-yield}; base=${base%-tco}; base=${base%-callbench}; base=${base%-lazy}; base=${base%-eager}; base=${base%-noyield}; base=${base%-reloc}
  case "$core" in
    *-lazy-flat) ;;
    *-eager) lazy="" ;;
    *-callbench) yield="-DCONFIG_POCKET_VM_CALLBENCH=1" ;;
    *-lazy) yield="-DCONFIG_POCKET_VM_YIELD=1 -DCONFIG_POCKET_VM_TCO=1" ;;
    *-tco) yield="-DCONFIG_POCKET_VM_YIELD=1 -DCONFIG_POCKET_VM_TCO=1" ;;
    *-alloca) segframes=""; flatcalls=""; lazy=""; yield="" ;;
    *-recur) flatcalls=""; lazy=""; yield="" ;;
    *-noyield) yield="" ;;
    # L3a: the explicit move API. Default n in the Kconfig and nothing in the
    # firmware calls it, so it is never in a plain variant -- this suffix is
    # the only way it is built, and --force-reloc is the only thing that
    # calls it.
    *-reloc) yield="-DCONFIG_POCKET_VM_YIELD=1 -DCONFIG_POCKET_VM_RELOC=1" ;;
    *-flat) flatcalls="-DCONFIG_POCKET_VM_FLATCALLS=1" ;;
    *-yield) flatcalls="-DCONFIG_POCKET_VM_FLATCALLS=1"; yield="-DCONFIG_POCKET_VM_YIELD=1" ;;
  esac
  case "$base" in
    asan) cflags="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined" ;;
    o2)   cflags="-O2 -g" ;;
    *) echo "unknown variant $variant" >&2; exit 2 ;;
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
  local defs="-DQUICKJS_NG_BUILD -D_GNU_SOURCE $segframes $flatcalls $lazy $yield $strip $rom $lazyb -I $OUT/include -I $GUEST/include"
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
      tools/vmtest/vmrun.c "$GUEST/src/vm_sched.c" "$GUEST/src/vm_clock.c" \
      "${objs[@]}" -lm -lpthread -ldl -o "$OUT/vmrun-$variant"
  echo "built $OUT/vmrun-$variant"
}

case "${1:-asan}" in
  all) build_variant asan; build_variant o2 ;;
  all-alloca) build_variant asan-alloca; build_variant o2-alloca ;;
  all-recur) build_variant asan-recur; build_variant o2-recur ;;
  all-flat) build_variant asan-flat; build_variant o2-flat ;;
  all-yield) build_variant asan-yield; build_variant o2-yield ;;
  *) build_variant "${1:-asan}" ;;
esac
