# Firmware cleanup continuation, 2026-11-13

Recovered the public baseline `test/session-cleanup-host-boundary`, commit
`28f9c1fb8d7fa5ba0baa62de06fdd477e324858a`. The tested app_session.c and shiki_heap.c
in the shared checkout match the baseline's recorded SHA-256 values exactly.
No product file or other worker's checkout was edited.

## Executed

- Original app_stop host probe: O2 and ASan/UBSan PASS.
- Original single-owner heap lifecycle probe: O2 and ASan/UBSan PASS.
- New AV/Kasane reset-return matrix (four combinations): O2 and ASan/UBSan PASS.
  A modeled external owner remains allocated when either reset reports failure;
  repeated stop does not release it, restart is rejected until its last free,
  then a new 49,152-byte heap starts empty. The conditional output-source reset
  occurs only after Kasane reset succeeds and receives the AV result unchanged.
- Same matrix using the exact app_stop extraction from PR #131 head
  `0befe3d4ea7efc22f079ccc6dcd2288732787d8c`: O2 and ASan/UBSan PASS.
  A stub present_settle is asserted before pocket_app_reset. It returns an error;
  cleanup still proceeds, as the source deliberately discards its return value.

Run `python3 run_extended.py <baseline-checkout> [<pr131-checkout>]`.
The optional second checkout is pinned to the app_stop at PR #131 head above.
The runner verifies baseline session, heap, and PR stop hashes, builds in a temporary
directory, and never downloads or modifies either checkout. With no second argument,
it explicitly reports the PR comparison as not executed. PR product sources are not
part of this test publication.
The PR #131 shiki_heap.c is byte-identical to the baseline heap implementation.

## PR #131 integration boundary

PR #131 adds present_settle before app_stop's teardown. Its actual implementation
joins a busy display before accounting a deferred present. Ignoring the accounting
result at stop is not by itself proof of unsafe teardown: completion and accounting
failure are separate concerns. The host test proves call ordering and behavior after
the stub returns, NOT actual disp_join completion, cross-core exclusion, or failure
handling. Those remain necessary integration checks after the owner's merge decision.
PR #131 was read only; no merge or edits were performed.

## Limits

Reset/guest/finalizer/present functions are deterministic stubs. The retained block
models ownership; this does not establish real AV/Kasane reset behavior. No full
app_start/manifest admission, real QuickJS finalizers, concurrent display, DMA/LCD,
target RAM/stack, real peripheral restart, or FPS claim. Leak detection is disabled.
No QEMU binary was available on PATH; no toolchain download or device action occurred.

Independent review reproduced the tests and checked the exact PR extraction. Its
source-hash recommendation was implemented in the final runner, followed by a fresh
8-executable O2/sanitized run. This improves reproducibility, not test scope.
