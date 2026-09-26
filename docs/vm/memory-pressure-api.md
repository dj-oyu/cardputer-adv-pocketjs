# Memory pressure API — host/VM contract

Basis: `vm/main` at `4437822` (F3a/F3b and STRESS TEST), compared with
`feature/kasane` and its current VM/host code. The STRESS LV3 experiment reaches
the foreground guest's 160 KiB QuickJS quota while Kasane native storage stays
about 13 KiB. Disabling reads does not prevent OOM; disabling retained JS load
does. This is a guest-quota case, not proof of physical DRAM exhaustion.

## Boundary and API

Memory pressure is a host/VM service, not a Kasane feature. The first version
offers one listener per guest. The implementation installs it for foreground
and overlay guests (only one guest is active at a time):

```js
pocket.memory.pressure()             // current integer mask, no result object
pocket.memory.info()                 // allocating diagnostic snapshot; may OOM
const subscription = pocket.memory.onPressure((mask, episode) => { /* trim */ });
subscription.close();
```

Mask bits are `1` guest quota headroom, `2` internal 8-bit DRAM free bytes,
`4` internal 8-bit DRAM largest free block, and `8` a recent allocation
failure. Each bit has independent entry/exit hysteresis; the mask represents
*currently active* reasons, not every reason seen historically. `info()`
separately exposes an OR-ed failure-reason set (quota, underlying allocator,
native subsystem) and counters. `0` means pressure has cleared. The episode
is a saturating session-local uint32: entering pressure starts one episode;
it ends only when every bit clears. Any mask change queues notification, but
intermediate states—including a `0` followed quickly by re-entry—may be
coalesced away. The callback receives only the latest `(mask, episode)`; it is
not a lossless event stream. A caller may read `pressure()` without
subscribing; reads never acknowledge or clear state. A second listener
registration fails with `BUSY` rather than silently replacing the first.
Registration queues the current nonzero state for next legal delivery, but
healthy state does not produce an initial callback. A failed registration
leaves the slot free. Closing is idempotent and cancels pending delivery and
retry; a later listener receives the current state, not its predecessor's.

`info()` returns guest used/limit/headroom, internal free/largest, current
mask and episode, sample age, reasons seen in the current episode, and
session-cumulative saturating failure counters/reason bits. PSRAM is not part of this first snapshot or its
thresholds. Headroom and largest
block are observations, **not** a guarantee that an allocation will succeed.
An underlying QuickJS allocator failure may increment both allocatorFailures
and nativeFailures through ESP-IDF's global failure hook; they are diagnostic
views, not disjoint event totals.
`pocket.device.metrics()` remains a 500 ms cached, allocating diagnostic API;
its `JS_ComputeMemoryUsage()` object walk is unsuitable for pressure sampling.

## Detection and delivery

The VM supplies an O(1), allocation-free accessor for QuickJS accounted
`malloc_size` and limit. Extend the OOM canary to distinguish quota rejection
from underlying allocator failure at the actual rejection branches. The
canary is currently process-global and destructive-read: the app-session
owner must drain it **once**, then fan the result out to existing OOM logs and
a fixed-size pressure latch. No API getter or listener drains it separately.
Native failure sites publish a bounded reason bit/counter to the host latch;
cross-task producers publish primitive data with a session generation, never
JS values or callbacks. Preserve the single-active-guest assumption until the
canary becomes per-runtime.

Sample guest accounting at every safe owner-task boundary and internal DRAM at
most every 100 ms, including display-only turns; never traverse all JS objects
per frame. A rejection sets the failure bit when the host drains the OOM
canary; it does not force another native heap walk. `pressure()`
returns the last sample, not a fresh allocation attempt. Select numerical
entry/exit byte watermarks from measured allocation and callback headroom,
requiring exit > entry and at least 500 ms continuously healthy before
clearing. Current provisional enter/exit watermarks: guest headroom 32/48 KiB,
internal free 24/36 KiB, largest internal block 16/24 KiB. These thresholds
still need device measurement; they are not a utilization-derived guarantee. A large failed
allocation may set bit `8` even when aggregate free bytes look healthy. Clear
that bit only after a failure-free recovery interval. Thresholds are firmware
policy, not app-controlled percentages.

Queue at most one pending callback carrying the latest mask and episode;
storms cannot grow memory use. Deliver only at an ordinary legal host-pump
boundary, after the current JS call and any parked continuation or pending-job
drain, before starting unrelated new JS work. Enter under the existing
turn watchdog/budget; never reset that budget for notification. Callback
return values are ignored. Synchronous cleanup is expected: queued jobs obey
the normal scheduler/rejection rules, and an async callback is not evidence
that reclamation finished. Do not run from an
allocator, GC, finalizer, ISR/driver task, suspended bytecode chain, or teardown.
Back/save/stop outrank this advisory. Idle sessions may be woken through the
existing bounded wake mechanism, without a per-event timer allocation.
Callback failure is contained and counted without formatting the exception
under pressure. Do not reuse the existing exception-stringifying subscription
delivery helper unchanged. Any callback exception closes its subscription:
QuickJS cannot reliably distinguish an entry allocation failure from a throw
after partial app-side effects, so retry could duplicate cleanup. The app may
register a new listener later. Handler-caused allocation
failures still update diagnostics and pressure state; suppress only recursive
notification triggered solely by those failures. Subscription state and
pending events die with the guest generation.

Only **detection/latching** is guaranteed allocation-free. `JS_Call`, VM
frames, and arbitrary app cleanup can allocate. Notification and successful
recovery are best effort, not an OOM rescue guarantee. Apps with long
allocation-heavy turns still need local limits and caught-failure cleanup.

## Kasane participation

Kasane must keep the displayed APP/SYSTEM banks, pending submission and repair
data, live templates/instances, pinned source snapshots, and active animation
state. It may register a bounded native reclaimer only for independently
reconstructible, unreferenced storage, invoked at an owner-safe point—not
inside `malloc`. Current `cache.release()` removes a template but retains the
cache backing; `free_cache()` runs at runtime teardown. Neither is an active
scene trim contract. Kasane must not treat guest-quota pressure as permission
to erase app JS state, and freeing native storage does not increase the guest
quota. App code chooses semantic degradation; the host reports pressure;
Kasane preserves committed native display state under its existing failure
contract and, where proven, performs native trim. It cannot roll back arbitrary
JS side effects in an app's build callback.

## Acceptance gates and current verification

- Unit-test quota refusal with ample DRAM, allocator refusal below quota,
  fragmentation with healthy total free, and separate PSRAM/internal pools.
- Verify bounded latch/counters across repeated `malloc`/`calloc`/`realloc`
  failures, including preservation of an old `realloc` allocation.
- Check pressure, escalation, transient dips, sustained recovery, huge failed
  requests, and no duplicate episode or clear.
- Exercise parked continuations, pending jobs, idle wake, display-only turns,
  Back/save, teardown, stale cross-session events, callback throw/OOM/close,
  and repeated failures without recursive notification.
- Forbid allocations in detector/native reclaimer fault-injection tests.
  Measure sampling/wake cost, internal free and largest-block floors, and
  notification delivery under STRESS LV3. Caught OOM must remain recoverable.
- Any Kasane trim needs separate pin/submission/repair/animation and pixel
  tests; never infer safety from `free_cache()` existing at teardown.

Implement in order: (1) reason-aware canary and cheap accounting, (2) host
latch/query and tests, (3) listener delivery with the safe-point tests, then
(4) optional native reclaimers only if measured system-DRAM pressure warrants
them. This entails a lazy `pocket.memory` namespace, session-owned listener
init/reset/GC marking, single canary fan-out in `report_oom_if_any()`, and a
no-format pressure pump outside FAIR mid-drain delivery. The owner is the
existing paced UI loop, not a new guest scheduler. Do not block the query/latch
on speculative Kasane reclamation.

The API, canary fan-out and STRESS LV3 use are implemented. The host QuickJS
STRESS harness exercises notification and allocation-failure trimming while
preserving deliberate OOM/recovery. The dedicated host test covers mask
entry/clear, episode numbering, Back suppression, listener replacement and
thrown-callback containment. On Cardputer ADV (2026-09-27, 20 seconds per
level), STRESS passed: LV3 reported 36 pressure notifications, 18 trims,
18 caught OOMs and zero app errors; the app stopped and returned home cleanly.
This verifies the end-to-end guest-quota path, not the system-DRAM thresholds
or the other fault-injection and scheduler gates above. No Kasane native
reclaimer is enabled. The ESP32-S3 `build_stress_kasane` firmware build links
successfully; the initial build's static DIRAM delta was +144 B. Threshold
calibration across music and overlay remains open.
