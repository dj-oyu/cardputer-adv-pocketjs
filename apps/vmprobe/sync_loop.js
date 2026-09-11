// VM_PROBE L0 workload (docs/quickjs-freertos-vm-spec.md sec.5), USB-only:
// no I/O, no allocation, one fixed-size arithmetic loop per frame -- the
// closest this set gets to pure bytecode-dispatch cost.
globalThis.frame = () => {
  let acc = 0;
  for (let i = 0; i < 20000; i++) acc += (i ^ (i << 1)) & 0xffff;
  return acc;
};
