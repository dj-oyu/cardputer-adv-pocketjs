// vmrun-flags: --profile device
// Same shape as deep_recursion.js under the device limits (160 KiB heap,
// 20 KiB JS_SetMaxStackSize, main/app_session.c). The depth is "#info": host
// frames are not Xtensa frames, so the host number says nothing exact about
// the device -- only that the device bound is far lower than the host one.
function sum(n) { return n === 0 ? 0 : n + sum(n - 1); }
print("sum(8)", sum(8));

let depth = 0;
function dive() { depth++; dive(); }
let kind = "none";
try { dive(); } catch (e) { kind = e.constructor.name + ": " + e.message; }
print("overflow", kind);
print("#info max_depth=" + depth);
print("recovered", sum(5));

// Overflow inside a callback a builtin is running: the RangeError must cross
// the native frame of Array.prototype.map.
let inner = 0;
function viaMap(n) { inner++; return [n].map(viaMap); }
try { viaMap(0); } catch (e) { print("through-map", e instanceof RangeError); }
print("#info max_depth_via_map=" + inner);
