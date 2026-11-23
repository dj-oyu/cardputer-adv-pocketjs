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
    gcc -std=gnu11 -c -O1 -g -w -DQUICKJS_NG_BUILD -DQJS_LEAN_LAZY_BUILTINS=1 -D_GNU_SOURCE \
      -I "$QJS" -I components/kobako_guest/include "$QJS/$f.c" -o "$CACHE/$f.o"
  fi
done
for options in '-O1 -g -fsanitize=address,undefined' '-O2 -fstrict-aliasing'; do
  gcc -std=c11 $options -Wall -Wextra -Werror tools/test_system_wall.c -o /tmp/test-system-wall
  /tmp/test-system-wall
  gcc -std=c11 $options -Wall -Wextra -Werror tools/test_system_notify.c -o /tmp/test-system-notify
  /tmp/test-system-notify
  gcc -std=c11 $options -Wall -Wextra -Werror tools/test_system_timer.c -o /tmp/test-system-timer
  /tmp/test-system-timer
  gcc -std=c11 $options -Wall -Wextra -Werror -I main/pet \
    tools/test_pet_hub.c main/pet/pet_hub_core.c main/system/sys_notify.c main/system/sys_timer.c main/system/sys_wall.c main/system/sys_state.c -o /tmp/test-pet-hub
  /tmp/test-pet-hub
  gcc -std=c11 $options -Wall -Wextra -Werror tools/test_system_clock_state.c -o /tmp/test-system-clock-state
  /tmp/test-system-clock-state
  gcc -std=gnu11 $options -Wall -Wextra -Werror -I tools/hostshim -I main/hal \
    tools/test_system_clock_device.c -o /tmp/test-system-clock-device
  /tmp/test-system-clock-device
  gcc -std=c11 $options -Wall -Wextra -Werror tools/test_system_state.c -o /tmp/test-system-state
  /tmp/test-system-state
  gcc -std=gnu11 $options -Wall -Wextra -Werror -I "$QJS" -I tools/hostshim -I main -I main/kobako \
    tools/test_kobako_clock.c main/kobako/kobako_clock.c main/system/sys_state.c \
    "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" "$CACHE/quickjs.o" "$CACHE/quickjs-vm.o" \
    -lm -o /tmp/test-kobako-clock
  /tmp/test-kobako-clock
  gcc -std=gnu11 $options -Wall -Wextra -Werror -fno-omit-frame-pointer \
    -I "$QJS" -I tools/hostshim -I main -I main/hal -I main/kobako -I main/vm \
    tools/test_kobako_power.c main/kobako/kobako_power.c main/kobako/kobako_api.c \
    main/system/sys_state.c main/system/sys_device.c main/system/sys_clock.c main/system/sys_notify.c main/system/sys_timer.c main/system/sys_ringer.c main/system/sys_wall.c tools/hostshim/hostshim_board.c \
    "$CACHE/dtoa.o" "$CACHE/libregexp.o" "$CACHE/libunicode.o" "$CACHE/quickjs.o" "$CACHE/quickjs-vm.o" \
    -lm -o /tmp/test-kobako-power
  /tmp/test-kobako-power
done
