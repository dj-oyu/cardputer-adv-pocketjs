// L2b (docs/vm-L2-design.md sec.10.3): a generator/async floor's argc.
// A flat call made from a default-parameter initializer returns into the
// floor before OP_rest binds the rest parameter, so OP_rest reads the argc
// the flat return rebuilt. The floor's frame is JSAsyncFunctionState.frame,
// whose arg_count is max(declared, passed), not what the call passed; the
// first version rebuilt argc from it and bound r = [undefined] on the flat
// path only (recur and alloca: []). Three shapes, called with too few, the
// declared number, and too many arguments, so a wrong argc shows as a wrong
// rest length whichever way it is off.
function h() { return 1; }
function* g(a = h(), ...r) { yield "gen " + r.length + " " + JSON.stringify(r); }
print(g().next().value);
print(g(5).next().value);
print(g(5, 6, 7).next().value);
async function af(a = h(), ...r) { return "async " + r.length + " " + JSON.stringify(r); }
af().then(print);
af(5, 6).then(print);
async function* ag(a = h(), ...r) { yield "asyncgen " + r.length + " " + JSON.stringify(r); }
ag().next().then(v => print(v.value));
ag(5, 6, 7).next().then(v => print(v.value));
