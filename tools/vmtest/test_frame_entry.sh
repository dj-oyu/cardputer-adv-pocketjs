#!/usr/bin/env bash
# Actual guest.c + jsconsole.c + production source-entry slice; Linux only.
# No device, timer thread, ESP-IDF, or external downloads are used.
set -euo pipefail
cd "$(dirname "$0")/../.."
variant=${1:-asan}
case "$variant" in
  o2) flags=(-O2 -g);;
  asan) flags=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined);;
  *) echo 'usage: test_frame_entry.sh [asan|o2] [--oom-lazy]' >&2; exit 2;;
esac
if [[ -n ${VMTEST_CFLAGS:-} ]]; then
  read -r -a extra_flags <<< "$VMTEST_CFLAGS"
  flags+=("${extra_flags[@]}")
fi
out=${VMTEST_OUT:-$PWD/.cache/vmtest}
export VMTEST_OUT="$out"
bash tools/vmtest/build.sh "$variant"
python3 tools/vmtest/frame_entry_source.py "$out/include/frame_entry_source.inc"
qjs=components/quickjs-ng/quickjs-ng
guest=components/pocketjs_guest
gcc -std=gnu11 "${flags[@]}" -DQUICKJS_NG_BUILD -D_GNU_SOURCE \
  -Wall -Wextra -Werror -I "$qjs" -I "$guest/include" -I "$out/include" \
  -I tools/vmtest/frame_entry_shim -I tools/hostshim -I main/ui -I main/text -I main/pocket \
  tools/vmtest/test_frame_entry.c "$guest/src/guest.c" \
  "$guest/src/vm_sched.c" "$guest/src/vm_clock.c" "$guest/src/block_cache.c" \
  main/ui/jsconsole.c "$out/obj-$variant/"*.o -lm -lpthread -ldl \
  -o "$out/test-frame-entry-$variant"
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
"$out/test-frame-entry-$variant" "${@:2}"
