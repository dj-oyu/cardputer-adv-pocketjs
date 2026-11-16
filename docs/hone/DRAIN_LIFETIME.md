# Failed display drain lifetime

The runtime retains a failed-completion flag until a caller supplies a successful
completion for the same display. Same-display identity is a trusted caller
precondition, not enforced by the runtime. The app_session adapter always invokes
board_present_finish for the single display. No stack callback context is retained.
APP detach, runtime shutdown, cache allocation and legacy presentation stay BUSY
while a completion is active or a drain is unresolved. Retrying drains before
reusing any strip or rendering again.

`ASAN_OPTIONS=detect_leaks=0 python tools/test_dma_completion_failure.py`
exercises the real runtime with O2 and ASan/UBSan. A mock display retains a strip,
and an APP tail represents owned source data. Failure preserves the APP tail;
repeated failed drain performs no new sends; successful fresh-context drain
allows rendering and eventual detach. Leak detection is disabled under ptrace.

## STOP caller integration

STOP now drains before hooks or source teardown and reports completion as a
boolean. Failed drain retains the guest and shared-strip ownership. A later
reset refusal preserves a prepared phase so the stop hook and preceding resets
are not repeated. Starts and resumes reject retained/pending guest state.
The owner UI retains screen/end-run/overlay transition intent and retries before
control pumping or rendering; native BUSY rejects a deferred start rather than
silently running it later.

`tools/test_app_stop_drain.py` compiles actual app_stop code with host dependency
mocks and checks no mutations on failed drain, one hook across retry, and one
final destruction. `tools/test_ui_stop_transitions.py` extracts actual UI
transition functions and checks retained release/stop/choice, screen/end-run and
source-copy ownership. These are bounded host fault tests, not a full ESP task
or physical DMA test. The normal firmware build is tracked per exact head.

Alpha production registration, immutable snapshot pin, visible ACK promotion,
private full-model firmware builds and device measurements remain unfinished.
