# Finite real-app DERBY diagnostic

PR50's original product images had no automatic workload launch. The broker's
`oval-pan8-390-510-v1`/120-context fixture is a different experiment and must not
accept these images. PR47's native app control also needs separate owner,
transport and broker changes; none are imported here.

This opt-in generator writes **only into a separate clean checkout**. Normal
product builds do not include it. The generated image runs once at boot using
`begin_run(local.derby)` and the existing UI owner's `app_tick`, guest scheduler,
GUEST_ALLOCATOR and LCD presentation. No new task, serial command, broker API,
renderer buffer/cache or allocator policy is added. The allocator counters
forward to the original callbacks. Product heap and stack limits, partition,
security and INFO startup settings are unchanged. The extra JS wrapper/native
counters do occupy some diagnostic memory and time on both variants.

Reproduce without hardware:

```sh
git worktree add --detach /tmp/derby-diag <exact-source-sha>
python3 tools/games/prepare_derby_app_diag.py --checkout /tmp/derby-diag --profile oval
cd /tmp/derby-diag
git submodule update --init --recursive
idf.py -B build_diag -D SDKCONFIG=build_diag/sdkconfig build
```

The dedicated PR50 workflow prepares baseline e0fe98a8 and candidate with the
same generator, ESP-IDF v6.0.1 and options. Artifacts contain the generated patch,
source SHA, workload ID, per-file SHA256, BIN/ELF/map, sdkconfig and flash inputs.

Workloads `derby-app-oval-120-v1` and `derby-app-straight-120-v1` use hardware seed
0x2545f491, race 1, forced course, HEAVY tier and fixed camera 8. Storage reads
return no saved state and writes are no-ops; audio is silent. Initial gate frames
load the real app plans. After at least 40 warmup frames and an empty load queue,
the actual model advances unmeasured until lead >=390m. Ten further app frames
settle the race scene and band plan; 120 subsequent real frame calls are timed.
The last leader is approximately 497.35m oval / 497.79m straight. These are
selected race windows, **not the earlier full-race host workload nor normal-game
steady FPS**. Profile `pad` is available for subsequent app-HUD experiments:
MID tier, same seed, 50 warmup frames and 120 measured paddock frames.

At boot, `DERBY_APP_DIAG START` identifies source/workload/allocator. The existing
owner continues normal frame pacing. A 24-second deadline (cooperatively checked
between owner turns) ends an incomplete run as `timeout`, never a successful
measurement. Success requires `END status=complete frames=120`, then `EXIT`.
Guest console output is capped at 16 calls; diagnostic output is six short lines.
Collect at most 30 seconds / 32KiB / 256 lines through the **existing broker's
bounded observation/log retrieval**; parent owns all hardware actions. Do not
select the old `derby_fixed` parser, spoof its markers, or relax its guards.
A missing END/EXIT, app exception, load failure or timeout is a failed/incomplete
run, not evidence of performance. No hardware action is performed by these tools.

Reported metrics:

- `views`, `ou`, `sqrt`: actual native calls within the measured JS frames.
- `frame_wall_us`: entry-to-return of the original app frame; includes any
  scheduler preemption/continuation gaps. Timer excludes diagnostic heap sampling.
- `window_us`: first frame entry to last frame return, includes owner pacing but
  ends before the last presentation. Do not call it completed-display FPS.
- `owner_tick_us`: summed wall time of owner `tick_run` calls during the window,
  including normal app_tick and LCD work. It is not exclusive CPU cycles.
- `guest_requests`, `realloc_requests`, `requested_bytes`: allocator entry requests
  made by the original frame; calloc is one request, realloc is separate, and
  failures are counted. Bytes are requested sizes, not physical TLSF occupancy.
- `boundary_free_min`: internal free heap sampled before frames, not transient
  peak usage. `free_end` is sampled before teardown.

`test_derby_app_diag.py` executes the exact diagnostic JS through the existing
real QuickJS/native-renderer host harness. It checks termination and A/B equality
of draw bytes and pixels for all three profiles. It does not emulate product
allocator, UI scheduling or LCD. Device execution remains required.
