#!/usr/bin/env bash
# Full serialized bytecode, including debug metadata, versus a local ref.
set -euo pipefail
cd "$(dirname "$0")/../.."
ref=${1:-origin/vm/main}
out="$PWD/.cache/parser-trim-compare"
qjs=components/quickjs-ng/quickjs-ng
mkdir -p "$out"
VMTEST_CFLAGS='-fno-pie -no-pie' bash tools/vmtest/build.sh o2
git show "$ref:$qjs/quickjs.c" > "$out/baseline-quickjs.c"
flags=(-std=gnu11 -O2 -g -fno-pie -no-pie -DQUICKJS_NG_BUILD -D_GNU_SOURCE -I .cache/vmtest/include -I "$qjs" -I components/pocketjs_guest/include)
gcc "${flags[@]}" -w -c "$out/baseline-quickjs.c" -o "$out/baseline-quickjs.o"
objs=()
for n in dtoa libregexp libunicode quickjs-libc quickjs-vm; do objs+=(".cache/vmtest/obj-o2/$n.o"); done
for mode in baseline candidate; do
  engine="$out/baseline-quickjs.o"
  [[ $mode == candidate ]] && engine=.cache/vmtest/obj-o2/quickjs.o
  gcc "${flags[@]}" -Wall -Wextra -Werror tools/vmtest/compile_image.c "$engine" "${objs[@]}" -lm -lpthread -ldl -o "$out/$mode"
done
python3 - "$out" "$ref" <<'PY'
import pathlib, subprocess, sys
out = pathlib.Path(sys.argv[1]); ok = rejected = 0
paths = sorted([*pathlib.Path('apps').rglob('*.js'), *pathlib.Path('apps').rglob('*.mjs'), *pathlib.Path('tools/vmtest/corpus').glob('*.js')])
for path in paths:
    a = subprocess.run([out/'baseline', str(path), out/'baseline.bc'], capture_output=True)
    b = subprocess.run([out/'candidate', str(path), out/'candidate.bc'], capture_output=True)
    if a.returncode == b.returncode == 0:
        if (out/'baseline.bc').read_bytes() != (out/'candidate.bc').read_bytes():
            raise SystemExit(f'BYTECODE MISMATCH: {path}')
        ok += 1
    elif a.returncode == b.returncode == 1 and a.stderr == b.stderr:
        print(f'identical compile rejection: {path}: {a.stderr.decode().strip()}'); rejected += 1
    else:
        raise SystemExit(f'RESULT MISMATCH: {path}: {a.returncode} {a.stderr!r} / {b.returncode} {b.stderr!r}')
print(f'full bytecode versus {sys.argv[2]}: {ok} identical, {rejected} identical compile rejections, 0 mismatches')
PY
