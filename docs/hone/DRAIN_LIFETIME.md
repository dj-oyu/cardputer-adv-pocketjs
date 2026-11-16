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

Scope: runtime admission, not complete app STOP or physical DMA proof.
`app_session.c` still continues guest/API teardown when `pocket_kasane_reset`
returns false. That caller must defer teardown and retry drain before the whole
STOP lifecycle is closed. Alpha production registration, immutable snapshot pin,
visible ACK promotion and device measurements remain unfinished gates.
