# Compact function allocation proposal

Reconstructed on normal main `05f472ecc9535b71aefbdd0ed73d8891dfb4aace`
(including merged #95), from the published #46 changes in #37's ancestry:

| Source | Included change |
| --- | --- |
| `886608f209f1eb4b781adff5b32b68a3306c182e` | Clear the complete final function header, including padding, then copy the packed payload once. |
| `446676e6f7c9eb1597d54dff9598b15327932cf6` | Align segmented frame blocks and the actual link type to the declared value/pointer ABI. |
| `2ef9a72e0d7b5b9b28c34bbedd30535e9184634d` | Compact function flags into GC-unused storage, derive pool/local pointers, align function allocations, and recover raw pointers on both GC free paths. |
| `072880da3c0ff9370299ed57fab0bb4a93725b23` | Propagate writer atom-index and buffer OOM through the existing single cleanup paths. |

The initialization change overlaps the compact change; it is applied once.
The focused runners and fixtures come from published #46 head
`10fbed312682a122011f447290ba82891c16e8fd`, adapted to the current 87-case
corpus. No unpublished environment or old integration/configuration is used.
Old #37 stays open. #96 pressure retention and #97 workspace reuse are separate.

## Layout and ownership

Function flags use `JSGCObjectHeader.dummy2`; `dummy1` records the raw allocation
shift. The GC never uses either field. The pool is directly after the header;
local definitions follow it only when the compiler/reader has definitions.
Stored closure and bytecode pointers preserve reader initialization semantics.
Normal reference destruction and deferred cycle-GC frees both recover the raw
allocator pointer. Other allocator/realloc/cache paths are unchanged.

The target-like 32-bit ABI has an 80-byte function header and requests
84 bytes plus payload, including the maximum four-byte alignment adjustment.
Native64 uses 112/116 bytes. The original 32-bit header/request was 92 bytes.
These are type/request sizes, not measured heap buckets, retained RAM or speed.
The frame repair independently increases the 32-bit link from 4 to 8 bytes and
rounds each block to 8; its charge grows by 4 bytes for even var-ref counts and
8 for odd counts. Existing budget checks charge/refund that full rounded size.

The ROM generator now emits `offsetof(JSFunctionBytecode, line_num/col_num)`
instead of baking host offsets into target code. Regeneration changes only those
two initializers. #95's C builtins, ROM graph, generator rejection rules, sticky
DynBuf fix, OOM corpus, dependency layout and product configuration are retained.

## Reproduction

From the repository root:

```sh
python3 tools/vmtest/function_allocation/run.py --variant o2
python3 tools/vmtest/function_allocation/run.py --variant asan --oom
python3 tools/vmtest/function_allocation/run.py --m32 --oom
python3 tools/vmtest/function_allocation/run.py --variant o2 --only poison --negative
python3 tools/vmtest/function_allocation/run.py --variant o2 --only poison --negative-copy
python3 tools/vmtest/function_allocation/compare_main.py --variant o2 --rom
bash tools/vmtest/floor/gen_rom_intrinsics.sh --check
bash tools/vmtest/oom_targets.sh
```

`run.py` builds baseline (whole-allocation zeroing), candidate (header-only
zeroing), and poison engines with the same repaired compact layout. Debug/strip,
O2/ASan+UBSan, and native/m32 have separate output/source directories. Poison
fills the allocation before production initialization, checks every header byte,
and requires every payload byte to be copied exactly once. Missing header clear
and missing bytecode-copy negative controls must fail their specific assertions.
All current corpus files must have identical output and exit status between modes.
Native debug must also match the shared expectations. Stripping changes the
existing traces/allocation targets in `error_toplevel`, `job_throw`,
`math_fast_call`, `oom_resolving_functions`, `oom_with_resolvers`, and `try_finally`;
this exact set is checked before comparing every candidate/poison byte.

The real-engine frame fixture tests nested odd/even tails, links, segment and
budget boundaries, charge/refund, dedicated allocations and refused-push rollback.
The bytecode fixture adds empty/nested pools, all four wire strip combinations,
truncated reads, compiler/reader/writer allocation failure points, shifts 0/4,
overflow rejection, reference destruction, cycles and runtime teardown.
ASan closure/generator sweeps cover at least 4,800 allocation points per file.
Sanitizer reports, signals, assertions and timeouts are never accepted as a
baseline difference. These tests do not exhaust arbitrary malformed bytecode or
multiple simultaneous allocation failures.

`compare_main.py` additionally builds untouched pinned main and the candidate
and compares every corpus output/exit status directly, with native O2/ASan,
ROM/non-ROM and debug/strip configurations. ROM has pre-existing differences
from shared expectations: the log lists them explicitly, while requiring exact
main/candidate equivalence. A successful comparison is not an all-pass ROM corpus.

The dedicated workflow verifies the exact PR head, runs the native/m32 allocation
matrix, native pinned-main comparisons, relocation/budget checks, the ROM generator,
and #95's DynBuf/OOM-target controls. Existing ordinary firmware workflows supply
actual Xtensa compilation/linking. Logs are under
`.cache/function_allocation/evidence/`; CI uploads them even on failure.

Local/CI results belong to the published PR head and are recorded in its body.
Historical #46/#37 results are source context only. Local m32/Xtensa availability
and device heap/timing/physical-display acceptance are separate limits. No device,
broker, merge, branch deletion or old-PR closure is part of this proposal.
