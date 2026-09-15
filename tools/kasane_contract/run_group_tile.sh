#!/usr/bin/env bash
# Tile-level work on the group's compositing tile (docs/perf/kasane-tile.md):
# candidate 3a (blocks the group's bbox cannot reach), 3b (a 16-pixel tile) and
# the smooth-layer shortcut. This is the entry point for
# tools/kasane_contract/test_group_tile.c alone.
#
# It deliberately does not touch tools/kasane_contract/run.sh: this container
# cannot finish that script's sanitizer arm (docs/perf/kasane-opt-integration.md
# 0; AddressSanitizer:DEADLYSIGNAL in an endless loop on pristine HEAD too), and
# the suite is run from a /tmp copy of run.sh with only the sanitizer flag
# stripped. Wiring this case into run.sh belongs to a session that can run the
# sanitizer arm, so it is left out here.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
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
