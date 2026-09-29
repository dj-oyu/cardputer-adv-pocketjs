#!/usr/bin/env bash
# The automatic clock sync's policy and attempt, and net_service's counting, on
# the host with a fake radio and a fake clock (tools/test_net_autosync.c).
# WSL only, like the other sanitizer builds: MinGW has no ASan.
set -euo pipefail
cd "$(dirname "$0")/.."
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -I tools/hostshim -I main/pocket \
  tools/test_net_autosync.c main/pocket/net_service.c main/pocket/net_autosync.c \
  -o /tmp/test-net-autosync
/tmp/test-net-autosync
