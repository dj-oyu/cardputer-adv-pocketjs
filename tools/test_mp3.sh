#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
gcc -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main/pocket -I .cache/codecs/minimp3 tools/test_mp3.c \
  main/pocket/mp3_decode.c components/minimp3/minimp3.c -lm -o /tmp/test-pocket-mp3
/tmp/test-pocket-mp3 "$@"
