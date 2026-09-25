#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
QJS=components/quickjs-ng/quickjs-ng
OUT=${OUT:-/tmp/test-pocket-kasane}
TEST_SOURCE=${TEST_SOURCE:-tools/test_pocket_kasane.c}
CACHE=${CACHE:-/tmp/qjs-kasane-host}
mkdir -p "$CACHE"
python3 tools/make_font.py "$CACHE"
python3 - "$CACHE/kasane_pet_test_data.h" <<'PY'
import pathlib, sys
data = pathlib.Path('apps/pet/assets/pets-compact.bin').read_bytes()
pathlib.Path(sys.argv[1]).write_text('static const uint8_t pet_test_data[] = {' + ','.join(map(str, data)) + '};\n')
PY
# Match the shipping VM path. Keep this cache separate from harnesses with
# different defines, and invalidate it on header or build-script changes.
DEFS="-DQUICKJS_NG_BUILD -D_GNU_SOURCE -DCONFIG_POCKET_VM_SEGFRAMES=1 -DCONFIG_POCKET_VM_FLATCALLS=1"
for f in dtoa libregexp libunicode quickjs quickjs-vm; do
  if [ ! -f "$CACHE/$f.o" ] || [ "$QJS/$f.c" -nt "$CACHE/$f.o" ] || [ "$0" -nt "$CACHE/$f.o" ] \
     || [ -n "$(find "$QJS" -name '*.h' -newer "$CACHE/$f.o" -print -quit)" ]; then
    gcc -std=gnu11 -c -O1 -g -w $DEFS -I "$QJS" -I components/pocketjs_guest/include \
        "$QJS/$f.c" -o "$CACHE/$f.o"
  fi
done
gcc -std=gnu11 ${CFLAGS:--O1 -g -fsanitize=address,undefined} -Wall -Wextra -Werror \
  -fno-omit-frame-pointer \
  -I "$QJS" -I tools/hostshim -I main -I main/pocket -I main/ui -I main/ui/kasane \
  -I main/text -I main/hal -I "$CACHE" \
  "$TEST_SOURCE" ${EXTRA_SOURCES:-} tools/hostshim/pocket_api_stub.c \
  main/text/ksn_font.c tools/hostshim/jpfont.c \
  main/pet/ksn_pet.c main/pet/pet_pixels.c tools/hostshim/ksn_pet_builtin.c \
  main/pocket/pocket_clock_source.c main/pocket/pocket_clock.c main/pocket/pocket_av_playback_source.c main/pocket/pocket_av_output_source.c main/pocket/app_view_assets.c main/pocket/app_view_provider.c main/pocket/app_music_view.c main/pocket/app_legacy_presenter.c main/pocket/pocket_kasane.c main/ui/kasane/ksn_runtime.c main/ui/kasane/ksn_schema.c main/ui/kasane/ksn_schema_session.c main/ui/kasane/ksn_source.c main/ui/kasane/ksn_source_pool.c main/ui/kasane/ksn_source_pool_adapter.c main/ui/kasane/ksn_core.c main/ui/kasane/ksn_view.c \
  main/ui/kasane/ksn_cache.c main/ui/kasane/ksn_modal.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c main/pocket/app_notice.c \
  "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" "$CACHE/quickjs.o" \
  "$CACHE/quickjs-vm.o" \
  -Wl,--wrap=calloc -Wl,--wrap=free -lm -o "$OUT"
echo "built $OUT"
