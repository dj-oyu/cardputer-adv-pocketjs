#!/usr/bin/env bash
# m32_sysroot.sh — a user-local i386 sysroot so vmrun and vmalloc_replay can be
# built -m32 on a WSL without gcc-multilib and without root (WSL only).
#
#   bash tools/vmtest/m32_sysroot.sh            # prints the flags on stdout
#   F=$(bash tools/vmtest/m32_sysroot.sh)
#   VMTEST_OUT=$PWD/.cache/vmtest32 VMTEST_CFLAGS="$F" bash tools/vmtest/build.sh o2
#   VMALLOC_OUT=../../.cache/vmalloc32 VMALLOC_CFLAGS="$F" VMALLOC_TLSF_ALIGN_LOG2=2 bash tools/vmalloc/build.sh
#
# Why: the 64-bit host has 16 B JSValues and 8 B pointers, which is not the
# device (docs/vm/vm-ledger/08-slab-study.md sec.9). i386 with quickjs-ng's
# default JS_NAN_BOXING (on whenever INTPTR_MAX < INT64_MAX, as on Xtensa) gives
# the device's 8 B JSValue and 4 B pointer. -malign-double makes double and
# int64 fields 8-aligned inside structs, as the Xtensa ABI does and the i386
# ABI does not; without it every struct holding a JSValue is laid out
# differently from the firmware's.
#
# `apt-get download` needs no root. The packages are unpacked, the libc.so
# linker script's absolute paths are pointed into the sysroot, and binaries
# are linked with that sysroot's ld-linux.so.2 and a DT_RPATH (not RUNPATH:
# libasan.so needs to find libm through the executable's path too).
set -euo pipefail
S=${M32_SYSROOT:-/tmp/m32sys}
DEBS=${M32_DEBS:-$(cd "$(dirname "$0")/../.." && pwd)/.cache/m32/debs}
GCCV=$(gcc -dumpversion | cut -d. -f1)
if [ ! -f "$S/.ok" ]; then
  mkdir -p "$DEBS"
  (cd "$DEBS" && apt-get download libc6-dev-i386 libc6-i386 "lib32gcc-$GCCV-dev" lib32gcc-s1 lib32asan8 lib32ubsan1 lib32stdc++6 >&2)
  rm -rf "$S"; mkdir -p "$S"
  for d in "$DEBS"/*.deb; do dpkg-deb -x "$d" "$S"; done
  for f in $(grep -l "GNU ld script" "$S"/usr/lib32/*.so); do
    sed -i "s@ /lib32/@ $S/usr/lib32/@g; s@ /usr/lib32/@ $S/usr/lib32/@g; s@ /lib/ld-linux.so.2@ $S/usr/lib32/ld-linux.so.2@g" "$f"
  done
  touch "$S/.ok"
fi
echo "-m32 -malign-double -B$S/usr/lib32 -B$S/usr/lib/gcc/x86_64-linux-gnu/$GCCV/32 -L$S/usr/lib32 -L$S/usr/lib/gcc/x86_64-linux-gnu/$GCCV/32 -isystem $S/usr/include/x86_64-linux-gnu -isystem /usr/include/x86_64-linux-gnu -Wl,--dynamic-linker=$S/usr/lib32/ld-linux.so.2 -Wl,-rpath,$S/usr/lib32 -Wl,--disable-new-dtags"
