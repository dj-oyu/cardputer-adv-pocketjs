// yield_terminate: paired with --terminate-after N (design sec.12.12, D25)
// once L2c's body exists -- terminating a parked chain must raise an
// uncatchable "interrupted" and skip any enclosing `finally`, the same as
// today's force-yield kill but from a RESUMABLE chain instead of one that
// was never parked. Until then --terminate-after only prints a
// "vmrun: note:" line, and this file's own output (no yield at all) is what
// --bless records as the guard's baseline.
let ranFinally = false;
function loopy(n) {
  try {
    let t = 0;
    for (let i = 0; i < n; i++) t += i;
    return t;
  } finally {
    ranFinally = true;
  }
}
print("loopy", loopy(2500), "finally", ranFinally);
