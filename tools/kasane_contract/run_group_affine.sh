#!/usr/bin/env bash
# Candidate 3c (docs/perf/kasane-group-affine.md): the folded group arm against
# the isolated premultiplied tile chain, in one binary.
#
# This is the entry point for tools/kasane_contract/test_group_affine.c alone. It
# deliberately does not touch tools/kasane_contract/run.sh: this container cannot
# finish that script's sanitizer arm (docs/perf/kasane-opt-integration.md 0,
# AddressSanitizer:DEADLYSIGNAL in an endless loop on pristine HEAD too), and the
# suite is run from a /tmp copy of run.sh with only the sanitizer flag stripped.
# Wiring this case into run.sh belongs to a session that can run the sanitizer
# arm, so it is left out here.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
# test_group_affine.c includes ksn_render.c for its statics: do NOT also compile
# ksn_render.c into this binary.
cc -std=c11 -Wall -Wextra -Werror -O2 -fstrict-aliasing -Imain/ui/kasane \
  main/ui/kasane/ksn_core.c main/ui/kasane/ksn_cache.c \
  tools/kasane_contract/test_group_affine.c -o "$out/group-affine" main/ui/kasane/ksn_blend_pie.c
"$out/group-affine"
# The counting arm: -finstrument-functions turns "the fold ran, and the scene
# that must not fold did not" into a number per render.
cc -std=c11 -Wall -Wextra -Werror -O2 -fno-inline -finstrument-functions -DKSN_AFFINE_COUNT \
  -Imain/ui/kasane main/ui/kasane/ksn_core.c main/ui/kasane/ksn_cache.c \
  tools/kasane_contract/test_group_affine.c -o "$out/group-affine-count" main/ui/kasane/ksn_blend_pie.c
"$out/group-affine-count"
