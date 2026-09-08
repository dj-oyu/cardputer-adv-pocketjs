#!/usr/bin/env bash
# Build tools/make_opus_asset.c against the pinned libopus and run it.
#
#     wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs && \
#         bash tools/make_opus_asset.sh IN.wav OUT.pok [bitrate]"
#
# WSL, because gcc is not on the Windows side of this machine -- the same reason
# tools/test_sfx.py and tools/build_pocket_text_test.sh say so.
#
# It builds libopus from .cache/codecs/opus-1.6.1, the tree
# tools/prepare_dependencies.py pins and components/opus compiles for the device,
# so the encoder and the decoder come from one revision. The host build is the
# FLOAT one (no -DFIXED_POINT): the encoder runs here, where there is an FPU and
# no deadline, and the bitstream it writes is the same either way.
#
# The object cache is .cache/codecs/opus-1.6.1-host, so the ~100-file build
# happens once.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/.cache/codecs/opus-1.6.1"
OBJ="$ROOT/.cache/codecs/opus-1.6.1-host"
[ -f "$SRC/include/opus.h" ] || {
    echo "libopus is missing. Run: python tools/prepare_dependencies.py" >&2; exit 1; }

if [ ! -f "$OBJ/libopus.a" ]; then
    echo "building libopus $(basename "$SRC") for the host (once)..." >&2
    mkdir -p "$OBJ"
    # Everything but the files that carry their own main() and the arch
    # intrinsics. The float encoder needs silk/float; the fixed one is not built
    # here at all.
    mapfile -t SRCS < <(ls "$SRC"/celt/*.c "$SRC"/silk/*.c "$SRC"/silk/float/*.c \
                           "$SRC"/src/opus.c "$SRC"/src/opus_encoder.c \
                           "$SRC"/src/opus_decoder.c "$SRC"/src/extensions.c \
                           "$SRC"/src/analysis.c "$SRC"/src/mlp.c "$SRC"/src/mlp_data.c \
                           "$SRC"/src/repacketizer.c \
                        | grep -v -e opus_custom_demo -e dump_modes)
    gcc -O2 -w -c -DOPUS_BUILD -DVAR_ARRAYS=1 -DOPUS_VERSION='"host"' \
        -I"$SRC/include" -I"$SRC" -I"$SRC/celt" -I"$SRC/silk" -I"$SRC/silk/float" \
        "${SRCS[@]}"
    mv ./*.o "$OBJ/"
    ar rcs "$OBJ/libopus.a" "$OBJ"/*.o
fi

gcc -O2 -Wall -Wextra -Werror -I"$SRC/include" \
    "$ROOT/tools/make_opus_asset.c" "$OBJ/libopus.a" -lm -o "$OBJ/make_opus_asset"
exec "$OBJ/make_opus_asset" "$@"
