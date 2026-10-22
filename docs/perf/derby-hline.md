# Plain horizontal lines in mixed procedural frames

Base: main e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267. Independent of PR50/53.
The production change is eleven lines in `plain_band()`: a horizontal plain
line in a frame containing pattern lines fills its clipped inclusive interval.
It avoids the old per-pixel Bresenham tests. Both directions and a single point
produce the same pixels; the row and x bounds are checked before addressing.
Coordinates are int16 and their arithmetic fits the target's int. Nonhorizontal
walks and the separate plain-only frame path retain their existing bodies.
Buffer ownership, cache lifetime, frame structure and LCD transport are untouched.

Reproduce from this branch with Node and GCC on PATH:

```sh
python3 -B tools/games/run_derby_hline.py --pairs 9
python3 -B tools/games/run_derby_hline.py --sanitize --pairs 0 --cases mid --out .cache/hline-asan
```

The runner changes only the renderer source between binaries. It uses the real
app, QuickJS, pocket.kasane, procedural renderer and scripted keys. The source
hash, flags, raw per-frame and per-band captures are retained. Band timers are
enabled only inside the host's actual present callback: oracle redraws are
excluded. The surrounding present timer includes advance, rendering and host
strip copies. It has no real LCD, product allocator or RTOS scheduler. The
additional diagnostic frame timer includes host draw validation and is not an
exclusive production callback latency. Pair 0 captures hits/CSV/panels and is
excluded from every timing summary; nine subsequent pairs alternate order.

Confirmed host results (64-bit GCC, O2):

| Script | Frames | Actual band calls / mixed calls | Horizontal plain calls | Hit bands | Pixel hash / displayed-panel hash |
| --- | ---: | ---: | ---: | ---: | --- |
| LIGHT | 1,566 | 25,782 / 24,677 | 80,598 | 15,822 | 0b3cb74774d8396a / 8dcf0ab7b6267490 |
| HEAVY oval | 1,562 | 25,714 / 24,626 | 85,004 | 15,789 | 2a2b839df4a81c0f / 64473febba5e472e |
| HEAVY straight | 1,564 | 25,748 / 24,660 | 94,422 | 16,450 | 460dfca4ca6b9d4f / 252f125f214991ce |
| MID full lifecycle, ASan/UBSan | 14,563 | 242,203 / 235,216 | 679,963 | — | 789dd8aa345ffcf3 / adb43d6c16ea5f94 |

Both binaries match hashes, frame CSV, finish order/times and allocation
counts/bytes. The MID script covers demos, key transitions, replay, next race and
Back. Existing pattern-line tests plus eleven clipped/reversed/point/off-panel
mixed-frame cases pass O2 and ASan/UBSan. A separate exploratory old/new kernel
check passed 228,800 boundary/random cases with guard buffers and sanitizers;
the real-app results above are the adoption evidence, not that synthetic check.

| Script | Median present, base → candidate (us) | Median paired ratio (bootstrap 95%) | Aggregate band-time paired ratio |
| --- | ---: | --- | ---: |
| LIGHT | 109.808 → 105.470 | 0.96156 [0.95764, 0.96458] | 0.78786 |
| HEAVY oval | 118.092 → 114.824 | 0.96105 [0.87809, 0.97475] | 0.83955 |
| HEAVY straight | 114.623 → 111.569 | 0.96519 [0.95477, 0.97819] | 0.83995 |

These are ratios of each pair's per-frame medians; marginal column medians need
not give the same ratio. Total wall time has large scheduling outliers (including
one final straight baseline concurrent with sanitizer startup). All samples
remain in the evidence. No device FPS or whole-app throughput gain is claimed.
Allocation changes are exactly zero. Host executable text grows 128B; data/BSS
are unchanged. Device code size and performance require target artifacts/testing.

Dedicated CI builds product images and paired finite oval/straight diagnostics
using PR50's exact generator at 1e68b268a4b91ec54e88f408118de3e39b8b6bc5. It does
not stack PR50's math change. Workloads are `derby-app-oval-120-v1` and
`derby-app-straight-120-v1`: product GUEST_ALLOCATOR, app_tick, scheduler and LCD;
fixed seed/camera, state advanced to 390m before the 120 measured frames.
Product partition/security/INFO settings remain intact. `owner_tick_us` is the
relevant device aggregate, including rendering; a JS callback timer alone does
not cover this raster change. Product allocation counts should remain equal.

Parent runs the existing broker observation route, bounded to 30 seconds,
32KiB/256 lines. Require START source/workload, END status=complete with n=120,
and EXIT; reject app_error, timeout or present_pending. Use matched profiles and
alternate baseline/candidate captures. This is not the broker's old fixed
fixture. No USB operation, broker change or merge is part of this experiment.
