#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
gcc -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main/kobako -I main/hal tools/test_mp3_sd_reader.c \
  -o /tmp/test-kobako-mp3-sd-reader
/tmp/test-kobako-mp3-sd-reader
