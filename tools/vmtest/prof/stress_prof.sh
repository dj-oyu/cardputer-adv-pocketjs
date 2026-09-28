#!/bin/bash
# Instruction profile of apps/stress (tools/test_stress_app.c's 900 frames)
# under callgrind: the device layout (-m32 -malign-double, tools/vmtest/
# m32_sysroot.sh), -O2, and the firmware's VM defaults. Instruction counts
# stand in for time on the in-order Xtensa core as a ratio between parts of
# one run, never as milliseconds. valgrind need not be installed: it is used
# from an extracted .deb (VG, default ~/vg/root; see tools/vmtest/prof/README.md).
#   wsl -e bash tools/vmtest/prof/stress_prof.sh [OUT_PREFIX]
set -euo pipefail
cd "$(dirname "$0")/../../.."
QJS=components/quickjs-ng/quickjs-ng
OUT=${1:-/tmp/stress-prof}
CACHE=${CACHE:-/tmp/qjs-stress-prof}
VG=${VG:-$HOME/vg/root}
F=$(bash tools/vmtest/m32_sysroot.sh)
mkdir -p "$CACHE"
python3 tools/make_font.py "$CACHE" >/dev/null
python3 - "$CACHE/kasane_pet_test_data.h" <<'PY'
import pathlib, sys
data = pathlib.Path('apps/pet/assets/pets-compact.bin').read_bytes()
pathlib.Path(sys.argv[1]).write_text('static const uint8_t pet_test_data[] = {' + ','.join(map(str, data)) + '};\n')
PY
DEFS="-DQUICKJS_NG_BUILD -D_GNU_SOURCE ${PROF_DEFS:-}"
for f in dtoa libregexp libunicode quickjs quickjs-vm; do
  gcc -std=gnu11 -c -O2 -g -w $F $DEFS -I "$QJS" -I components/pocketjs_guest/include \
      "$QJS/$f.c" -o "$CACHE/$f.o"
done
gcc -std=gnu11 -O2 -g -w $F -fno-omit-frame-pointer \
  -I "$QJS" -I tools/hostshim -I main -I main/pocket -I main/ui -I main/ui/kasane \
  -I main/text -I main/hal -I "$CACHE" \
  tools/test_stress_app.c tools/hostshim/pocket_api_stub.c \
  main/text/ksn_font.c tools/hostshim/jpfont.c \
  main/pet/ksn_pet.c main/pet/pet_pixels.c tools/hostshim/ksn_pet_builtin.c \
  main/pocket/pocket_clock_source.c main/pocket/pocket_clock.c main/pocket/pocket_av_playback_source.c \
  main/pocket/pocket_av_output_source.c main/pocket/app_view_assets.c main/pocket/app_view_provider.c \
  main/pocket/app_music_view.c main/pocket/app_legacy_presenter.c main/pocket/pocket_kasane.c main/pocket/pocket_proc.c \
  main/ui/kasane/ksn_procedural.c main/ui/kasane/ksn_proc_analysis.c \
  main/ui/kasane/ksn_proc_plan.c main/ui/kasane/ksn_proc_plan_points.c \
  main/ui/kasane/ksn_proc_points.c main/ui/kasane/ksn_proc_points_pie.c \
  main/ui/kasane/ksn_proc_points_dispatch.c \
  main/pocket/pocket_memory.c \
  main/ui/kasane/ksn_runtime.c main/ui/kasane/ksn_schema.c main/ui/kasane/ksn_schema_session.c \
  main/ui/kasane/ksn_source.c main/ui/kasane/ksn_source_pool.c main/ui/kasane/ksn_source_pool_adapter.c \
  main/ui/kasane/ksn_core.c main/ui/kasane/ksn_view.c main/ui/kasane/ksn_cache.c main/ui/kasane/ksn_modal.c \
  main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c main/pocket/app_notice.c \
  "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" "$CACHE/quickjs.o" "$CACHE/quickjs-vm.o" \
  -Wl,--wrap=calloc -Wl,--wrap=free -lm -o "$OUT"
VALGRIND_LIB=$VG/usr/libexec/valgrind "$VG/usr/bin/valgrind" --tool=callgrind \
  --callgrind-out-file="$OUT.cg" "$OUT" apps/stress/stress.js 2>&1 | grep -E 'STRESS_HOST|frames 900|Collected'
echo "profile: $OUT.cg"
