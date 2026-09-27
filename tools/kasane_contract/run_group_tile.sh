#!/usr/bin/env bash
# Tile-level work on the group's compositing tile (docs/perf/kasane-tile.md):
# candidate 3a (blocks the group's bbox cannot reach), 3b (a 16-pixel tile) and
# the smooth-layer shortcut. This is the entry point for
# tools/kasane_contract/test_group_tile.c alone.
#
# Keep this large sweep as a separate entry point, but test both sanitizer and
# optimized builds. Negative gradient spans must not reach a signed left shift.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -Imain/ui/kasane \
  main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c \
  tools/kasane_contract/test_group_tile.c -o "$out/group-tile-sanitized"
UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=halt_on_error=1 "$out/group-tile-sanitized"
# test_group_tile.c links ksn_render.c as a translation unit (it only needs the
# three switches, which ksn_render.h declares): do NOT also include it.
cc -std=c11 -Wall -Wextra -Werror -O2 -fstrict-aliasing -Imain/ui/kasane \
  main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c \
  tools/kasane_contract/test_group_tile.c -o "$out/group-tile"
"$out/group-tile"
# The counting arm: the tile's waste rate and how often each arm is entered.
# A shortcut that is never taken passes a two-arm comparison by comparing the
# old path with itself, so this arm is what makes the other one mean something.
cc -std=c11 -Wall -Wextra -Werror -O2 -fstrict-aliasing -DKSN_TILE_COUNT \
  -Imain/ui/kasane main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c \
  tools/kasane_contract/test_group_tile.c -o "$out/group-tile-count"
"$out/group-tile-count"
