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

Independent read-only review and m32 validation are in progress. Device diagnostic CI will follow if the effect remains; no USB/hardware/flash/merge has been performed.
