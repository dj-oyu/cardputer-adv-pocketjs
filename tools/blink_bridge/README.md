# Indexed controls to Scene host bridge

Test-only follow-up to `tools/blink_geometry` at `f4d25d92`. No product API, native renderer, private character or alpha-mask changes.

The prior Node adapter validated three own indexed controls before constructing its result. This harness tests that boundary in real QuickJS and the existing `Scene.evaluateSkins`/native snapshot path:

- Accept a three-element Array and read each own indexed finite normalized Number once.
- Capture binary64 control values before any Scene target mutation. Compute the visibility decision in that domain, independently from Float32 geometry.
- Construct the geometry buffers before applying visibility and evaluating the existing identity-skin batch.
- Reject invalid controls, sparse arrays, typed arrays and throwing getters without changing the Scene target mask, ready state or held buffers. Repeat rejection while a render packet is pending and verify the expected pixels survive IO retry.
- Verify valid getters run once while the prior Scene remains ready; compare their resulting pixels to the existing all-open state.
- Retain 343 forward + 343 reverse control combinations, integer-lattice rectangle coverage, buffer poisoning/disposal after submit and allocation ledger checks from the prior geometry harness.

Run `python tools/blink_bridge/run.py`. The runner records sources, compiler commands and four arms (4/8 strip rows × direct 0/1). ASan/UBSan are enabled; LSan is disabled and the explicit allocation ledger must be zero.

This only connects a test adapter to existing host Scene behavior. It does not create a production blink API or prove native OOM transactionality, atomic reads of hostile/mutating objects, arbitrary alpha/private-shader equivalence, target admission or physical 20 FPS. Native evaluation/submission failures are not claimed to restore Scene state. Getter rejection checks do not undo arbitrary side effects performed by user-supplied getters themselves.
