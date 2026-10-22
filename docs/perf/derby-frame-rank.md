# DERBY frame-local ordering reuse

The simulation orders the runners after each step to find a lead change. The
director then repeats the same sort before selecting its camera. Keep the latest
step's ordering in a frame-local variable and reuse it for the director. A frame
without a simulation step still computes its own ordering.

This is the single production hypothesis from shallow commit
`284d6631ff7969c5f63b2729ec77226eaba9c5f6`, based on main
`e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267`. No change to the comparator,
floating-point arithmetic, native renderer, guest budgets or buffer lifetimes.

`order()` reads only `rs.tc` and `rs.x`. Between the latest `step()` and the
director, `photo()`/`at()` modify lane/projection data, not these inputs. Each
simulation iteration replaces `rank`, so a multiple-step frame uses its last
ordering. The variable starts undefined on every frame; gate and non-step slow
frames take the original fallback. Settling exits to the photo scene before this
director path. Consumers of `ro` read the array and do not mutate it.

## Host evidence

Two separate runs each use 15 alternating A/B pairs, 300 frames per process after
50 warmup frames. They run the actual lowered app and native APIs in one binary.
The timer covers the JS frame and native draw validation, before host present;
it is not device latency or FPS. Host allocation bytes use the existing
64-bit TLSF rounded-length model, not actual product allocator requests.

| Measure | First run | Second run |
| --- | --- | --- |
| race paired frame-median ratio | 0.987902 | 0.987033 |
| bootstrap 95% interval | 0.985158–0.990476 | 0.976167–0.992767 |
| pad ratio | 1.002438 | 0.998584 |
| pad 95% interval | 0.996348–1.008859 | 0.996955–1.005398 |

Race removes 2,700 allocator entries / 296,400 modeled bytes per 300 frames:
**9 entries / 988B per frame**. Pad allocations are unchanged. The shallow 2.8%
timing signal used a different scope; these deeper measurements show about 1.2–1.3%.

Cost: host retained/run-peak heap increases **88B** (after-eval 157,656→157,744B;
race peak 181,148→181,236B); eval peak increases **332B**
(171,596→171,928B). Maximum turn rise remains 2,972B pad / 3,148B race.
Physical ESP heap and stack effects remain unmeasured; no budgets are enlarged.

The MID lifecycle (14,563 frames: input, demos, replay, next race, Back) preserves
procedural pixels `789dd8aa345ffcf3`, final-panel stream `adb43d6c16ea5f94`,
frame CSV SHA256 `f80da9e732cdc19f27a2e38d34f97ba07bac9d63d831e240b43bdb3bec5b0695`
and finish order/times. An untimed observer recomputes ordering after every race
frame. A separate 300-frame boundary script forces one multiple-step and three
zero-step frames: order calls 599→302, 299 steps, final-panel
`74f8315e44143822` on both. ASan/UBSan pass these checks.

```
python3 -B tools/games/run_derby_frame_rank.py --pairs 15
python3 -B tools/games/run_derby_frame_rank.py --sanitize --verify-only --out .cache/rank-asan
```

Use different output directories when compiler flags change. Raw samples and
full lifecycle/observer logs are included in the dedicated CI host artifact.

## Finite product diagnostic

The diagnostic directly uses repaired PR50 common generator
`5bf332368d6e793cd40a8a18a48b4da4ac270622` and
`derby-app-oval-120-v2`. It includes the strict deadline guard, so the previous
wrapper/PR56 guard import is no longer needed. PR50's parent device v1 baseline
stopped inside synchronous preparation (`interrupted`, samples=0); v2 advances
at most eight model steps per normal frame callback. Product watchdog, scheduler,
pacing and 24-second deadline remain unchanged. The shared host regression
matches v1 pixel/draw bytes and endpoints; target completion remains unverified.
Do not mix v1/v2 artifacts. The product rank-reuse change is unchanged.

```
python3 -B /path/to/pr50/tools/games/prepare_derby_app_diag.py --profile oval \
  --checkout /path/to/clean-disposable-checkout
```

The firmware auto-starts the real app through the product UI owner, guest
allocator, scheduler and LCD. It uses seed `0x2545f491`, no stored state/writes,
audio off, HEAVY, oval, camera 8, unmeasured simulation advance to lead 390m,
at least 40 loading frames plus 10 settling frames, then 120 measured frames.
That is a fixed mid-race workload, not a full-race FPS test. The normal sort
sites in the app remain active despite the fixed camera.

An untimed host run of this exact generated workload confirms 120 frames,
order calls 240→120, lead 399.166859458..497.350884362, identical procedural
`2ecc984362eaf8ce` and final-panel stream `22a4bee442a13a6b`.
The diagnostic source does not include this extra order-counting observer.

The cooperative deadline is 24s, including startup. A final frame at/after the
deadline reports timeout. Accept only `END status=complete frames=120` followed
by `EXIT running=0`; owner continuations may make owner-tick count differ from
frame count. `frame_wall_us` includes scheduling across a guest frame;
`owner_tick_us` includes rendering work. `boundary_free_min` is a sampled boundary
value, not transient peak heap. Product allocator counters delegate unchanged
and report actual requests, unlike the host rounded-length model.

Use existing authorized finite log capture (30s, 32KiB, 256 lines), requiring
startup INFO and the six diagnostic summary lines. Firmware artifacts include
ELF, flash arguments, hashes, generated patch/manifest, sdkconfig and the product
partition. CI verifies INFO, security settings, partition bytes and existing
COURSE_CACHE/PROJECT_BATCH OFF settings. Ordinary product A/B artifacts are built
separately. Device operation, broker registration and MCP changes are outside
this script; no device measurements have been performed here.
