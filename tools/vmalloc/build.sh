#!/usr/bin/env bash
# build.sh — builds tools/vmalloc into .cache/vmalloc/{vmalloc_replay-asan,vmalloc_replay-o2}.
# WSL only (no gcc on Windows), same as tools/vmtest/build.sh.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
OUT=../../.cache/vmalloc
mkdir -p "$OUT"

SRCS=(replay.c adapter_tlsf.c adapter_estalloc.c adapter_naive.c
      vendor/multi_heap.c vendor/tlsf/tlsf.c vendor/estalloc/estalloc.c)
INCLUDES=(-I. -Ivendor/include -Ivendor -Ivendor/tlsf -Ivendor/tlsf/include -Ivendor/estalloc)
# VMALLOC_TLSF_ALIGN_LOG2=3 (8-byte stride): see the VMALLOC PATCH comment in
# vendor/tlsf/tlsf_control_functions.h for why this host build cannot use
# the stock 4-byte stride (ALIGN_SIZE_LOG2=2, the device's actual value).
DEFS=(-DVMALLOC_TLSF_ALIGN_LOG2=3)

echo "building vmalloc_replay-asan (ASan+UBSan)"
gcc -std=gnu11 -O1 -g -Wall -Wextra "${DEFS[@]}" "${INCLUDES[@]}" \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${SRCS[@]}" -o "$OUT/vmalloc_replay-asan"

echo "building vmalloc_replay-o2"
gcc -std=gnu11 -O2 -Wall -Wextra "${DEFS[@]}" "${INCLUDES[@]}" \
    "${SRCS[@]}" -o "$OUT/vmalloc_replay-o2"

echo "done: $OUT/vmalloc_replay-{asan,o2}"
