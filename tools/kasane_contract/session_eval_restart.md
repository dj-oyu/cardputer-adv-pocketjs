# Initial evaluation failure and real owner teardown

This synthetic host connection test closes a gap in `session_cleanup_probe`:
that probe extracted `app_stop()` but used fake reset and guest-finalizer functions.
Here QuickJS runtime/context, Shiki resources, Kasane scene submission, resets and
QuickJS finalizers are actual product implementations. No product code was changed.

Run:

```
python3 tools/kasane_contract/run_session_eval_restart.py
```

If the host cannot execute LeakSanitizer under ptrace, use the explicit `--no-lsan`
option. ASan/UBSan and the allocation ledger remain enabled. The initial default
run in this environment stopped with LSan's ptrace fatal error, not a test assertion.

## Coverage

Three initial-evaluation rejection points, repeated three times:

1. Before a Shiki resource exists.
2. After a real procedural resource and a Shiki data window with two field views
   are allocated. The views remain globally reachable during reset.
3. After a real Kasane image is submitted, with `needs_present` still true.

Each failure consumes the actual JS exception, ends the turn, resets Kasane before
Shiki (the relative ordering in product `app_stop`), destroys the actual context
and runtime, and ends the Shiki heap. Before destroying the context, all three
globally retained views must report byteLength zero after actual Shiki reset.
Requested live bytes and allocation blocks from the linker-wrapped allocator
must both be zero. Repeated cleanup is then checked. Immediately afterward, a new
runtime/context must initialize the same APIs and successfully present a frame.
There are nine rejected sessions and nine successful restart sessions.

## Result and scope

O2 with ASan/UBSan passed, with LSan disabled for the stated host restriction.
The recorded Shiki heap usage is a host observation,
not target RAM budget or performance measurements. Native compositor transfer uses
the existing deterministic delayed host port; it is not physical DMA or LCD I/O.
The QuickJS default allocator is used; there is no separate MB guest ledger claim.

This is NOT full `app_start_test`, manifest admission, the pocketjs_guest wrapper,
all peripheral/API resets, real stop hooks, a real firmware restart, or concurrent
display safety. It checks the real ownership/finalizer connection after an explicit
initial JS exception. It does not inject native OOM (covered in the separate Scene
lane). PR #131 and private character assets are not included. Real device 20 FPS
remains unverified.

Evidence is written to `.cache/session-eval-restart/results.log` and `commands.json`.
