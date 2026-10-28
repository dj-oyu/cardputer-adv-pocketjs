# Skip unused turf-screen projection

`paint()` computes the screen rectangle and then discards it outside a race or
in HEAD ON. Move that eligibility check before the projection. The projection
formula, clipping, rounding and screen warmup remain unchanged for eligible
views. Production source is shallow commit
`e45c983811c266997a49b19cc4dbc332bb038010`, independently based on main
`e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267`.

`pose()`/`pj()` only read geometry. `vq` is consumed only in `pan()` under `vr`;
each eligible panning paint recomputes it before use. Ineligible paint sets `vr`
to null as before, and does not increment `von`. No new retained cache, native
buffer, transport or workspace lifetime is introduced. The only product change
is `apps/derby/derby_scene.js::paint`; HUD changes are separate experiments.

## Host evidence

Two runs of 15 alternating A/B pairs, 300 pad/race frames after 50 warmup frames:

| Paired frame-median ratio | Run 1 | Run 2 |
| --- | --- | --- |
| pad | 0.983788 | 0.985560 |
| pad bootstrap 95% | 0.973122–1.001769 | 0.980849–0.990503 |
| race control | 1.000937 | 0.996882 |
| race bootstrap 95% | 0.987957–1.007558 | 0.986826–1.019264 |

Pad removes **9 allocator entries /456 TLSF-rounded modeled bytes per frame**.
The real app/native frame timer includes draw validation and excludes host
present; it is not product FPS. The shallow 6.1% timing signal uses a different
scope. This deeper signal is about 1.4–1.6%, with the first interval including1;
independent CI/device confirmation remains necessary. Race allocations match.

64-bit host cost: retained/eval heap **+8B**. Pad run peak improves
177,532→177,420B (-112B), and turn rise 2,972→2,668B (-304B).
Race run peak increases8B; its turn rise remains3,148B. No budget is increased.
Host bytes are rounded model charges, distinct from raw product allocator
requests or physical ESP heap, which remains unmeasured.

The MID14,563-frame lifecycle preserves procedural pixels789dd8aa345ffcf3,
final-panel streamadb43d6c16ea5f94, CSV and finish results. An untimed observer
evaluates the original projection at each paint and checks `vr`, visible
panning `vq` and `von`; separate forced HEAD ON entry/exit checks the skipped
branch and resumption. Both baseline/candidate panels must match.

```
python3 -B tools/games/run_derby_screen_projection.py --pairs 15
python3 -B tools/games/run_derby_screen_projection.py --sanitize --verify-only --out .cache/screen-asan
```

Use separate output directories when compiler flags change. The dedicated CI
host artifact retains raw logs, frame samples, panel hashes and CSV files.

## Paired product diagnostics

CI reuses PR56's wrapper at146128065e81bb3493985e0b1b7123ff819baaf4 and PR50's
common generator at1e68b268a4b91ec54e88f408118de3e39b8b6bc5, applied only to clean
disposable A/B checkouts. No shared generator, workflow or frozen reference is
changed. `derby-app-pad-120-v1` is unchanged: actual app/UI owner/GUEST_ALLOCATOR/
scheduler/LCD, seed0x2545f491, no storage or audio, MID, 50warmup+120sample frames.
An untimed host proof of this exact generated workload reaches the two projected
poses240→0 over120samples and preserves procedural8ca8539d58f721a5 and final-panel
stream6abc8c7a6e51609d. The extra pose counter is not in the firmware. Generated
native source matches A/B except the source-ID string.
The strict24s deadline reports timeout when the final frame finishes too late.
Accept only `END status=complete frames=120` and subsequent `EXIT running=0`.

Product firmware and diagnostic firmware are separate CI artifacts containing
ELF/BIN, bootloader/partition, flash arguments, hashes, sdkconfig and diagnostic
patch/manifest. CI checks startup INFO, product partition/security and existing
COURSE_CACHE/PROJECT_BATCH OFF settings. Use only the existing authorized
30s/32KiB/256-line log capture. Frame/owner timers and boundary heap samples have
the same limits as PR50/56; boundary free memory is not transient peak usage.
No device or MCP operation is performed by these tools.
