# Production frame owner admission

`pocket_shiki_frame.c` rejects access outside the already-bound VM owner task,
including ISR calls, before touching frame flags, packet pointers, or QuickJS.
`vm_wake_bind` runs at the beginning of `ui_task`, before producers start.
The binding is immutable during producer lifetime, as in the existing wake
contract. No frame-sized lock, copy, or critical section is added.

A rejected JS-returning entry returns `JS_EXCEPTION` without constructing an
exception on a foreign task. Such calls are programming errors; they must not
use the VM from that context. Completion producers must enqueue an event for
the owner, not call frame ACK/reset directly. Void entries leave outputs/state
unchanged on rejection. The host-only TLS switch is fault injection, not a
production bypass.

Evidence: the real QuickJS adapter test rejects foreign-context native begin,
append, reset, ACK, backdrop, row and span access, then resumes the exact pending
frame and existing 24-frame, retry and pixel regressions. `test_vm_owner.py`
compiles the actual predicate against synthetic unbound/owner/other-task/ISR
identities. This is not an ESP32 concurrency or timing measurement.

Not completed by this checkpoint: alpha registration and preflight/pin wiring,
DMA loan tracking, final-drain release, or actual LCD testing. Runtime detach
already refuses reset while presenting; an unconditional free inside proc reset
alone does not demonstrate an unsafe public STOP path. Conversely, finish IO
is not evidence that DMA has drained. Future alpha ownership must distinguish
ACK, retry retention, cancellation and successful drain. The overlay synchronous
backdrop path must be tested separately from APP completion-based presentation.

Reproduce from repository root:

```sh
python tools/hone/source_package/test_vm_owner.py --root .
python tools/kasane_contract/run_pocket_proc_qjs.py --opt 1
```

On a ptrace-based executor, LeakSanitizer can terminate with its unsupported
ptrace error. Running the resulting test binary with `ASAN_OPTIONS=detect_leaks=0`
retains address/undefined-behavior checks but is **not** a leak-check pass.
