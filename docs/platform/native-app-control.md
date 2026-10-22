# Native app control initial implementation

The versioned wire contract is [app-control-v1](../api/app-control-v1.md).
The native `C` namespace is available at HOME and remains alive when the guest
`B` bridge resets or suspends. The existing input task parses both namespaces;
only the UI owner creates, resumes, suspends, ticks or destroys a guest.

The catalog binds eleven foreground app IDs to their actual embedded source
and stable registry manifest. Known editor, dynamic-work, player and overlay
entries are advertised unavailable; typed launches for those host workflows
are not implemented. VM PROBE also advertises unavailable, including builds
that contain its diagnostic workloads. No public launch falls back to HELLO.

`fresh` evicts a dormant guest and creates a new run. `resume` preserves the
same guest generation and run after SUSPEND inside the current control session.
A new broker session cannot attach to an unmanaged dormant guest: initial
`start=resume` is explicitly unsupported by the broker. BACK uses the existing
action path and may suspend; STOP destroys the controlled guest and does not
infer success from HOME_READY. Physical/guest launches of another instance,
even with the same app ID, invalidate controlled input. Cleanup refuses to
destroy an unrelated replacement guest and reports needs_review.

The firmware permits at most 60 user operations plus bounded lifecycle cleanup
within its 64-entry ledger. Broker plans further limit interactive operations
to 59 because initial START consumes one device operation. The queue has four
requests and one separate full-queue refusal mailbox. That mailbox yields a
stable queue_full receipt. A hostile burst beyond both bounded areas can lose
ACKs; the client asks STATUS and reports outcome_unknown when evidence is absent.
It never turns missing evidence into input replay. Rejected/consumed receipts
stay separate from console output and cannot be evicted by log flood.

The UI checks the fixed device TTL between commands and after potentially
blocking owner calls. It cannot preempt a currently executing guest turn; the
existing guest watchdog remains responsible for bounded guest execution.
Expiry destroys the controlled guest, retires the device-issued session offer
and prevents old OPEN/START replay. No reconnect, reboot or replay is performed.
The sequence/receipt guarantees do not extend across firmware reboot.

Non-frame stdout/stderr escape raw GS as visible `\x1d`, so guest strings cannot
forge native acknowledgments or legacy frames. `usb_console_frame` remains the
only raw framed writer. CRC detects corruption and is not authentication.
Authorization, immutable plans, lease fencing and log redaction stay in the
broker; there is no second authentication implementation in the firmware.

## Host checks

```sh
python tools/test_app_control.py
python tools/test_usb_console.py
python tools/test_session_dispatch.py
python tools/test_app_control_broker_interop.py --broker-source /path/to/esp-device-broker-mcp
```

The first check compiles the actual native controller and shared B/C parser
with optimized strict aliasing and ASan/UBSan. It exercises lifecycle/identity,
queue and sequence bounds, ACK loss/status, dedup/conflict, rate/command limits,
TTL across blocking turns, replaced guest generations and legacy isolation.
The interop check executes the broker's Python encoder/decoder against actual
C firmware state, without serial hardware. Existing benchmark parsers and
quality gates remain independent; transport success does not imply benchmark
quality success. GCC `-fstack-usage` records compiler stack estimates for the
owner/control/transport sources; these are not measured stack high-water marks.

The fixed-workload diagnostic-OFF gate keeps its historical source comparison
against pre-feature head `10fbed312682a122011f447290ba82891c16e8fd`.
It additionally compares the current ordinary owner paths against independently
reviewed native-feature head `6087f329146f4373569e7f82e0b1477ac51d13a4`.
Negative controls reject changed action delivery and instance-generation fences;
the diagnostic stripper and the other benchmark/machine-code gates are unchanged.
The native feature stays in its own commit so a later PR #46 device A/B can
apply the same control/measurement mechanism to both firmware variants.

## Integration prerequisites

This change is stacked on firmware PR #46 at
`10fbed312682a122011f447290ba82891c16e8fd`. Its dependency must be integrated
before this feature. The broker must be upgraded to the matching v1 contract,
then the client host must refresh imported MCP tool metadata using its normal
connection UI. These are later deployment steps; this task performs none of
them. ESP-IDF remains fixed at 6.0.1. Firmware must be independently reviewed,
built, and later validated on the actual device before any merge or flash.
