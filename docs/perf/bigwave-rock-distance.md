# BIG WAVE rock distance guard experiment

The only product edit is the rock callback in `apps/bigwave/big_wave.js::draws`. Apply the same `r.x-S.cx < C.dn || > far` rejection as `proj` before evaluating pure face/curl/height geometry. In-range rocks keep their original order and arguments. Other riders still execute their stateful `rider()` path unchanged. Product commit2f888e0c516244fcfc37ab09fce85c9fc6042ff1, verified blobd8af056b24cc7bbf72297c3e4e372536ad4906a9, directly on main e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267. No PR48/49 or42/44 integration, native/LCD/workspace/cache lifetime or guest/stack budget edit.

Prior64-bit GCC14 O2 native host40 AB/BA pairs twice, 120frames/trial: ride ratios0.96783 [0.96103,0.99877] and0.95762 [0.92774,0.99621]. Same binary6e647d2686768e1256e5c6992c675ccf6f1404aa5b822344800616f28296a325; CPU direction also agrees. Title/pause controls include1. Mean ride successful allocations-6.875/chargedbytes-259 per frame, retained+140B, compile/whole peak+344B. These initial runs did not set a QuickJS memory limit. They are provisional JS_Call(frame) timings, not whole host/FPS/device benefit.

On resumption the explicit163840B product limit fails even baseline source evaluation on64-bit host. Do not interpret that as candidate regression or enlarge the firmware budget. Current native host comparison explicitly uses524288B/initial GC clamp262144 (larger-word host validation); independent m32 CI uses163840B/initial GC clamp81920. The engine may update its GC threshold after collections. Record both scopes, and require actual m32/production-allocator device evidence before a firmware claim.

Each timing trial uses a fresh process/context, common O2/ffp-contract=off binary, alternating order, two discarded pairs, 40 warm then120 measured actual JS_Call(frame) calls. Bot decisions, input pumping, pending jobs, renderer/panelhash and state probes are outside the timer. Thread CPU and wall time are captured, no timed logs/heap traversal/per-frame forced GC; one full GC before window and automatic GC. Allocator counters use the existing host libc/TLSF charge model, not the production allocator. Raw samples remain CI artifacts. Neither timing nor functional tests instrument the product for a benchmark fastpath.

Functional verification reuses the complete real key/bot game scenario:7279 composed panels, native begin/draw/commit records,51events/29checks including pause/tier/wipe/retry/set-clear/Back agree. `boundary.mjs` extracts the actual product rock callback and proj with pure geometry fixtures, and compares3tiers×9depth values: near/far below/equal/above, NaN and infinities. Strict-boundary mutant is rejected. This table proves rejection inequalities; it is not a native panel test. Full native replay supplies the composed-byte evidence. Geometry helpers are pure and `s===S` in the real app. External getters/mutation/OOM sequence equivalence is outside these internal numeric invariants.

```sh
node tools/experiments/bigwave-rock-distance/boundary.mjs
python tools/experiments/bigwave-rock-distance/verify.py
python tools/experiments/bigwave-rock-distance/bench.py --pairs 40
# CI installs gcc-multilib; local saved environment lacks32-bit libc headers.
python tools/experiments/bigwave-rock-distance/verify.py --m32
python tools/experiments/bigwave-rock-distance/bench.py --m32 --pairs 40
```

Independent read-only review found no product/boundary/timer/finite-diagnostic blocker. First independent CI GCC13 native40pair ride120.969→116.047us, ratio0.96378 [0.95664,0.96769]; m32 at163840B178.037→170.642us, ratio0.95912 [0.95119,0.96486]. Pause controls include1, title also improves on both ABIs. Full7279 native panels match on both ABIs with explicit caps. m32 retained+132B (nativeCI+144B; local+140B); successfulalloc-6.875 and m32chargedbytes-166.17/frame. Final exact-head CI must be checked separately. Initial run39876685268/head8e4ceac45b052c52858a04f4209d8a021db3d7d0 supplies this result.

## Finite device diagnostic (not hardware tested)

`prepare_device.py` only patches a separate clean checkout and emits exact source/workload/patch hashes; no USB access. The dedicated workflow builds product and diagnostic baseline/candidate separately with ESP-IDF6.0.1, product partition/security/INFO/size settings. Source main e0 and candidate head remain independent of PR42/44 and PR48/49. Product builds have no instrumentation. ELF/map, sdkconfig, bootloader/partition/app images, flasher_args.json, experiment.json and SHA256SUMS identify each artifact. Use the artifact offsets0x0/0x8000/0x10000 and existing factory/dict/font/storage layout; no app-control PR47.

Workload `bigwave-rock-ride-120-v1` starts automatically via normal begin_run/app_tick/owner/scheduler/LCD/GUEST_ALLOCATOR. There are40title frames, one native start press, one empty frame and40bot warm frames, then120ride frames. The existing read-only host bot logic runs before the JS timer; its native key state/edge masks feed unchanged product key queries. No state/physics acceleration or synthetic benchmark fastpath. The full product source is captured before the wrapper. No synchronous setup loop or increased watchdog/guest/stack budget. The app does not call storage; diagnostic adds no storage/random hooks or persistent writes.

The host fixture compiles the exact injected native marker/workload: ordinary real-FIFO bot, diagnostic baseline and diagnostic candidate each produce203 byte-identical full panels and final state1/x50.909642993 with zero exception/present/oracle failures. m32 repeats this check at the product cap in final CI. Host geometry fixtures/full replay are separate checks.

120unsigned samples occupy480B in each diagnostic. Median_x2 is the exact twice-median; p95 uses nearest rank, no approximate quantiles/raw stream. Expected about202owner frames (~6.7s at30Hz), cooperative deadline24s; seven diagnostic lines plus capped app logs fit existing30s/64KiB receive/256lines32KiB retention. No broker/guard changes. Timed JS includes constant marker argument/entry overhead; excludes bot and native present. Owner tick totals include bot/normal tick/present and cannot be treated as uninstrumented FPS. Allocator counts/requestedbytes are requests, not successful/charged host bytes. Internal free memory is sampled before timer at frame boundaries/end, not a transient peak heap statistic.

Accept only INFO `Application information:` with expected source-derived version/ELF, fullhead START, pointer_bytes=4, frames120/ride120/other0, tier1 active path, END status=complete complete:true, then EXIT running=0. Host x progresses12.983918476→50.909642993; device values/timing remain unmeasured. timeout/app_error/input_mismatch/present_pending invalidates the run. Hardware/USB/flash/merge has not been performed.

```sh
python tools/experiments/bigwave-rock-distance/test_device.py
python tools/experiments/bigwave-rock-distance/test_device.py --m32
# Only a separate clean checkout:
python tools/experiments/bigwave-rock-distance/prepare_device.py --checkout /path/to/disposable-checkout
```
