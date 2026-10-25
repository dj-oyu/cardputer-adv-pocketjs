# LCD CATCH stable digit experiment

The sole product change is `apps/lcdcatch/lcd_catch.js::paint`: preserve the 21 desired digit bits when the numeric display value is unchanged. The first five status bits are still reset each frame. The lamp test fills all bits and invalidates with `undefined`. The cache describes the desired array, not committed native state; failed `V.patch` keeps it valid and the existing committed-state reset forces a complete retry. Numeric `-1` is a valid saved HI and cannot be an invalid key.

Product source is the verified shallow commit [d09525422d9f404f84e84f022daec95597e12018](https://github.com/dj-oyu/cardputer-adv-pocketjs/commit/d09525422d9f404f84e84f022daec95597e12018), blob `91f0ead4b459de83a142a1767e238abf312c5c73`, directly on main `e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267`. No PR48/49 prepared-call or app-control integration is included. No LCD transport, procedural buffers, frame/cache lifetime, guest/stack budget or shared frozen reference is changed.

## Native host result

GCC 14.2.0, O2, `-ffp-contract=off`, 64-bit Linux; same binary for both JS inputs, fresh process/context per trial, alternating AB/BA, two discarded pairs then 40 pairs, 40 warm frames and 120 measured frames. The timer surrounds cached `JS_Call(frame)` through the actual native input/Kasane paths. Input pumping, bot decisions, pending jobs, state probes, full panel hashing and native raster/present are outside that interval; present is measured separately. No timed logging, forced per-frame GC or heap traversal. One full GC before the window and automatic GC remain. This host allocator is the existing libc-backed TLSF-charge model, not production GUEST_ALLOCATOR.

| Workload | baseline/candidate JS µs per frame | paired ratio; bootstrap 95% |
| --- | --- | --- |
| no movement (84 play/36 miss frames) | 11.052 / 7.702 | .68294; [.65698,.75719] |
| alternating native A/D input | 18.067 / 13.421 | .76833; [.71950,.82901] |
| native bot catching stars (120 play frames) | 11.241 / 7.712 | .67121; [.64953,.69496] |
| title | 10.079 / 6.519 | .65585; [.63623,.69149] |

Absolute times are medians of trial means; ratios are medians of paired totals, so they need not equal ratios of those time columns. Thread CPU ratios also exclude 1 (see JSON). The independent first 40-pair run without bot also reproduced the reduction. These are JS-call measurements, not whole host wall time, firmware FPS or device results. Native raster/present, audio/storage stubs and UI scheduler differences prevent that generalization.

Successful allocation counts/charged bytes are unchanged. Host retained heap and frame/window live peaks increase 168 B, JS bytecode 46 B, compile peak increment 196 B. The per-frame peak *rise* is unchanged. Raw results and all memory fields are emitted by the script and retained as CI artifacts. These are charged allocator bytes, not measured ESP32 heap savings.

## Compatibility and causality

`verify.py` reuses the existing real game/input/native renderer harness: 2918 complete panels, GAME A/B, pause, game over, saved HI, reload and Back are byte/log equivalent. One 54-frame boundary table covers saved -1/INT_MIN HI after lamp, title switching, score 0/9/10/99/100/999/reset, explicit lamp invalidation and an actual transaction abort after its callback followed by same-value retry. An old -1-sentinel mutant must produce different panels and is rejected. External LCD test environment variables are cleared. Full panel bytes are compared directly, not just procedural content or hashes.

A separate untimed audit instruments only the original digit loop. The no-movement/input/title 120-frame windows execute it 120 times in baseline and zero times in candidate; pixels still match. It does not supply speed figures. The bot timing additionally verifies every mode/score sample and final composed panel hash, so useful catches do not erase the benefit. Independent read-only review found no blocker in product semantics, timer boundaries, negative test or finite device diagnostic.

Reproduce from the branch root:

```sh
python tools/experiments/lcdcatch-stable-digits/verify.py
python tools/experiments/lcdcatch-stable-digits/audit.py
python tools/experiments/lcdcatch-stable-digits/bench.py --pairs 40
python tools/experiments/lcdcatch-stable-digits/test_device.py
# Requires gcc-multilib (not installed in the saved local environment):
python tools/experiments/lcdcatch-stable-digits/verify.py --m32
python tools/experiments/lcdcatch-stable-digits/bench.py --m32 --pairs 40
```

## Device handoff (not hardware tested)

The dedicated workflow builds clean product and diagnostic baseline/candidate separately with ESP-IDF 6.0.1 and the same product partition/security/size optimization/INFO boot configuration. Product builds contain no instrumentation. `prepare_device.py` only patches a separate clean checkout, exports exact source and patch hashes and never opens USB. Artifact `experiment.json`, SHA256SUMS, ELF/map, sdkconfig, partition image and `flasher_args.json` identify the exact build. Use actual offsets from that file: bootloader 0x0, partition 0x8000, application 0x10000; product factory/dict/font/storage partition layout is retained. No app-control PR47 is included.

Diagnostic workload `lcdcatch-natural-120-v1` auto-starts LCD CATCH through normal `begin_run`, production GUEST_ALLOCATOR, app_tick, owner/scheduler and LCD. It fixes seed 20260929, empty physical input and seeds one native GAME A press after 32 frames, warms 40 frames and measures 120 real calls (84 play/36 miss, score zero). Native key query functions remain the product ones. Storage get/set are Promise stubs, with no persistent storage writes; audio remains the product path. There is no synchronous setup loop or altered watchdog/guard/budget.

The JS interval includes constant marker-entry/argument overhead and excludes native present. Separate owner tick totals include the product tick/present path. Allocation metrics are production allocator request counts/requested bytes, not success counts or charged bytes. Internal free memory is sampled at frame boundaries before the timer and after measurement, not a transient peak measurement. The 120 unsigned samples occupy 480 B static RAM in both diagnostic variants. Exact median is printed as `median_x2`, p95 uses nearest rank; no approximate quantile or raw frame stream is used. About 193 frames (~6.5 s at 30 Hz), deadline 24 s, fewer than 16 summary/app lines: fits the 30 s / 64 KiB receive / 256 lines / 32 KiB retention broker limits. Application INFO boot must show `Application information:` and matching ELF/version before accepting results.

Accept only full source head in START, `pointer_bytes=4`, frames=120/play=84/miss=36, END `status=complete complete:true`, then EXIT `running=0`. Any timeout/app_error/input_mismatch/present_pending invalidates the measurement. `test_device.py` builds the exact injected native marker and workload and verifies this state sequence and final A/B panel/log equality on host; it does not validate device timing or the production scheduler. Hardware capture/flash remains a separate task.


## Current-main comparison (2026-10-25)

The active comparison baseline is `6db9244d585fa3caad899853ff8c7fbe96c5d7d3` (merged PR65/68/69),
not the historical e0fe98a8 baseline reported above. The candidate keeps that
main product and adds only this PR's original application change. Prior numerical
results remain historical; they do not establish the current-main device effect.
The existing dedicated CI now checks both arms against this pinned baseline.

Reuse the finite workload `lcdcatch-natural-120-v1` unchanged: 120 measured frames,
production guest allocator, scheduler/owner and LCD path, 24-second cooperative
deadline, normal product INFO/partition/security settings and existing guards.
No synchronous model preparation, budget bypass, persistent storage writes or
new normal-firmware measurement hook is introduced.

Device comparison: verify source/ELF/manifest and complete retained logs for each
boot; use one A1/B1/B2/A2 block with fresh boot contexts and identical input.
Compare summed `owner_tick_us` over the complete matched 120-frame windows.
Keep each boot's JS median and p95 separate. `window_us` includes pacing and is
not an FPS measurement; boundary-free samples are not exact transient peak heap.
Accept a runtime improvement only when both paired directions agree and the
owner reduction exceeds observed same-arm variability, with matching semantic
markers and no new error, continuation, timeout or unexplained memory regression.
Otherwise hold the candidate and inspect the discrepancy; do not reinterpret
JS-only gains as whole-device benefits or add retries merely to obtain a win.
Existing host panel/state/boundary oracles remain the output-equivalence evidence.
No device result or merge is claimed by this preparation.
