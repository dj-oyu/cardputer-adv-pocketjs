# Exact managed skin allocation failure boundary

Run `python tools/ayatsuri/skin_oom/run.py` on a host with GCC, binutils,
Python and Node. This test adds no product hooks or runtime behavior.

The existing guest-memory-limit tests can fail before the native workspace
allocation is reached. This companion test uses linker `--wrap=js_malloc` to
inject one failure at the managed skin workspace request. It verifies the
return address with `addr2line`: every failure must originate on the actual
`js_malloc(ctx,bytes)` source line in `managed_instance_evaluate_skin_mode` or
`managed_instance_evaluate_skins_mode`. The wrapper raises the ordinary QuickJS
OOM exception and returns NULL; it does not exhaust or test the allocator.

Four APIs (single/batch, LBS/rigid DQ) each cover three phases:

1. Initial skin allocation after a cold evaluated frame.
2. Growth after a successful smaller skin workspace.
3. Growth with a new dirty Scene position target.

For clean phases, the complete core-instance struct, core arena and retained
workspace bytes remain unchanged, and no core evaluate call occurs. In the
dirty-target phase the position target **has already been applied** before
allocation: its new native pose/epoch remains and the Scene dirty mask is
cleared. This deliberately records the existing lack of whole-Scene rollback,
rather than introducing a new transaction guarantee.

All phases retain workspace capacity, leave the Scene not ready and not busy,
preserve an already submitted frame through an IO retry, then recover after
fault removal with different rendered pixels. Final disposal requires a zero
explicit allocation ledger. Sizes on this host are 304 -> 352 bytes for the
single path and 327 -> 743 bytes for the batch path. These are workspace sizes,
not total guest/target RAM.

The pending-frame assertion first records image A, commits a different-clear
image B and verifies B differs, then leaves A pending before fault injection.
Thus falling back to the older committed B cannot satisfy the comparison.

The runner builds real QuickJS and native rendering with ASan/UBSan, disables
LSan explicitly, and records commands, input hashes, call-site symbols and
four arms (strip rows 4/8, direct 0/1): 48 injected allocation failures total.
Inputs are existing public synthetic rectangles. No private model, native
projection-failure rollback, allocator fragmentation/exhaustion, PR #131,
real DMA/peripherals, target FPS or full-character acceptance is established.
