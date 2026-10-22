# Initialize paddock odds text with its scene

Base: main e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267, independently of PR50.
Hypothesis: paddock odds are constant until the scene is rebuilt, so formatting
and assigning eight labels in every hud() call creates avoidable short-lived
strings. The production change supplies the identical strings to tx.text during
build() and removes only that repeated loop. Capacity, bounds and formatting
remain unchanged. No other HUD optimization is included.

`od` is assigned only in enter('pad'), which first sets need=1. Initial startup,
late storage reads, demo exit and next-race entry all go through it. A new R is
constructed for the next displayed frame. Pick/stake/tier changes do not mutate
od. Native text initialization and setText both copy the supplied string; neither
retains a guest string. Formatting moves earlier inside the same synchronous
replace callback. There is no new cross-frame object cache or lifetime policy.

Reproduction:

```sh
python3 -B tools/games/run_derby_odds_initial.py --pairs 15
python3 -B tools/games/run_derby_odds_initial.py --sanitize --verify-only --out .cache/odds-asan
```

The runner uses one unchanged native O2 binary for both app sources. It runs the
existing real-app MID script (14,563 frames, demo/keys/replay/next race/Back), and
an additional run whose stored state arrives after 20 displayed paddock frames.
That run also rebuilds labels for 1.1,9.9,9.95,10,10.5,99.9,998.9,999. A test-only
observer records the actual text arguments delivered to the native text API and
checks every displayed paddock frame. A negative control mutates od[0] without
need=1: the old per-frame updater repairs the label, while the candidate is
correctly rejected by the observer. Such a mutation is absent in the real app;
if introduced later, the scene invalidation contract must be maintained.

Confirmed on 64-bit host:

| Check | Baseline and candidate |
| --- | --- |
| Standard script pixel hash | 789dd8aa345ffcf3 |
| Standard frame CSV SHA256 | f80da9e732cdc19f27a2e38d34f97ba07bac9d63d831e240b43bdb3bec5b0695 |
| Late-storage/boundary pixel hash | 0eb145dead2c87db |
| Late-storage/boundary CSV SHA256 | 52b18e34ed3eaae0c442b138527f727a60353223f760b062059b43c33937fcb2 |
| Finish/order/times | exact equality |
| ASan/UBSan full standard and lifecycle runs | pass |

Fifteen alternating pairs, each 50 warmup +300 timed paddock frames, reproduce
222→209 allocator entries and 12,360→11,788 requested bytes per frame:
**13 entries /572B removed per frame**. The paired median frame-time ratio is
0.980918; paired bootstrap 95% interval [0.976958,0.982898]. This is about 1.9%
on this instrumented host run. The shallow agent's 6–11% result used a different
harness timing scope, so those percentages must not be pooled. This runner's
native draw hook includes validation work inside frame(); LCD/scheduler are not
present. Long-tail times remain noisy.

Pad maximum per-turn heap rise stays 2,972B. After-eval, eval peak and whole-run
peak are each 84B smaller; these are a 64-bit host TLSF length model, not physical
ESP heap occupancy. The observer is disabled for timing and allocation samples.
Product allocator and device FPS remain unmeasured.

Dedicated CI runs host verification and produces paired product and finite
paddock diagnostic images. The latter reuses PR50's exact generator at
6e5fbdbcd612526df8b6600dd55aae1327205b93: `derby-app-pad-120-v1`, real product
GUEST_ALLOCATOR/app_tick/LCD, 50 warmup +120 measured frames, 24-second cooperative
deadline, no persistent storage writes. Product partition/security/INFO defaults
are retained. Parent owns bounded broker observation (30s/32KiB/256 lines);
no USB action or broker modification is performed here. It is not the broker's
old fixed fixture and must not be submitted to that parser.
