#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
QJS=components/quickjs-ng/quickjs-ng
CACHE=/tmp/qjs-kasane-host
mkdir -p "$CACHE"
# Same shipping VM configuration as build_kasane_test.sh.
for f in dtoa libregexp libunicode quickjs quickjs-vm; do
  if [ ! -f "$CACHE/$f.o" ] || [ "$QJS/$f.c" -nt "$CACHE/$f.o" ] \
     || [ -n "$(find "$QJS" -name '*.h' -newer "$CACHE/$f.o" -print -quit)" ]; then
    gcc -std=gnu11 -c -O1 -g -w -DQUICKJS_NG_BUILD -D_GNU_SOURCE \
      -DCONFIG_POCKET_VM_SEGFRAMES=1 -DCONFIG_POCKET_VM_FLATCALLS=1 \
      -I "$QJS" -I components/pocketjs_guest/include "$QJS/$f.c" -o "$CACHE/$f.o"
  fi
done
for options in '-O1 -g -fsanitize=address,undefined' '-O2 -fstrict-aliasing'; do
  gcc -std=c11 $options -Wall -Wextra -Werror tools/test_system_clock_state.c -o /tmp/test-system-clock-state
  /tmp/test-system-clock-state
  gcc -std=gnu11 $options -Wall -Wextra -Werror -I tools/hostshim -I main/hal \
    tools/test_system_clock_device.c -o /tmp/test-system-clock-device
  /tmp/test-system-clock-device
  gcc -std=c11 $options -Wall -Wextra -Werror tools/test_system_state.c -o /tmp/test-system-state
  /tmp/test-system-state
  gcc -std=gnu11 $options -Wall -Wextra -Werror -fno-omit-frame-pointer \
    -I "$QJS" -I tools/hostshim -I main -I main/hal -I main/pocket -I main/vm \
    tools/test_pocket_power.c main/pocket/pocket_power.c main/pocket/pocket_api.c \
    main/system/sys_state.c main/system/sys_device.c main/system/sys_clock.c tools/hostshim/hostshim_board.c \
    "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" "$CACHE/quickjs.o" "$CACHE/quickjs-vm.o" \
    -lm -o /tmp/test-pocket-power
  /tmp/test-pocket-power
done
