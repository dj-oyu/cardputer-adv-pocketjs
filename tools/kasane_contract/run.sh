#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -DKSN_CONTRACT_HOST -Imain/ui/kasane \
  tools/kasane_contract/use_cases.c tools/kasane_contract/probe.c -o "$out/probe"
"$out/probe"
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -Imain/ui/kasane -Itools/kasane_contract main/ui/kasane/ksn_core.c \
  tools/kasane_contract/use_cases.c tools/kasane_contract/test_core.c -o "$out/core"
"$out/core"
for options in '-g -fsanitize=address,undefined' '-O2 -fstrict-aliasing'; do
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_cache.c \
    main/ui/kasane/ksn_modal.c main/ui/kasane/ksn_view.c tools/kasane_contract/test_runtime.c -o "$out/runtime"
  "$out/runtime"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_cache.c \
    main/ui/kasane/ksn_modal.c main/ui/kasane/ksn_view.c tools/kasane_contract/test_app_teardown.c -o "$out/teardown"
  "$out/teardown"
  for fill_mode in '' '-DKSN_PIE_FILL_MODEL'; do
    cc -std=c11 -Wall -Wextra -Werror $options $fill_mode -Imain/ui/kasane \
      main/ui/kasane/ksn_core.c tools/kasane_contract/test_fill.c -o "$out/fill"
    "$out/fill"
  done
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_cache.c \
    main/ui/kasane/ksn_modal.c main/ui/kasane/ksn_view.c tools/kasane_contract/test_view.c -o "$out/view"
  "$out/view"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_cache.c \
    main/ui/kasane/ksn_modal.c main/ui/kasane/ksn_view.c tools/kasane_contract/test_repair.c -o "$out/repair"
  "$out/repair"
  cc -std=c11 -Wall -Wextra -Werror $options -DKSN_FROST_PIE_MODEL -Imain/ui/kasane \
    main/ui/kasane/ksn_frost.c tools/kasane_contract/frost_baseline.c tools/kasane_contract/test_frost_equivalence.c -o "$out/vector"
  "$out/vector"
  cc -std=c11 -Wall -Wextra -Werror $options \
    -Imain/ui/kasane main/ui/kasane/ksn_core.c tools/kasane_contract/test_review.c -o "$out/review"
  "$out/review"
  cc -std=c11 -Wall -Wextra -Werror $options \
    -Imain/ui/kasane tools/kasane_contract/test_exhaustion.c -o "$out/exhaustion"
  "$out/exhaustion"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c tools/kasane_contract/test_resources.c -o "$out/resources"
  "$out/resources"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c tools/kasane_contract/test_render.c -o "$out/render"
  "$out/render"
  # Boundary 7: the render path's phase counts, and the pixel identity of the
  # two arms inside one binary -- the switch off must not move a pixel, and the
  # counts must not move with it off.
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c tools/kasane_contract/test_render_prof.c -o "$out/render-prof"
  "$out/render-prof"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c tools/kasane_contract/test_primitives.c -o "$out/primitives"
  "$out/primitives"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c tools/kasane_contract/test_text_render.c -o "$out/text"
  "$out/text"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c tools/kasane_contract/test_image_render.c -lm -o "$out/image"
  "$out/image"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c tools/kasane_contract/test_animation.c -o "$out/animation"
  "$out/animation"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane -Imain/pet \
    main/pet/ksn_pet.c main/pet/pet_pixels.c tools/kasane_contract/test_pet_provider.c -o "$out/pet-provider"
  "$out/pet-provider"
  python3 tools/make_font.py "$out"
  cc -std=c11 -Wall -Wextra -Werror $options -Itools/kasane_contract/fontshim \
    -Itools/hostshim -Imain/hal -Imain/text -Imain/ui/kasane -I"$out" \
    main/text/ksn_font.c tools/kasane_contract/test_font.c -o "$out/font"
  "$out/font"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c tools/kasane_contract/test_group_dither.c -o "$out/group-dither"
  "$out/group-dither"
  # One script, one binary, three arms of g_ksn_decode_once; ksn_core_read is
  # wrapped so the read counts are counted call sites, not estimates.
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c \
    tools/kasane_contract/test_decode_reuse.c -Wl,--wrap=ksn_core_read -o "$out/decode-reuse"
  "$out/decode-reuse"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_cache.c \
    tools/kasane_contract/test_cache.c -o "$out/cache"
  "$out/cache"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c tools/kasane_contract/test_cache_exhaustion.c \
    -o "$out/cache-exhaustion"
  "$out/cache-exhaustion"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_cache.c \
    tools/kasane_contract/test_coverage_runs.c -o "$out/coverage-runs"
  "$out/coverage-runs"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_cache.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_modal.c \
    tools/kasane_contract/test_composition.c -o "$out/composition"
  "$out/composition"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_frost.c tools/kasane_contract/frost_baseline.c tools/kasane_contract/test_frost_equivalence.c -o "$out/equivalence"
  "$out/equivalence"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_frost.c tools/kasane_contract/test_frost.c -o "$out/frost"
  "$out/frost" > "$out/frost.bin"
  python3 tools/kasane_contract/frost_reference.py "$out/frost.bin"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_frost.c tools/kasane_contract/test_stress.c -o "$out/stress"
  "$out/stress" > "$out/stress.bin"
  python3 tools/kasane_contract/stress_reference.py "$out/stress.bin"
done
python3 tools/pie/test_frost.py
python3 tools/kasane_contract/fill_pie.py
# Measured, not estimated: -finstrument-functions counts entries into the pixel
# predicate and into the row solver, so this arm prints the before/after call
# counts per frame (see the reporter in test_coverage_runs.c).
cc -std=c11 -Wall -Wextra -Werror -O2 -fno-inline -finstrument-functions -DKSN_COUNT_CALLS \
  -Imain/ui/kasane main/ui/kasane/ksn_core.c main/ui/kasane/ksn_cache.c \
  tools/kasane_contract/test_coverage_runs.c -o "$out/coverage-count"
"$out/coverage-count"
printf '#include "ksn_api.h"\n#include "ksn_ports.h"\n#include "ksn_core.h"\n#include "ksn_cache.h"\n#include "ksn_modal.h"\n#include "ksn_view_host.h"\n#include "ksn_runtime.h"\n' | \
  c++ -std=c++17 -Wall -Wextra -Werror -Imain/ui/kasane -x c++ -fsyntax-only -
