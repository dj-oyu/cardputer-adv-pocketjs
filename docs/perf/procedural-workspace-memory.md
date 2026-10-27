# Procedural image workspace reuse

This extracts the workspace unit from #37/#44 onto normal main
`45a06e09606fec234b5c7241a004bbd9cce0e4e2`. Product source:
`4e4d7bccd6ed461d4b17072db118b453ce62722b`; final historical #44 review:
`687b90319577df36b74c16a4f5ee0d351801a463`. #44 merged into the diagnostic
integration, not normal main. Its product file equals that source commit.

The adapter had a static 3,840-byte eight-row RGB565 cache beside its allocated
10,246-byte VM scratch frame. Synchronous owner-task phases can share that
allocation: execution writes the frame, copies its segments into the surface
candidate, and returns before rasterization. Rasterization reads candidate or
committed frames, never the workspace frame. Image spans copy their pixels into
the caller's output; no workspace pointer is passed to transport.

The workspace is a union with the same allocation size and full frame capacity,
enforced by a static assertion. Candidate/committed storage, surface caps,
eight-row dimensions and cache keys, presentation/repair ownership and reset
lifetime remain intact. A blank image before allocation or after reset fills
the requested span without allocating. A raster failure leaves the cache invalid.

Current main has a runner-affine writer as well as the VM. Invalidation occurs
before either writer, including failed or subsequently abandoned draws. A local
frame alias preserves that branch and its counters. The existing span-profiler
finish also runs for allocation-free blank spans; band counters count actual
band raster work. No measurement hooks from the diagnostic integration are
imported.

## Verification

Existing real-QuickJS limits/turn tests run scalar and fake PIE with ASan/UBSan.
They retain four first-frame allocations, a 10,246-byte workspace, the exact
surface segment limits and reset/unregister checks. Image contracts cover
48 complete procedural RGB565 rasters, both image surfaces, repeated/partial spans, damage,
IO retry and stale resources. Failed and abandoned writes must leave subsequent
committed-image reads correct. The ROM/array snapshot keeps checking every VM
field and segment through the final #44 workspace alias correction.

A small case in that existing ROM fixture checks cached committed rows before
and after abandoning an affine draw and a VM fallback, with the original affine
counts. The standard affine full-pipeline test still covers LIGHT/MID/HEAVY.
The capture helper now recognizes the application's numeric draw arguments;
its earlier array-only anchor otherwise stopped fixture generation on main.
Only the temporary captured app is instrumented.

Local image A/B has identical hashes for all 48 240x135 procedural rasters;
these hashes are not a whole-application LCD comparison. Limits/turn scalar
and fake PIE and the full affine pipeline pass. Local ROM fixtures are captured
through the real native 64-bit game; CI independently uses the existing m32
capture. Native host results are not device results.
Removing only the pre-write invalidation fails the new committed-row pixel
assertion in the affine case, confirming the regression check detects stale
workspace pixels.

Fresh local Xtensa objects use the retained ESP-IDF 6.0.1 compile flags, current
source/headers and regenerated normal main sdkconfig (SHA256
`9fea08cfca669fe6e033e1ceb9281c7e1f89f49536a4419393bbc4108182a04b`):

| Configuration | Baseline BSS | Candidate BSS | Delta |
| --- | ---: | ---: | ---: |
| Normal default | 4,343 B | 503 B | -3,840 B |
| Runner affine ON | 4,363 B | 523 B | -3,840 B |

This reproduces the old static saving on current main. It is an object result,
not a local full-image link, guest-heap saving, largest-block measurement or
device timing result. CI builds the ordinary product and repeats both object
comparisons using that build's exact compiler command and SDK configuration.
Physical resource, native UI and copied-DMA composition acceptance remain
separate. No guest cap, stack budget, resource threshold or product default is
relaxed.
