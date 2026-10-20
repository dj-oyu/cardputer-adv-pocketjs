# Fixed-trial runtime resource observations

The accepted integration at `df63fbaa8cec6dce4a0f9d2d05d2d41a929baaf6`
has no qualifying UI-task runtime stack observation in its fixed-trial logs.
VMPROBE can sample stack and heap, but is a separate opt-in probe that samples
inside measured turns. Compiler `.su` files describe individual functions, not
the complete QuickJS/native call chain. Neither substitutes for runtime evidence.

The new diagnostic-only path emits one `DERBY_RESOURCE v=1` record after all
three dependent repetitions, after their summaries and before `DERBY_FIXED_END`.
It gets its own reporting turn, with one bounded USB write and a guest return
before END. There is no new observation inside timed work, detail scopes, DMA
callbacks, or an ISR. Ordinary builds do not compile the adapter or bind a task.
Source/layout changes can still affect instruction-cache behavior: previous
performance results do not qualify this new candidate.

The adapter binds the task handle once at `ui_task` entry. Sampling requires the
same current handle, the task name `ui`, a live guest, and phase `DF_TAIL`.
Failed/invalid sampling, an observation taking over 125 ms, an insufficient
session reserve, or a refused/partial write poisons the trial. The existing
armed guest deadline is checked before sampling and before output with 125 ms
reserve, then again after output with zero reserve; no deadline is rearmed by
the observer. Existing 250 ms
guest watchdog, 180 s session, output bounds, STOP/HOME and owner checks remain.
This observation has no retry. Native allocator queries do not allocate memory.
Resource failure poisons the trial without a second USB write, including an
invalid END attempt. This keeps a slow observation plus failed output from
adding another 100 ms native wait. Missing END cannot qualify as completion.

| Field | Meaning, in bytes |
| --- | --- |
| `stack_free` | Minimum unused UI-task stack since task creation, including prior applications and observer/report frames; maximum 32768, the configured UI stack size |
| `internal_free` | Current allocator-reported free memory with `MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT` |
| `internal_largest` | Current largest free block from the same `heap_caps_get_info` query |
| `guest_used`, `guest_cap` | Current QuickJS allocation counters from `JS_GetMemoryCounters`; headroom is cap minus used |

[ESP-IDF 6.0.1](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/api-reference/system/freertos_idf.html)
reports `uxTaskGetStackHighWaterMark` in **bytes**, not upstream FreeRTOS words.
The stack value is a lifetime low-water observation, not a per-repetition peak.
The heap values are post-trial snapshots, not minimum free heap or transient
allocation peaks. We deliberately omit `minimum_free_bytes`: per-region minima
and existing global local-minimum monitors have different scopes. This observer
does not start, stop, or reset those monitors. Other tasks can allocate while
heap information is aggregated; it is not a globally atomic memory census.

The host verifier validates every resource marker whenever one is present,
including schema, identity, units, ordering, duplicate/unknown fields, range and
internal consistency. Resource qualification additionally requires
`--require-runtime-resources` and **all four explicit positive byte floors**:

```
--min-ui-stack-free-bytes N
--min-internal-free-bytes N
--min-internal-largest-bytes N
--min-guest-headroom-bytes N
```

Each value must meet its floor; equality passes. Missing evidence or an omitted
floor fails closed. Floors are acceptance-policy inputs, not values inferred
from a successful run. No production stack margin or experiment floor is
established by synthetic tests. Existing native pressure thresholds are also
not allocation guarantees. Freeze the four floors before requesting physical
execution; do not choose them after seeing the candidate's observation.

Legacy no-record J1 archives retain consistency verification and explicitly
report `runtime_resources.resource_verified=false`. An omitted qualification
flag never grants resource acceptance. Host controller observations identify
`source=synthetic` and cannot qualify. Tests also construct explicitly synthetic
owner archives with the device record shape to exercise acceptance and rejection
against actual CI BIN/ELF pairs. Their report remains `hardware_tested=false`;
JSON self-hashes authenticate neither the archive nor any physical measurement.

No source admission, registry entry, broker authorization, deployment or device
operation is part of this PR. The next approval must name this new exact head.
The registry's structural four combinations (detail ON/OFF × DMA ON/OFF) remain
distinct from the proposed two physical conditions: **detail ON / DMA OFF first,
then detail ON / DMA ON**. Four CI builds do not authorize four physical trials.
Preserve the measured `b561aea1` J1 OFF image and recovery data. Normal main and
the measured branch remain unchanged; physical acceptance and DMA gains remain
pending a separately approved bounded trial.
