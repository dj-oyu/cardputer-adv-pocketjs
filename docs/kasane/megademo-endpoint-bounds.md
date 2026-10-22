# MEGADEMO endpoint rectangle reuse

Hypothesis: at zoom 0 and 44, returning the existing private FULL/MON arrays avoids the map closure, four floating point interpolation calculations, and a transient rectangle. Product change: two lines in boundsAt only. Baseline is main e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267; independent of all other optimization PRs. Shallow candidate: 23cf62ce81943bb3b0af80070901d28ba3cec0b9.

parse_rect_mode reads four JS elements into int16_t locals then copies them to ksn_rect. image construction and change_ref/setRect use that value; neither retains nor mutates the input array. FULL/MON have no writes in the app and boundsAt is private. Interpolation and out-of-range inputs retain the old calculation. No buffer/cache/frame lifetime, partition, security, guest retention or stack policy changes.

## Host evidence

Real QuickJS, native pocket.kasane, grid/pixel/procedural rendering, all panels after each app frame. The host uses a 64-bit TLSF size model and a mocked internal-memory gate, not product GUEST_ALLOCATOR, scheduler or LCD. JS_Call(frame) is timed directly; JS_Eval probes and end-turn/present are outside it. Registration wrappers only keep loader counts; no per-draw JS timing wrapper. Pair 0 captures panels and is excluded from timing.

1136 frames with both zoom directions, tier down/up and scene skips: all 1137 panels (73,677,600 bytes), loader state and app behavior equal. Panel SHA256 f640cc00c72b7e44406dab50d2fd070ec9bee68e675cae0aa09b3ce3e2c1da7d. Frozen FULL/MON pass the same real app path and produce identical panels; deliberate mutation is rejected. The probe compares Object.is coordinates at both endpoints, every integer zoom step, neighboring values, -0, NaN and infinities. ASan/UBSan passed this 1136-frame comparison and negative control.

Allocation deltas per ordinary JS frame: 1036 endpoint frames -13 successful allocations / -648 modeled bytes, 14 view-construction frames -26 / -1296, 86 interpolation frames unchanged. Retained and whole-run peak heap +24 B; evaluation compilation peak +160 B. This is a code-size cost, no added long-lived object. These sizes are host-model measurements, not device guarantees.

Paired candidate/base median ratios with bootstrap 95% intervals:

| Run / controls workload | JS call total | owner + present total | process wall |
|---|---|---|---|
| 9 pairs, fixture GC each frame | .9772 [.9610,.9834] | .9878 [.9728,1.0029] | .9829 [.9779,1.0045] |
| 15 pairs, automatic GC | .9757 [.9614,.9875] | .9944 [.9807,1.0144] | .9949 [.9869,1.0158] |
| 15 pairs, automatic GC, obsolete timing probes removed | .9720 [.9442,.9781] | .9954 [.9868,.9965] | .9938 [.9831,1.0017] |

The natural 736-frame loop's final JS ratio was .9919 [.9658,1.0012], down from an initial apparent ~5% gain. Whole-run benefit is not established consistently; do not turn the allocation result or one interval into an FPS claim. Earlier harnesses queried removed timing fields outside the measured frame call; final harness removes these unused probes. Original generated harnesses and raw results remain separate.

Reproduce:

    python3 tools/kasane_contract/run_megademo_bounds.py --sanitize --verify-only --out .cache/new-asan
    BOUNDS_AUTO_GC=1 python3 tools/kasane_contract/run_megademo_bounds.py --pairs 15 --out .cache/new-timing

Use fresh output directories and do not run other benchmarks concurrently.

## Product diagnostic

The candidate-specific workflow produces baseline/candidate ordinary product images and diagnostic images. The shared diagnostic templates are frozen at 5bf332368d6e793cd40a8a18a48b4da4ac270622; this PR does not change that shared reference or its files.

Workload megademo-app-twist-120-v1: 48 natural NEWS frames, one per normal owner callback, then 120 ordinary TWIST frames 0..119 at initial HEAVY, zoom 0. Product memory gating, loader, GUEST_ALLOCATOR, scheduler, owner and LCD remain active. No synchronous fast-forward or guard relaxation. The marker temporarily uses the existing lazy pocket.derby namespace equally in both diagnostic images; no DERBY simulation/math is exercised. Native brackets collect wall time and allocation request counts (including failed requests), distinct from successful host allocations. Owner tick/window and internal free heap are also reported.

24-second deadline takes precedence over completion; final pending presentation is reported explicitly. START, 4 summary lines, EXIT and capped app logs use MEGA_APP_DIAG, not the existing DERBY_FIXED fixture. Boot INFO, product partition and security settings are checked in CI. Product defaults keep comparison COURSE_CACHE/PROJECT_BATCH off.

The real-host diagnostic check compares 168 callbacks / 169 panels of the wrapper with ordinary app execution and checks exactly 120 marker pairs and last index119. Six owner cases cover completion, exact-deadline timeout, incomplete, app error and pending presentation.

    python3 tools/kasane_contract/test_megademo_bounds_diag.py --common /path/to/frozen/tools/games/prepare_derby_app_diag.py --out .cache/new-diag
    python3 tools/kasane_contract/prepare_megademo_bounds_diag.py --common /path/to/frozen/tools/games/prepare_derby_app_diag.py --checkout /separate/clean/checkout

No physical device measurements yet. Parent queues any hardware use; this agent never opens USB or changes the broker.
