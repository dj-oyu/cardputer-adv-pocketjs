#!/bin/bash
# Miss counting on the shipped apps (docs/vm/builtin-floor-plan.md §7): builds
# the kasane host harness (same file list as tools/build_lessons_test.sh) around
# a counting copy of quickjs.c written by lazyprobe_patch.py. Host 64-bit build:
# the counts are lookups, not bytes, so the layout does not matter here.
# Objects are cached in .cache/vmtest-floor. WSL only.
#
#   wsl -e bash tools/vmtest/floor/lazyprobe.sh
set -e
cd "$(dirname "$0")/../../.."
SP=tools/vmtest/floor
QJS=components/quickjs-ng/quickjs-ng
CACHE=$PWD/.cache/vmtest-floor; mkdir -p "$CACHE"
python3 "$SP/lazyprobe_patch.py" $QJS/quickjs.c "$CACHE/quickjs.c"
python3 tools/make_font.py "$CACHE" >/dev/null
python3 - "$CACHE/kasane_pet_test_data.h" <<'PY'
import pathlib, sys
data = pathlib.Path('apps/pet/assets/pets-compact.bin').read_bytes()
pathlib.Path(sys.argv[1]).write_text('static const uint8_t pet_test_data[] = {' + ','.join(map(str, data)) + '};\n')
PY
DEFS="-DQUICKJS_NG_BUILD -D_GNU_SOURCE -DCONFIG_POCKET_VM_SEGFRAMES=1 -DCONFIG_POCKET_VM_FLATCALLS=1"
for f in dtoa libregexp libunicode quickjs-vm; do
  [ -f "$CACHE/$f.o" ] || gcc -std=gnu11 -c -O1 -w $DEFS -I $QJS -I components/pocketjs_guest/include $QJS/$f.c -o "$CACHE/$f.o"
done
# The counting copy is always rebuilt: it is what the probe measures.
gcc -std=gnu11 -c -O1 -w $DEFS -I $QJS -I components/pocketjs_guest/include "$CACHE/quickjs.c" -o "$CACHE/quickjs.o"
gcc -std=gnu11 -O1 -g -w \
  -I "$QJS" -I tools/hostshim -I main -I main/pocket -I main/ui -I main/ui/kasane \
  -I main/text -I main/hal -I "$CACHE" \
  "$SP/lazyprobe.c" tools/hostshim/pocket_api_stub.c \
  main/text/ksn_font.c tools/hostshim/jpfont.c \
  main/pet/ksn_pet.c main/pet/pet_pixels.c tools/hostshim/ksn_pet_builtin.c \
  main/pocket/pocket_clock_source.c main/pocket/pocket_clock.c main/pocket/pocket_av_playback_source.c main/pocket/pocket_av_output_source.c main/pocket/app_view_assets.c main/pocket/app_view_provider.c main/pocket/app_music_view.c main/pocket/app_legacy_presenter.c main/pocket/pocket_kasane.c main/ui/kasane/ksn_runtime.c main/ui/kasane/ksn_schema.c main/ui/kasane/ksn_schema_session.c main/ui/kasane/ksn_source.c main/ui/kasane/ksn_source_pool.c main/ui/kasane/ksn_source_pool_adapter.c main/ui/kasane/ksn_core.c main/ui/kasane/ksn_view.c \
  main/ui/kasane/ksn_cache.c main/ui/kasane/ksn_modal.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c main/pocket/app_notice.c \
  "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" "$CACHE/quickjs.o" "$CACHE/quickjs-vm.o" \
  -Wl,--wrap=calloc -Wl,--wrap=free -lm -o "$CACHE/lazyprobe"
"$CACHE/lazyprobe"
