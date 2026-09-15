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
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c tools/kasane_contract/test_primitives.c -o "$out/primitives"
  "$out/primitives"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c tools/kasane_contract/test_group_dither.c -o "$out/group-dither"
  "$out/group-dither"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_cache.c \
    tools/kasane_contract/test_cache.c -o "$out/cache"
  "$out/cache"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c tools/kasane_contract/test_cache_exhaustion.c \
    -o "$out/cache-exhaustion"
  "$out/cache-exhaustion"
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
printf '#include "ksn_api.h"\n#include "ksn_ports.h"\n#include "ksn_core.h"\n#include "ksn_cache.h"\n#include "ksn_modal.h"\n#include "ksn_view_host.h"\n#include "ksn_runtime.h"\n' | \
  c++ -std=c++17 -Wall -Wextra -Werror -Imain/ui/kasane -x c++ -fsyntax-only -
