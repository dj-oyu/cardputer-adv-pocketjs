# Decorative ray cost estimate (2026-09-09)

This is a workload estimate, not an ESP32-S3 frame-time measurement. Rendering
behaviour was not changed for this measurement. Reproduce the host comparison
with `python tools/bench_decor_cost.py` (MSYS2 UCRT64 GCC on this workstation).

The prior broad-volume variant is reconstructed from the current source with
the preceding width, protection and fixed-colour settings. It is not a saved
firmware binary. Each comparison runs inside one process over the same 256
moments spanning 128 seconds, with a constant input row and no LCD transfer.

| Per-frame work (mean) | Before widening/colour following | Wide rays | Wide rays + events |
| --- | ---: | ---: | ---: |
| Candidate pixel visits | 15,679 | 21,868 | 21,123 |
| Pixels evaluating profiles | 14,489 | 18,022 | 17,437 |
| RGB565 composites | 9,972 | 13,539 | 13,160 |

Counts include overlapping rays separately. The busiest sampled frame by blend
count performs 37,395 visits, 33,041 profile evaluations and 20,598 composites.
The reveal skips unreached rows, reducing average composites by about 2.8%.

Before adding events, the longer host trials measured 0.170..0.181 ms for the
prior variant and 0.223..0.246 ms for the wide variant: paired increases of
29..45%. Absolute host times varied substantially with workstation load.
Event-on / event-off ratios in subsequent trials were 0.85, 1.06 and 0.98.
That spread does not support a precise event overhead or a claimed speedup.

ESP32-S3 object disassembly confirms a scalar auxiliary pass: the blend path
alone is about 48 instructions, in addition to two profiles, protection and
loop work. Two variable divisions are per active ray-row, not per pixel.
Using measured path counts, roughly 1.5..2 million scalar instructions/frame
is a useful first-order budget for wide rays, with branch/load/cache costs
additional. At 240 MHz, allow roughly 6..12 ms/frame for the whole auxiliary
pass, and roughly 2..4 ms for the latest widening/colour change. These are
engineering estimates with substantial uncertainty, not calibrated timings.
The new events should be a smaller contribution than the widening; they add
row decisions and a conditional narrow-fragment operation, partly offset by
skipped reveal rows. A sub-millisecond event budget is a hypothesis to measure,
not a validated upper bound.

## Measured on the device (2026-09-15)

The 6..12 ms band above is an estimate built from instruction counts. It now has a
measurement: `decor` -- `garden_row_blend`'s row time minus the vector pixel pass,
the one number in SPLIT that had never been split -- is bracketed into its two
jobs by `garden_prof_vegetation()` / `garden_prof_rays()` and reported as SPLIT3
(`main/scene/garden.c`, `main/scene/flower.c`). 36 sixty-frame windows on the
Cardputer ADV (`vm-L1-60-g8779e3e` + the brackets), species 1 / 10 / 11 / 12,
log `\.cache/hosttests/logs/flower-instrument-20260915.log`, re-read with
`python tools/flower_instrument_summary.py <log>`:

| term | measured (ms/frame) | cycles/row | instruction-count floor |
| --- | ---: | ---: | ---: |
| rays (the auxiliary pass) | 7.6 .. 13.7, typical ~10 | 13,450 .. 24,300 | 3.9 |
| vegetation (canopy + trunks + grass) | 5.0 .. 5.9 | 8,900 .. 10,460 | 2.4 |
| rest (row scaffolding, dissolve memcpy + mix) | 0.24 .. 0.36 | -- | not counted |

Three things follow.

1. **The rays are the largest single term in `decor`, and the estimate holds.**
   7.6..13.7 measured against a 6..12 estimate: the auxiliary pass was the piece
   to instrument, and it was measured, not guessed, at the top of its band.
2. **The two bracketed jobs are each about 2.4x their instruction-count floor.**
   That is where the 8..10 ms of unattributed `decor` went -- not into a fourth
   job and not into the row scaffolding, which is 0.24 ms. A 1-cycle-per-
   instruction count on this scalar row code underestimates by ~2.4x (branch,
   load and cache costs), and that factor is now measured rather than assumed.
3. **A cross-fade is visible in the same report.** During a dissolve the
   vegetation pass runs twice, which the counters show directly: rows dissolving
   105 of 135 gave veg 9.61 ms with 240 passes and rest 3.37 ms, against 5.7 ms /
   135 passes / 0.26 ms for the settled windows. A bare cycles-per-row average
   would have been the mean of two different amounts of work.


The existing `SPLIT decor` is garden minus pixels and includes vegetation as
well as these rays, so it must not be read as isolated auxiliary-ray timing.
SPLIT3 (above) is what isolates them: `rays=` is a bracket around
`garden_decor_row` alone, and `veg=` around the vegetation pass; `rest` is what
neither bracket covers. Host ratios must not be scaled from the PIE main-background
time: the host uses scalar code where the device uses SIMD.
