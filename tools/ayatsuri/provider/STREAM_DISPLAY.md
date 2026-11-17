# Explicit pocket/Kasane streaming diagnostic presenter

`CONFIG_AYATSURI_STREAM_DISPLAY_CANDIDATE` defaults OFF. Broker variant
`ay_stream_display` compiles the bridge and streaming API; it does not invoke
them at startup, load a model, write persistent data or register a guest actor.
The build requires `KASANE_DMA_OVERLAP`. Returning to the ordinary variant removes
the entry point. Build/flash/physical validation remain broker operations.

## Actual connection and ownership

The explicit native `pocket_stream_candidate_present` entry acquires the frame
owner after caller preflight, and passes its pinned snapshot as an opaque black
background through the **real** pocket/Kasane backdrop renderer. The existing
pocket native-source ACKs and Kasane core ACK stay in their original order.
Normal `pocket_kasane_present_complete` calls still use the ordinary backdrop.

The explicit backdrop helper rejects an existing procedural frame or a pending
proc/grid/video/pixel publication. It requires an existing runtime and invalidates
all bands for the new diagnostic frame. It is not an alternative source-selection
policy. Existing committed overlays remain under their own source contracts and
the renderer's normal validation/error handling.

Before acquiring, the bridge calls `ksn_runtime_finish_display` with the caller's
real completion context to drain earlier work. A wrapped display port preserves
the original strip/send/text contexts; every submission marks the display as
borrowed before calling the underlying driver. The real completion callback's
successful return is the drain evidence. No token, elapsed timeout, successful
send return or assumed byte count substitutes for completion.

On a drained successful presentation, the candidate snapshot is promoted after
the pocket/Kasane function returns. On a drained failure, the old READY survives.
If completion fails, both snapshots remain pinned and subsequent generation is
blocked. `pocket_stream_candidate_recover` accepts a fresh real completion context,
retries **only drain**, then rolls back the failed candidate and invalidates for
normal repair. It never replays or ACKs the failed frame. The adapter retains no
stack-local port/completion pointer. Its state, frame owner and scratch must stay
alive through this recovery. The single UI owner must not reenter, present other
work, tear down or mutate these resources while the bridge is held.

The existing copied-DMA port contract still applies: each present callback must
release/copy its input before returning. Pixel-band DMA ownership is controlled
by that driver. The bridge does not make a pointer-borrowing asynchronous port
safe, free a DMA band, or change the board queue.

## Preflight and memory

Caller preflight must validate all snapshot geometry and the candidate's complete
composition constraints before submission. The synthetic test checks all rows.
The normal Kasane preparation and source availability checks remain active, and
later provider/send errors still require drain and repair. Actual materials,
transparent layer bounds and attributed clipping are not implemented here.

The bridge allocates nothing and reuses the board's band. It requires dedicated
240-float (960-byte) depth scratch, in addition to the two snapshots and existing
geometry storage. Writable adapter/stats/depth spans are checked against frame,
instance, arena, immutable model/blob and snapshot allocations; the caller must
also keep unrelated display/provider storage disjoint. Whole-frame preflight and
serialized geometry/display ownership can cost time: none of these host counts
establishes a 50 ms frame budget or internal-RAM fit on ESP32-S3.

## Host test

Run on Linux with the existing QuickJS host prerequisites:

```
python3 tools/ayatsuri/provider/test_stream_display.py --out /tmp/stream-display-new --sanitize
```

This links actual pocket, QuickJS, Kasane runtime/render and streaming source.
It keeps the original pocket regression suite and adds public synthetic geometry
with a copied asynchronous-display simulation. Checks cover panel/raster parity,
send failure, failed final completion retaining the pin, repeated failed recovery,
successful drain followed by rollback, preflight rejection, competing pending
native publication and ordinary no-damage completion. It does not communicate
with a physical device. Windows standalone adapter compilation also checks
`-Wall -Wextra -Werror`; the linked test is the relevant ownership check.

## What still prevents a persistent restored-model actor

This is an explicitly invoked diagnostic presentation, **not a registered
persistent backdrop**. A later normal UI redraw can replace it. A model actor
needs an APP/session-bound source registration, a committed immutable snapshot
used for every redraw, pending selection during submission, and revoke/reset
rules that refuse destruction until the existing display owner confirms drain.
This bridge must not be advertised as fulfilling those lifetime requirements.

Next required integrations are a frozen control/driver/geometry/material package,
real-material admission and appearance comparison, a unified opaque/alpha frame
owner, startup-owned bounded scratch, and target allocation/stack admission.
The private 63-pose geometry result used sampled rig transforms and placeholder
materials; it is independent of this synthetic display test. Broker validation
must identify the exact enabled firmware/model, observe real DMA/ACK failures and
repair, measure internal free/largest block and task stack under load, and time
the whole target-model frame including display completion. Existing demo FPS,
the CPU-only synthetic probe and these host tests are not the target's 20 FPS gate.
