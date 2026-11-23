# Explicit streaming producer candidate

This is a firmware-compilable **producer endpoint**, not a registered actor,
model loader, renderer, or execution-ready model. The default model API/ABI and
render path are unchanged. `main/ayatsuri/ayatsuri_model_stream_candidate.c`
compiles the existing model implementation in a separate translation unit under
isolated symbols and explicit streaming branches. No source-rewriting step is
required to build this API. Only successfully opened immutable provider-only
group/mesh models are admitted; holes, double-sided meshes and more than 32
vertices per node are rejected. Ordinary cold geometry fallback is not allowed.

The default-OFF Kconfig option is `CONFIG_AYATSURI_STREAM_CANDIDATE`.
The broker variant is `ayatsuri_stream`, selected by the checked-in
`sdkconfig.variant.ayatsuri_stream`. It enables only compilation of the
separate TU and `-fstack-usage`; it adds no boot hook, data assets, allocations,
JS registration, frame publication or display calls. Firmware build and device
validation must use the authorized broker workflow.

## Ownership and rollback

The zero-initialized `ayatsuri_stream_candidate` is a distinct handle. Use its
open/bytes/init/targets/frame/dispose API only; do not access its private core or
pass it to ordinary APIs. Model and blob remain immutable borrowed storage for
the whole instance lifetime. A generation and exclusive synchronous owner are
required. Provider callbacks have the same restrictions as the existing trusted
native provider: no script execution, reentry, retained slot, or source/model
mutation. All mesh nodes, including disabled nodes, are supplied on each frame.

Persistent node state retains transform and pose. Each mesh packet is generated
in one shared maximum-sized slot, copied immediately into the caller's pending
snapshot, and then the node offset is restored. There is no retained packet/view
API. The separately compiled internal next/view functions explicitly reject.
Numeric/provider/append failure leaves private candidate and pending work
potentially partial, but publishes nothing. A later retry recomputes all nodes.

Pending descriptor and capacity are checked against the instance, arena,
model/blob, provider slot, camera/light and each other. The caller must additionally
keep pending storage **disjoint from externally READY, pinned, and DMA-owned
storage**; this producer cannot inspect an unrelated display owner's pointers.
The API is not thread/ISR safe and is not a security boundary against native
handle forgery. Successful production only completes pending. It does not
preflight a complete composed display frame, swap ready, pin a frame, or ACK.

The existing kobako path performs display completion before presentation ACK,
including the no-damage path (`kobako_kasane_present_complete`). Its pending versus
committed snapshots and retry/repair ownership must remain authoritative. A
future caller may transfer a completed pending snapshot only through that owner;
it must not replace a packet allocation while rendering or a display submission
can still read it. `ksn_runtime_finish_display` distinguishes a drained display
from a still-borrowed one. DMA pixel bands are separately owned: freeing source
geometry after packet copy does not authorize reuse of a submitted pixel band.
No bypass of that ownership chain is implemented by this candidate.

## Host evidence

Run the public synthetic API test with a host C compiler and Node available:

```sh
CC=cc python tools/ayatsuri/provider/test_stream_candidate_api.py --out /tmp/stream-api-o2
CC=cc python tools/ayatsuri/provider/test_stream_candidate_api.py --out /tmp/stream-api-o0 --opt 0
CC=cc python tools/ayatsuri/provider/test_stream_candidate_api.py --out /tmp/stream-api-asan --sanitize
```

Use a new output directory for each run. Three fixtures include a mesh-parent
chain and mixed 3/32-vertex nodes. Each runs 24 frames and 1,124 injected failures,
checking packet/raster identity to the ordinary arena path, invalid owner/
generation, busy state, alias rejection, retry, and ready rasterization after
model/arena release. O0/O2 and Linux ASan/UBSan passed. The existing transformed
stream experiment and provider-only capability/fallback/alias/lifetime regression
also still pass; the production-default branch was not replaced.

A separate private probe connected the same native geometry used by the bounded
63-pose accuracy test to this actual API. Its 63 poses plus initial rest frame
passed packet/raster identity, all 231 mesh copies per frame, 256 injected
failures with ready preserved, and ready rendering after source/model release.
The same actual-API integration passed Linux ASan/UBSan. The model remains limited
to placeholder materials and restricted rig handling. Cold rig-to-mesh maps
were independently unchanged across those 63 poses (maximum difference zero);
this does not establish invariance for arbitrary controls/poses.

## RAM and stack: what is and is not measured

For that private host probe, instance arena allocation changes from 337,140 to
27,496 bytes; maximum observed packet snapshot is 35,280 bytes. The layout is
aligned node-state storage plus 88 bytes per node plus the maximum legacy mesh
slot. These are **host allocation counts**, not measured ESP32 physical SRAM or
PSRAM placement. ABI `sizeof` differences and linker placement require the target
build. This candidate performs no allocations itself; the owner chooses storage.

Arena reduction alone is insufficient as a target RAM admission. For example,
current fully retained host geometry uses 15,912 bytes of source positions,
70,968 bytes of corner normals and up to 5,496 bytes of normal scratch. Two
snapshots at the observed maximum add 70,560 bytes. These together with the
stream arena already total 190,432 bytes before node/world arrays, masks, wrap
scratch, driver/material state, pixel bands, task stacks, allocator overhead,
and other firmware activity. The probe's per-wrap allocations must be replaced
by bounded startup-owned scratch before sustained target use. Cold model/data
must have a deliberate flash/RAM placement plan rather than copying the host
probe's heap-loaded blob pattern.

Windows GCC 15.2.0 `-O2 -fstack-usage` reports 1,712 bytes for `mesh_packet`,
704 for the evaluation core, and 144 for the public frame entry. Those are
individual host function frames, not a complete call-chain bound or target
measurements. The provider's 32-result slot is another 768 bytes of caller-owned
storage, and its gather plan is 256 bytes. The UI task is configured with 32,768
bytes in this tree, but this is not evidence of remaining stack headroom. Use
the broker target build's `.su` files, actual call graph, and task high-water mark
under load before approving stack size. Record heap free and largest contiguous
internal block at startup/allocation peaks; total free bytes alone do not prove
allocation success. Physical DMA ownership and target-model FPS remain untested.

## Optional synthetic device probe: design only

A follow-up probe should be separately default-OFF and explicitly invoked on the
VM owner while no frame/submission is pending. It should use public synthetic
geometry, preallocated internal-memory arena/snapshots/provider slot, and one
bounded frame per invocation. It can log target cycles, allocation capacities,
internal free/largest block and stack high-water mark without submitting pixels.
Failure must dispose only probe-owned storage and leave the existing display
owner untouched; restoration means disabling the probe variant and returning to
the prior firmware through the broker. No automatic boot run, UI-task replacement,
DMA override, or flash/model write is proposed here. Adding an invocation hook
requires separate review of its exact scope and recovery conditions. Such a
probe measures its synthetic workload, never the target model's 20 FPS gate.
