#!/usr/bin/env bash
# Builds and runs tools/test_keystate.c twice: ASan+UBSan for the edge rules,
# TSan for the seqlock between one writer and two readers. WSL (gcc).
#
#   wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs && bash tools/build_keystate_test.sh"
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=${OUT:-/tmp/test-keystate}
for san in address,undefined thread; do
  gcc -std=gnu11 -O1 -g -fsanitize=$san -Wall -Wextra -Werror -fno-omit-frame-pointer \
    -I main/hal tools/test_keystate.c main/hal/keystate.c -pthread -o "$OUT-$san"
  # TSan needs a fixed layout on recent kernels ("unexpected memory mapping").
  if [ "$san" = thread ]; then setarch "$(uname -m)" -R "$OUT-$san"; else "$OUT-$san"; fi
done
