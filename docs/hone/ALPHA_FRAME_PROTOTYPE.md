# Opt-in owned alpha frame lane

Enable with CMake `-DKASANE_ALPHA_FRAME_PROTOTYPE=ON`, or the source-bound
broker variant `alpha-frame-prototype` (Kconfig symbol
`CONFIG_KASANE_ALPHA_FRAME_PROTOTYPE=y`). Default OFF adds no alpha
fields, branches, allocations or packet copies to the existing frame path.
This is a production-path prototype, not a shipped full-character application.

A native caller opens `beginMeshFrame`, then supplies bounded packets through
`pocket_proc_native_alpha`. Inputs are clipped and copied to candidate-owned
storage, never retained as mutable caller pointers. No per-pixel callback or
full-frame image copy is introduced. Native transaction marks include alpha
count and per-surface monotonic revision; rollback cannot mutate a pinned or
later frame. Each packet is 84 bytes on the measured host; each packet buffer is
bounded to 65,536 bytes. Two buffers preserve candidate and committed data.

Commit verifies fragment capacity and explicit frame work before publishing.
Vertex AABBs prove exterior samples empty; the complete potentially covered
region is validated. Two alpha traversals share the frame work budget with
opaque raster. Publish rejection unpins the candidate. Missing image-resource
admission is checked before pinning. Image IO retains the exact candidate for
retry; backdrop IO repairs from the old committed snapshot. Successful
presentation swaps painter, opaque and alpha snapshots together. Reset runs
only after the separate runtime/caller STOP-drain contract is acknowledged.

Tests:

```
ASAN_OPTIONS=detect_leaks=0 python tools/kasane_contract/run_pocket_proc_qjs.py --opt 1 --alpha-prototype
python tools/kasane_contract/run_alpha_bounds.py
```

The real QuickJS/proc tests cover copied source, denied owner, rollback token,
publish/resource BUSY retry, fragment/work rejection, image retry, repeated
backdrop failure/repair, ACK and 1/2/4/8-row identity. Bounds tests compare exact
pixels against identical full-screen traversal for 32 screen-clipped/offscreen scenes (not a near/far clipping sweep).
They run O2 and ASan/UBSan; leak checking is excluded under ptrace.

## Host observations, not device performance

Same O1/ASan synthetic one-triangle test: full-screen preflight/publish 1,340 us
and four-frame row replay 5,927 us; bounded traversal previously observed 43 us
and 443 us. Timing noise and sanitizer overhead apply. Same-executable 32-scene
O2 comparison: bounded replay 2,535 us vs full-screen 64,467 us total; sanitizer
run 9,993 us vs 335,986 us. These compare this alpha algorithm's two traversal
strategies, NOT alpha vs an equivalent opaque scene or full-character FPS.

Host O1/sanitizer stack-usage reports: native_alpha 912 B, frame_preflight
880 B, commit_impl 160 B, render_rows 464 B (dynamic/bounded). These are
individual host frames, not ESP stack high-water or summed call-chain limits.
Firmware ON build, worst-case packet count,
physical heap/stack/DMA, whole-model material controls and actual 20 fps remain
separate acceptance gates. Public normal builds with this option OFF do not
validate the enabled firmware path. A full private-model caller is not yet wired.
Fast-math is rejected because it invalidates finite-value admission checks.
