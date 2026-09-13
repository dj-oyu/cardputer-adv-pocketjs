#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -DDS_CONTRACT_HOST -Imain/ui/ds \
  tools/ds_contract/use_cases.c tools/ds_contract/probe.c -o "$out/probe"
"$out/probe"
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -Imain/ui/ds -Itools/ds_contract main/ui/ds/ds_core.c \
  tools/ds_contract/use_cases.c tools/ds_contract/test_core.c -o "$out/core"
"$out/core"
for options in '-g -fsanitize=address,undefined' '-O2 -fstrict-aliasing'; do
  cc -std=c11 -Wall -Wextra -Werror $options \
    -Imain/ui/ds main/ui/ds/ds_core.c tools/ds_contract/test_review.c -o "$out/review"
  "$out/review"
  cc -std=c11 -Wall -Wextra -Werror $options \
    -Imain/ui/ds tools/ds_contract/test_exhaustion.c -o "$out/exhaustion"
  "$out/exhaustion"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/ds \
    main/ui/ds/ds_core.c tools/ds_contract/test_resources.c -o "$out/resources"
  "$out/resources"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/ds \
    main/ui/ds/ds_core.c main/ui/ds/ds_render.c tools/ds_contract/test_render.c -o "$out/render"
  "$out/render"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/ds \
    main/ui/ds/ds_core.c main/ui/ds/ds_render.c main/ui/ds/ds_cache.c \
    tools/ds_contract/test_cache.c -o "$out/cache"
  "$out/cache"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/ds \
    main/ui/ds/ds_core.c tools/ds_contract/test_cache_exhaustion.c \
    -o "$out/cache-exhaustion"
  "$out/cache-exhaustion"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/ds \
    main/ui/ds/ds_core.c main/ui/ds/ds_cache.c main/ui/ds/ds_render.c main/ui/ds/ds_modal.c \
    tools/ds_contract/test_composition.c -o "$out/composition"
  "$out/composition"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/ds \
    main/ui/ds/ds_frost.c tools/ds_contract/test_frost.c -o "$out/frost"
  "$out/frost" > "$out/frost.bin"
  python3 tools/ds_contract/frost_reference.py "$out/frost.bin"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/ds \
    main/ui/ds/ds_frost.c tools/ds_contract/test_stress.c -o "$out/stress"
  "$out/stress" > "$out/stress.bin"
  python3 tools/ds_contract/stress_reference.py "$out/stress.bin"
done
printf '#include "ds_api.h"\n#include "ds_ports.h"\n#include "ds_core.h"\n#include "ds_cache.h"\n#include "ds_modal.h"\n' | \
  c++ -std=c++17 -Wall -Wextra -Werror -Imain/ui/ds -x c++ -fsyntax-only -
