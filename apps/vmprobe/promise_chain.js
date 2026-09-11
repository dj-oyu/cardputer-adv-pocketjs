// VM_PROBE L0 workload, USB-only: one frame = a fixed-length .then() chain,
// resolved eagerly, so the job queue -- jobs executed and max pending
// length, both counted in the vendored quickjs.c -- is actually exercised
// every tick instead of sitting empty between frames.
globalThis.frame = () => {
  let p = Promise.resolve(0);
  for (let i = 0; i < 40; i++) p = p.then((v) => v + 1);
  p.catch(() => {});
  return 0;
};
