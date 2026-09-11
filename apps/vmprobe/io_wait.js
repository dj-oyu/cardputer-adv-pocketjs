// VM_PROBE L0 workload, USB-only: one outstanding pocket.time.sleep at a
// time, re-armed from its own .then() -- exercises the completion-to-
// handler latency L0 asks for (main/pocket/pocket_api.c's completion table,
// timestamped at pocket_api_complete() and again where resolve/reject runs).
function again() { pocket.time.sleep(10).then(again).catch(() => {}); }
again();
globalThis.frame = () => 0;
