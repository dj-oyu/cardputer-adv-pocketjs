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

The existing `SPLIT decor` is garden minus pixels and includes vegetation as
well as these rays. It must not be reported as isolated auxiliary-ray timing.
Device cycle-counter measurements around the auxiliary pass, inside one build,
are needed for a reliable number. Host ratios must not be scaled from the PIE
main-background time: the host uses scalar code where the device uses SIMD.
