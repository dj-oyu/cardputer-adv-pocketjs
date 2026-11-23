// VM_PROBE L0 workload, USB-only: one outstanding kobako.time.sleep at a
// time, re-armed from its own .then() -- exercises the completion-to-
// handler latency L0 asks for (main/kobako/kobako_api.c's completion table,
// timestamped at kobako_api_complete() and again where resolve/reject runs).
function again() { kobako.time.sleep(10).then(again).catch(() => {}); }
again();
globalThis.frame = () => 0;
