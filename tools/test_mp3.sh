#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
gcc -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main/kobako -I .cache/codecs/minimp3 tools/test_mp3.c \
  main/kobako/mp3_decode.c tools/host_fir_stub.c main/scene/fxmath.c components/minimp3/minimp3.c -lm -o /tmp/test-kobako-mp3
/tmp/test-kobako-mp3 "$@"
