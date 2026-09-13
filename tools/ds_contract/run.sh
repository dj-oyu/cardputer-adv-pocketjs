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
done
printf '#include "ds_api.h"\n#include "ds_ports.h"\n#include "ds_core.h"\n' | \
  c++ -std=c++17 -Wall -Wextra -Werror -Imain/ui/ds -x c++ -fsyntax-only -
