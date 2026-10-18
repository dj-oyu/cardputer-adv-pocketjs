# Explicit DERBY diagnostic source

This branch is an instrumented source variant for measurement acceptance. It
defaults telemetry ON and is not intended to merge its ON default into main.
The ordinary telemetry development branch and shipping configuration remain OFF.

A normal `idf.py build` must complete the `diagnostic_mode_proof` ALL target.
The target checks the actual cache, native ELF symbols, exact generated play
bytes embedded in the ELF, IDF 6.0.1 application descriptor, and matching BIN/ELF
hash. An existing OFF cache makes the build fail; it is not silently forced ON.
CI supplies no telemetry mode override and independently requires ON in its
exact-head source receipt. Its artifact includes `diagnostic-mode-proof.json`.
The mandatory proof runs Python with `-B` so imported verification modules do
not create bytecode in the immutable source snapshot. Regression validation
hashes every source file before and after complete and incremental builds;
only Git metadata and the explicitly selected build output are excluded.

The proof also runs from a source archive without Git metadata. Source identity
comes from the broker's existing source receipt and successful build manifest.
Join both BIN and ELF SHA256 values from the proof to that same build manifest,
whose source provenance must match the full accepted commit and tree. Capture
the `DERBY_DIAGNOSTIC_BUILD_PROOF` line using the existing build-log cursor with
the `unknown` level included, then persist it with the manifest, source receipt,
job/plan identity, commit and tree before the 300-second log retention expires.
The proof JSON itself is not listed in the broker's BIN/ELF-only manifest.
A manifest without the corresponding proof is insufficient to recheck mode.

This requires no new mode API, caller-selected build arguments, registration,
profile or authorization changes. A diagnostic build and CI success do not
constitute a device observation or authorize a flash.
