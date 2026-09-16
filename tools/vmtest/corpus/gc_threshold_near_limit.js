// vmrun-flags: --profile device
// Regression for docs/vm/backlog.md #5, second half: cyclic garbage while
// most of the 160 KiB limit is LIVE. Upstream sets the next threshold to 1.5x
// the survivors; with ~3/4 of the limit surviving that lands past the limit,
// so lowering only the initial threshold would still OOM here.
// quickjs.c js_gc_next_threshold caps it at half the remaining headroom.
//
// The live set is sized by filling to OOM and dropping a quarter, so the file
// needs no knowledge of per-object sizes (the host's are twice the device's).
const keep = [];
try {
  for (;;) keep.push({ v: keep.length });
} catch (e) {}
keep.length = (keep.length * 3) >> 2;
let n = 0;
let caught = false;
try {
  for (; n < 20000; n++) {
    const a = { n }, b = { a };
    a.b = b;
  }
} catch (e) { caught = true; }
print("near-limit-cycles-oom", caught);
print("live", keep.length > 0, keep[keep.length - 1].v === keep.length - 1);
