# Procedural array length experiment

One product change, based on integration `578d552384c992cb852822d40e2635e7b317d498`
(includes PR46). Main is `e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267`.
PR49 is based on PR48 `879da5aaa818c497d29cd6c3b67d9134cd315abe`; neither
prepared-call dispatch nor app control is part of this experiment.

`array_length()` retains its strict `JS_IsArray` guard and uses existing
`JS_GetLength` instead of looking up the string `length` and repeating finite,
range and integer validation. In the pinned engine `JS_IsArray` accepts only
JS_CLASS_ARRAY, not Proxy. Actual arrays own a nonconfigurable uint32 length;
`JS_ToLengthFree` therefore cannot invoke user coercion here. No engine/API,
allocation, input coercion or ownership changes. Keep the Proxy rejection
regression if updating QuickJS changes `JS_IsArray` semantics.

The path is shared by procedural array draw and registration, including dynamic
instruction rows. DERBY's `derby_view.js:41` generally forwards numeric inputs;
those draws are a control, not the optimization's target. Its zero-input array
draws and other apps using array inputs can reach this path. A host array-draw
result does not establish full DERBY FPS or device improvement.

## Host protocol

```
python3 tools/vmtest/length-read/run.py --pairs 100 --loops 100000
LENGTH_READ_SYSTEM_M32=1 python3 tools/vmtest/length-read/run.py --m32 --pairs 100 --loops 100000
```

Native O2, pinned to the first allowed CPU. Baseline proc source is extracted
from the exact integration commit; all other product sources are identical.
Each child process has a fresh runtime/context. Setup, boundary assertions,
source compilation, ROM registration (draw cases), and teardown are outside
the draw timer. Startup times the eval/compile/register expression together.
Each case discards four warm-up pairs; recorded order alternates AB/BA.
Caches are host-warm across processes, contexts are fresh, and the first frame's
native workspace allocations remain inside the draw interval. Frames alternate
one input coordinate, execute generated real `derby.pt` ROM, commit, acknowledge
and hash every output segment. Hash/count/result parity is required per pair.
The timer includes JS looping, bridge, plan execution, commit and output hashing;
it excludes the LCD. CPU thread time accompanies monotonic wall time. There are
no timed logs, heap scans or allocation callbacks in timing binaries.

Separate accounting binaries forward libc malloc/calloc/realloc and actual usable
size; report requested allocation calls/bytes during the same interval. `adapter`
means direct allocations in pocket_proc.c, not all firmware/native allocations.
No peak heap claim. This is not the product GUEST_ALLOCATOR or board scheduler.

One 28-value boundary matrix covers numeric values, signed zero, nonfinite and
nonnumber rejection; actual/sparse/frozen lengths through UINT32_MAX; and Proxy,
typed-array and array-like rejection without calling their traps. Existing
procedural contracts cover rendering, numeric/array equivalence and failures.

Independent read-only review found no product semantic or harness blocker.
The actual O2 scalar/fake-PIE adapter contract passes all 48 exact RGB565 frames.
The intentional OFF text change will be detected by the existing shared
`tools/test_derby_detail_generation.py` freeze at `4e4d7bcc`. Per ownership
instructions that file is unchanged. A proposed narrow reference update points
only pocket_proc.c to the separately published product commit
`8dcb42110b56280f5524d3e207ce3b32b7b5bc02`, retaining the other four references and
changed-return negative control. The proc comparison and negative mutation were
run separately and passed locally; adopting the shared reference update requires
coordination with the parent/workspace research owner. The full local historical
generator check also needs missing ancestor objects; CI fetches full history.

PR42/44 ownership boundary: no LCD TX/window, workspace union, image cache,
rendering or allocation changes. Only array_length() and its separate measurement
files are owned here. The proposed shared OFF reference update is held for coordination; workspace
research owns that proof boundary and must preserve the negative control.

## Results

Local first confirmation: 100 pairs x 100,000 frames, native GCC 14 O2:

| Case | Baseline median ns/op | Candidate median ns/op | Paired median ratio |
|---|---:|---:|---:|
| Numeric draw control | 764.19 | 759.85 | 0.9968 |
| Array draw | 951.18 | 922.24 | 0.9706 |

Array paired-median bootstrap 95% interval: 0.9617–0.9823 (5000 resamples,
seed 423). This interval describes these host samples, not target hardware.
Allocation counts match: numeric 13 JS requests/928 B total per 100k batch;
array 300013/21600928 B; both four adapter requests/31754 B. Startup 46 JS
requests/2318 B plus one adapter request/56 B. No allocation reduction expected:
the builtin atom already exists; this removes name lookup and validation work.
Raw local evidence: [first summary](proc-length-read-results/first-summary.json),
[first trials](proc-length-read-results/first-trials.json),
[repeat summary](proc-length-read-results/repeat-summary.json),
[repeat trials](proc-length-read-results/repeat-trials.json). These files record
source SHA256 even though the measurements preceded the publication commit.

Second confirmation: another 100 pairs x 100,000 frames: numeric 769.17 vs
763.94 ns/op (paired ratio 0.9927); array 948.72 vs 913.70 ns/op (ratio 0.9676).
Allocation metrics again match. This is a reproducible ~3% host array-path signal;
CI/m32 and actual device results remain pending.

A preceding direct numeric-tag reader experiment was rejected: paired numeric
ratios 0.9952/0.9956 over 100 pairs x 50k/100k, both bootstrap intervals included
1. The first 40-pair apparent 2.5% saving did not reproduce. It is not combined
with this change.

## Device comparison

The dedicated workflow builds baseline integration and candidate independently
using IDF 6.0.1, the same product sdkconfig/partitions/security, and the existing
finite DERBY fixed-workload diagnostic (detail ON, DMA overlap OFF defaults). No custom benchmark app, app control or
prepared machinery is added. Compare the emitted workload identities and existing
finite frame/bridge timing/resource records; do not infer FPS from host batches.
Artifacts contain app, bootloader, partition table, ELF/map, flash arguments,
verification/transport/stack evidence and SHA256SUMS. Standard flash offsets are
bootloader 0x0, table 0x8000, app 0x10000; verify flasher_args.json and hashes.
Firmware execution/flash is delegated separately. No USB actions are performed
by this experiment's author. Both source heads and artifact identities must be
recorded by the device operator; revert to the original firmware afterward.
