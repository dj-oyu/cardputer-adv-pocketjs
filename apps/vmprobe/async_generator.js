// VM_PROBE L0 workload, USB-only: one frame = draining a fresh async
// generator, mixing generator suspend/resume with await's own job-queue hop.
async function* gen(n) {
  for (let i = 0; i < n; i++) { await Promise.resolve(); yield i; }
}
globalThis.frame = () => {
  (async () => { for await (const _ of gen(20)) {} })();
  return 0;
};
