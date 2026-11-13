# Experimental rigid DQ: host core and native packet contracts

Run `python3 tools/ayatsuri/dq-oracle/host/run.py /tmp/dq-host-new` from this checkout. Requires cc and Node; uses a fresh output directory. O2 and ASan/UBSan run; LSan is disabled. Assertions must remain enabled. Base is public oracle commit 87d2aa5a.

The support header is a test-only copy of an existing experimental rigid-DQ implementation, not a production API addition. Production LBS and AYNM are untouched. This runner compiles it explicitly; it is not included by firmware.

Core: public axis (1,0,1)/sqrt(2), angle60, pivot(1,0,0), six vertices and weights0/32768/65535. Independent closed-form expectations include quantization rather than using unquantized Blender values. Fresh maximum raw error8.19e-8; half-weight quantization component delta9.10e-6. Ten late-input/admission failures preserve every output byte; invalid-method and post-call input poison tests are separate.

Native: same public axis/angle, scaled grid and pivot15, existing public three-node fixture mesh1. Independent expected position/transported normal feed a second native evaluator. Pixels match through the shared renderer, posed pixels differ from cold, and the packet size/count/bytes remain after caller-gated DQ failure. Borrowed inputs can be poisoned before snapshot; snapshot renders after model/arena destruction. Linked allocation ledger ends at zero (peak6048 in this host test); this is NOT target RAM/stack accounting.

The two-pass core is synchronous and allocation-free. Rigid-only admission rejects scale, shear, reflections and ambiguous multi-influence hemispheres. Normal rotation is a fixed-weight transported-normal contract, not deformed-surface normal recomputation. Upstream authored scale must not be hidden by bind cancellation.

Not accepted: production method dispatch, JS/Scene integration, OOM/ready/workspace behavior of a future binding, actual DMA/flush, independent shading/raster correctness, general rig/morphs, device memory or FPS. Caller-gating a failed core call is demonstrated in the harness only, not claimed as existing production DQ error handling. No private assets, generated files, or performance threshold changes are included.

Snapshot capacity follow-up: the native test also checks zero/one-byte-short packet capacity, work-limit exhaustion, exact-capacity retry, and failed second-append prefix/guard preservation without allocation. This is single-packet append admission, not heap OOM, multi-node cursor retry, or whole-frame rollback. Existing snapshot-after-free image checks remain separate.

Source-admission proposal (test-only): independently admitting G and inverse-bind rejects scale/shear/reflection cancellation that palette-only validation accepts. Seven negatives include nonfinite data and missing upstream rigid-source assurance, preserving output on failure. The synthetic assurance flag models a caller responsibility; it does not establish trustworthy authoring provenance. Near-identity floating-point tolerance cannot establish that provenance either. Real converter/JS integration and OOM remain unaccepted.
