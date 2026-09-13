// yield_toplevel_loop (design sec.12.9/12.15 stage 2, first-version 12.13-2):
// the simplest MAY_YIELD floor once L2c's body lands -- a plain top-level
// loop, no calls at all, so its floor is a host-owned SEG floor (JS_VMEval
// directly, sec.12.4's first table row). Blessed WITHOUT yield: once the
// body exists, --force-yield must reproduce this exact output after
// resuming.
let sum = 0;
for (let i = 0; i < 5000; i++) sum += i;
print("sum", sum);
