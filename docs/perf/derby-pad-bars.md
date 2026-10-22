# Update paddock bars when the selection changes

Base: main e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267. The production change is
the independent shallow candidate 1c795821fe7dbc5641bac260c9eaf1c383a67b5e.
Only the selection rectangle and three horse-stat bars are guarded by R.pick.
Odds, titles, closures, rendering and native memory ownership are unchanged.

Each new scene creates a new R array. Its absent pick property forces the first
HUD update, including when the selected horse is zero. Horse stats are immutable
within the paddock: F is replaced in enter('pad'), which sets need=1 and rebuilds
R. Delayed storage, demo exit and next race use the same path. Pick/stake/tier
input keeps its existing behavior. Native setRect copies the numeric bounds.
If an update throws, frame rethrows and product tick_run ends the app, so the
partly updated JS cache is not reused in a later frame. Future in-place changes
to horse stats would require a scene rebuild or explicit invalidation.

Reproduce with Node and GCC on PATH:

```sh
python3 -B tools/games/run_derby_pad_bars.py --pairs 15
python3 -B tools/games/run_derby_pad_bars.py --sanitize --verify-only --out .cache/bars-asan
```

One unchanged O2 native binary runs both app variants. The real app's MID
lifecycle (14,563 frames: demos, input, replay, next race and Back) matches
procedural pixels, final displayed panels, frame CSV and finish/order/times.
A second lifecycle delays storage for 20 frames, cycles selection 18 times,
and rebuilds at lower/upper clamp and rounding boundaries. A test-only observer
checks every paddock frame's bounds accepted by the native rectangle methods;
it is disabled for timing and allocation samples. Mutating horse.top in place
without a rebuild is a negative control: baseline repairs the bar, while the
candidate is correctly rejected by the observer. O2 and ASan/UBSan pass all
positive cases and detect this negative case.

| Script | Procedural pixel hash | Final-panel stream hash | Frame CSV SHA256 |
| --- | --- | --- | --- |
| Standard | 789dd8aa345ffcf3 | adb43d6c16ea5f94 | f80da9e732cdc19f27a2e38d34f97ba07bac9d63d831e240b43bdb3bec5b0695 |
| Delayed storage / boundaries | b40983ab7e68c4ca | 6e9ffb2cd7053be1 | b555b5f095f51b45a24756282585c1e63abd2fbec902e056a3d3ebdc29d88940 |

Fifteen alternating pairs, each 50 warmup +300 sampled paddock frames:

| Metric | Baseline | Candidate |
| --- | ---: | ---: |
| Allocator entries per frame | 222 | 207 |
| Requested bytes per frame | 12,360 | 11,616 |
| Median of per-run median frame times | 225.665us | 219.423us |
| Max per-turn heap rise | 2,972B | 2,972B |
| After-eval / eval peak / run peak | 157,656 / 171,596 / 177,524B | 157,696 / 171,636 / 177,564B |

Paired median time ratio 0.977264, paired bootstrap 95% [0.965508,0.991337].
The deep measurement is about 2.3%, smaller than the shallow 8.9% signal.
The timing scopes differ: this runner includes native draw validation inside
frame(); neither is product FPS. Raw tails are retained. The cost is about
40B more retained/peak heap in the 64-bit host TLSF length model, in exchange
for 15 fewer allocation requests /744B requested per steady paddock frame.
ESP physical heap and device latency are unmeasured; no budget is increased.

Dedicated CI produces ordinary product images and paired finite pad diagnostic
images. The wrapper reuses PR50's exact common generator
1e68b268a4b91ec54e88f408118de3e39b8b6bc5 and its unchanged
derby-app-pad-120-v1 workload: product GUEST_ALLOCATOR, UI owner, scheduler and
LCD; no storage writes; 50 warmup +120 sampled frames. It also applies PR54's
deadline-ordering correction to the generated main.c, equally on both sides:
even a finished frame at/after the 24-second deadline is timeout. The compiled
owner-callback boundary test runs in CI. Generated patch and hashes are saved.
No shared generator, frozen reference or existing workflow is changed.

Product INFO/security/partition settings are verified. The parent owns device
observation through the existing broker, bounded to 30 seconds/32KiB/256 lines.
Require matching source/workload START, END status=complete frames=120 and EXIT
running=0; reject timeout/app_error/present_pending. Compare guest_requests,
requested_bytes, frame_wall_us and owner_tick_us on alternating paired runs.
The JS diagnostic marker adds common overhead; boundary_free_min is not a
transient heap peak. This is not the broker's old fixed-fixture parser.
Hardware operations remain paused while the parent resolves approval.
