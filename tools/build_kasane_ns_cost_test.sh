#!/usr/bin/env bash
# Builds tools/test_kasane_ns_cost.c: the guest heap that the first read of
# pocket.kasane builds (docs/kasane/api-reorg.md sec.6 P0 item 4), with the
# real grid, video and pixel members and kasane.shiki (WSL, from anywhere).
#   bash tools/build_kasane_ns_cost_test.sh && /tmp/test-kasane-ns-cost
#   M32=1 bash tools/build_kasane_ns_cost_test.sh && /tmp/test-kasane-ns-cost-m32
# M32=1: the device's 8 B JSValue and 4 B pointers (tools/vmtest/m32_sysroot.sh),
# the figure to quote; the 64-bit host lays objects out differently.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ -n "${M32:-}" ]; then
  F=$(bash tools/vmtest/m32_sysroot.sh)
  export QJS_CFLAGS="$F" CFLAGS="$F -O1 -g -fsanitize=address,undefined"
  CACHE=${CACHE:-/tmp/qjs-kasane-host-m32}
  OUT=${OUT:-/tmp/test-kasane-ns-cost-m32}
fi
CACHE=${CACHE:-/tmp/qjs-kasane-host}
# The members as tools/kasane_contract/run_megademo_app_host.py links them,
# plus pocket.kasane.video (its SD stream stubbed in the test).
OUT=${OUT:-/tmp/test-kasane-ns-cost} TEST_SOURCE=tools/test_kasane_ns_cost.c CACHE="$CACHE" \
  EXTRA_SOURCES="main/pocket/pocket_shiki_api.c main/pocket/pocket_grid.c main/ui/kasane/ksn_proc_grid.c
    main/ui/kasane/ksn_proc_grid_pie.c main/ui/kasane/ksn_proc_grid_image.c main/ui/kasane/ksn_proc_grid_resize.c
    main/pocket/pocket_pixel.c main/ui/kasane/ksn_pixel_function.c main/ui/kasane/ksn_pixel_span.c
    main/pocket/pocket_video.c main/ui/kasane/ksn_video_frames.c" \
  bash tools/build_kasane_test.sh
