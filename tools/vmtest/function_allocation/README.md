# Final function allocation and frame alignment leaf

Base: `5b47d28ce77cfe0778be15dab903c58da3eed330` on
`integration/derby-j1-dma-host-r2-20261020`.
New branch: `vm/function-allocation-leaf-r2-20261022`.
The old cloud workspace and its unpublished changes were not recovered or used.
The old published branch is not updated by this work.

## Change and safety audit

The function-allocation optimization changes only the final allocation in
`js_create_function`: `js_mallocz` becomes
`js_malloc`, followed by `memset(b, 0, sizeof(*b))` after the existing NULL check.
The deserializer's separate allocation and other function-allocation sites stay as-is.

The full `JSFunctionBytecode` struct, including padding, spare flag bits, unused
pointers and zero counts, remains initialized. Its packed tail is the contiguous
concatenation of constant pool, argument/local definitions, closure definitions,
and bytecode. There are no alignment gaps between those ranges. The five existing
copies overwrite every nonempty range. Struct representation padding in the
source definitions was already copied before this change; its treatment is unchanged.

On allocation failure, the original `goto fail` still frees the function definition
and throws the same OOM exception. The requested allocation size, allocation count,
quota checks and accounting stay the same. No ownership has moved at that point.
After successful allocation there is no subsequent `goto fail`. The debug pc2line
realloc can fail, but its existing fallback retains the original buffer. Debug and
`CONFIG_POCKET_VM_STRIP_DEBUG` both keep all header bytes initialized. Atom,
constant-pool JSValue, filename, source and closure ownership transfers are unchanged.
Realm duplication and GC registration occur at their original positions after
initialization, with no new fallible operation or publication of partial state.

The reduction per successfully constructed function is
`function_size - sizeof(JSFunctionBytecode)` bytes of requested zero-initialization.
The guest and vmrun custom calloc implementations explicitly memset their malloc
result, so this removes that many duplicate zero-writes there. It does not reduce
the allocation size or retained heap capacity, and no speed improvement is claimed.
Other allocators may optimize calloc using already-zero pages; the formula is not
a claim about physical memory traffic on every allocator.

## Reproduction

Run from the repository root:

```sh
python3 tools/vmtest/function_allocation/run.py --oom
python3 tools/vmtest/function_allocation/run.py --m32 --oom
python3 tools/vmtest/function_allocation/run.py --only poison --negative
python3 tools/vmtest/function_allocation/run.py --only poison --negative-copy
```

The matrix independently builds baseline, candidate and poisoned engines under
native O2 and ASan/UBSan, with debug metadata retained and stripped. `--m32` repeats
that matrix with the project's official sysroot flags, including `-malign-double`.
Each engine runs the entire 80-file VM corpus. Baseline is recreated by reversing
only this allocation change in a private source copy. No shared expected output,
prepare source or existing workflow is modified. Source/output directories are
separated by architecture, mode, variant and negative control. All three engines
retain the frame alignment repair below; baseline reverses only the function
zeroing optimization. The pre-repair sanitizer failure remains recorded by the
earlier CI head, rather than being accepted or suppressed in the matrix.

Debug native output must match the checked-in expected corpus. Strip debug changes
three stack-trace locations and two tests pinned to allocation-attempt numbers even
in the baseline: `error_toplevel`, `job_throw`, `try_finally`,
`oom_resolving_functions`, `oom_with_resolvers`. Those five baseline differences
are explicitly checked. All 80 candidate/poison outputs and exit statuses must
match the corresponding baseline byte for byte in each configuration. The m32
matrix also compares against its own baseline, since allocation indices/layouts
can differ. Sanitizer errors, assertions, signals and timeouts are never accepted
as a baseline expectation.

Poison mode fills the entire final allocation with `0xa5` before the production
header memset, verifies every initial header byte is zero, and tracks every payload
copy in a byte bitmap. Each payload byte must be copied exactly once within bounds
before GC registration. Test-only bitmap storage uses raw libc allocation, outside
the VM allocator and its injected failure numbering, and is freed before publication.
The two negative controls respectively remove the header memset and bytecode copy;
each must fail on its specific initialization/coverage assertion.

ASan OOM sweeps fail each individual VM allocation on closures and generators,
with at least 4,800 points per file per configuration. Successful-run traces are
counted first and increase that limit if necessary, so every allocation in those
workloads is covered. OOM sweeps check memory safety and hangs rather than matching
normal output. This is not an exhaustive sweep of every corpus file or every
combination of multiple allocation failures.
The existing OOM sweep disables LeakSanitizer; normal full-corpus runs retain
leak detection and explicitly reject AddressSanitizer, LeakSanitizer and UBSan
diagnostics, including in configurations compared against their own baseline.

All execution logs and traces are under `.cache/function_allocation/evidence/`.
Poison logs also include per-success header and payload byte sizes; these are
instrumented byte counts, not a heap or timing benchmark.

## CI and independent review

The dedicated `.github/workflows/function-allocation.yml` checks out and verifies
the exact PR head for both architecture matrices and the existing complete contract
script (`GAMES_M32=1`). It uploads evidence even on failure. Existing shared workflows
and `tools/vmtest/prepare/` are unchanged. Local native-only contracts are additional
evidence, not a substitute for full m32 CI.

An independent agent audited production initialization, padding, contiguous tail
coverage, debug/strip, ownership, atom cleanup, GC registration and OOM paths.
It found no required production fix and requested stronger coverage assertions,
negative controls and independent source directories; these are included here.
CI results and the final reviewed head must still be checked before integration.

## Environment provenance

On 2026-10-22 the regular `wait_for_environment` action returned `ready`.
Actual `pwd`, `git status`, `git log`, build, corpus and OOM commands were run.
The initial checkout was `e0fe98a`, not the integration base. Git fetch failed
through the environment proxy with HTTP 503, and the CLI token was invalid.
The connected GitHub read API supplied the exact base commit and recursive tree.
Missing blobs were downloaded read-only and all blob/tree IDs were verified locally.
The original signed commit was reconstructed byte for byte and verified as
`5b47d28ce77cfe0778be15dab903c58da3eed330`, with tree
`29afc3fa1093e4def7189588e5c55452566855e2`, before switching to the new branch.
Unavailable parent history was marked shallow; no substitute base commit was made.
The exact base tree has no AGENTS.md or `.agents/skills`; workspace `.agents` was
empty. No past uncertain blob operation was retried.

Native debug baseline/candidate/initial-poison corpus runs passed all 80 files.
Candidate and initial-poison ASan/UBSan debug OOM sweeps each passed 9,600 points.
The strengthened byte-coverage poison also passed native debug O2 and ASan/UBSan.
Final matrix and CI evidence should be read from their logs and the PR checks.
Local m32 preparation was blocked: packages were unavailable in the empty apt
index, and fetching the configured Debian snapshot index returned proxy HTTP 503.
No local m32 success is claimed. Publishing uses new GitHub objects/branch only;
force push, main updates, merges, deployment and device operations are excluded.

The pre-repair m32 CI (`ed9c761`, then `3f46a33`) reached the old-allocation baseline
but rejected UBSan unaligned `JSStackFrame` accesses before candidate execution.
The user subsequently authorized the following minimal repair within this PR.

## Frame alignment repair (separate commit)

The old 32-bit rule selected 4-byte block alignment and a 4-byte link prefix.
With the official `-m32 -malign-double` flags, the real `JSStackFrame` requires
8-byte alignment: a 16-aligned segment followed by the 4-byte link places its
first frame at 4 mod 8. Odd pointer tails can misalign later blocks as well.
The earlier CI failure is in `JS_CallInternal`, not the function bytecode allocation.

Block alignment now follows the maximum declared alignment of `JSValue` and
pointers. `_Alignas` on the existing `JSVMLink.caller_sp` member also aligns and
pads the actual link type. This is essential because call/return, GC and relocation
readers use both `link + 1` and `((JSVMLink *)frame) - 1`; changing a prefix constant
alone would break those inverse operations. Static assertions against the real
private `JSStackFrame` ensure block alignment, prefix alignment and trailing
JSValue-slot alignment. No frame field, GC root, ownership transition, exception
choice, allocator failure handling, segment growth policy or segment size changes.

Resource effect: native 64-bit layout stays unchanged. On the official m32 ABI
and the installed Xtensa ESP32-S3 GCC ABI, frame body size stays 48 bytes, alignment
is 8, and the link grows from 4 to 8 bytes (4 bytes of tail padding). The entire
block is rounded to 8 instead of 4. With `n` var-ref pointers and 8-byte value slots,
the old charged size is `52 + 8*slots + 4*n`; the new charge is
`round_up_8(56 + 8*slots + 4*n)`: +4 bytes for even `n`, +8 for odd `n`.
On a 32-bit ABI whose JSValue alignment is only 4, both the link and block remain
4-aligned. The repair can reduce frames per segment and nesting capacity at a
fixed byte budget. For example 3 value slots plus 1 var-ref pointer cost 80 -> 88
bytes, so a fixed 512-byte segment holds 6 -> 5 such blocks. Budget checks and
push/pop continue to charge/refund the same rounded size, including this padding;
the byte limit and the RangeError-versus-OOM contract are preserved. Segment counts,
allocation positions and peak heap use can increase. These costs are separate from
the duplicate zero-write reduction, and no net heap or speed benefit is claimed.

`frame_alignment.c` includes the real engine and tests 64 nested frame blocks
with odd/even pointer tails across segment boundaries, typed frame/value/pointer
accesses, link address round trips, exact charge/refund, budget boundaries,
dedicated segments and rollback after a refused allocation. The candidate debug
build runs it under each architecture and O2/ASan+UBSan variant. The existing full
corpus exercises GC, closures, generators, yielding and exception paths. A local
ASan relocation build also passed all 80 files with `--force-reloc`.

The installed Xtensa ESP32-S3 GCC 15.2 compiled the actual engine/test source with
`-DFA_EXPECT_ALIGN8 -fsyntax-only`, including assertions that frame size is 48,
alignment is 8 and link size is 8. This is a compile/layout check, not a device run.
Native and m32 CI must pass again at the new head; the previous head's otherwise
passing workflows are not evidence for the repaired head.
