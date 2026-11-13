# Experimental rigid DQ: host core and native packet contracts

Run `python3 tools/ayatsuri/dq-oracle/host/run.py /tmp/dq-host-new` from this checkout. Requires cc and Node; uses a fresh output directory. O2 and ASan/UBSan run; LSan is disabled. Assertions must remain enabled. Base is public oracle commit 87d2aa5a.

The support header is a test-only copy of an existing experimental rigid-DQ implementation, not a production API addition. Production LBS and AYNM are untouched. This runner compiles it explicitly; it is not included by firmware.

Core: public axis (1,0,1)/sqrt(2), angle60, pivot(1,0,0), six vertices and weights0/32768/65535. Independent closed-form expectations include quantization rather than using unquantized Blender values. Fresh maximum raw error8.19e-8; half-weight quantization component delta9.10e-6. Ten late-input/admission failures preserve every output byte; invalid-method and post-call input poison tests are separate.

Native: same public axis/angle, scaled grid and pivot15, existing public three-node fixture mesh1. Independent expected position/transported normal feed a second native evaluator. Pixels match through the shared renderer, posed pixels differ from cold, and the packet size/count/bytes remain after caller-gated DQ failure. Borrowed inputs can be poisoned before snapshot; snapshot renders after model/arena destruction. Linked allocation ledger ends at zero (peak6048 in this host test); this is NOT target RAM/stack accounting.

The two-pass core is synchronous and allocation-free. Rigid-only admission rejects scale, shear, reflections and ambiguous multi-influence hemispheres. Normal rotation is a fixed-weight transported-normal contract, not deformed-surface normal recomputation. Upstream authored scale must not be hidden by bind cancellation.

Not accepted: production method dispatch, JS/Scene integration, OOM/ready/workspace behavior of a future binding, actual DMA/flush, independent shading/raster correctness, general rig/morphs, device memory or FPS. Caller-gating a failed core call is demonstrated in the harness only, not claimed as existing production DQ error handling. No private assets, generated files, or performance threshold changes are included.

Snapshot capacity follow-up: the native test also checks zero/one-byte-short packet capacity, work-limit exhaustion, exact-capacity retry, and failed second-append prefix/guard preservation without allocation. This is single-packet append admission, not heap OOM, multi-node cursor retry, or whole-frame rollback. Existing snapshot-after-free image checks remain separate.

Known-axis follow-up: 48 signed-permutation cases cover X/Y/Z rotations by 0/90/180/270 degrees, all four influence slots, arbitrary translation and independently normalized normals. These exercise all matrix-to-quaternion conversion branches. Single-influence half-turn acceptance is separate from the existing ambiguous multi-influence half-turn rejection. The test-only DQ header was byte-equal to the published mixed branch production header at 3fc5166a (SHA-256 a926365a010507b935b7ce31f0cff84c3a498562f554818274f6a7cc5707a455); this does not establish full production dispatch or target behavior.

## Connect the same oracle to production headers

Run `python3 tools/ayatsuri/dq-oracle/host/run.py NEW_OUTPUT --product-root PRODUCT_CHECKOUT`.
This compiles the **same** core and native tests against that checkout's
`main/ayatsuri/ayatsuri_skin_dq.h`, model/renderer sources, fixture generator,
and existing render helper. No test-support include directory is added in this mode;
a missing production DQ header is an error, not a fallback to the experimental copy.
Compiler header traces (`*.compile.log`), separate test/source heads, source hashes,
commands and logs are saved. Both checkouts remain untouched.

The product-header mode was exercised against the Scene integration tree
`75f005d233f1fa32799c1cc7af50591fc6dbda09` (public commit `94fde6ab`).
This closes the copied-header-only oracle gap, but it does not invoke JS dispatch,
Scene admission, allocator failure paths, real display, or the device. Those require
the distinct integration tests; do not describe this runner as full product acceptance.
