// Loop shapes: every backward branch (goto/goto8/goto16, if_true/if_false)
// that the L2 checkpoints hook. Output is a checksum per shape.
function h(acc, v) { return (Math.imul(acc, 31) + v) | 0; }

let a = 0;
for (let i = 0; i < 1000; i++) a = h(a, i);
print("for", a);

let w = 0, i = 0;
while (i < 777) { w = h(w, i * 3); i++; }
print("while", w);

let d = 0; i = 0;
do { d = h(d, i ^ 0x55); i += 7; } while (i < 5000);
print("do", d);

let lab = 0;
outer: for (let x = 0; x < 30; x++) {
  for (let y = 0; y < 30; y++) {
    if (y > x) continue outer;
    if (x * y > 400) break outer;
    lab = h(lab, x * 100 + y);
  }
}
print("labeled", lab);

const o = { b: 2, a: 1, 10: "ten", 2: "two", c: 3 };
let keys = [];
for (const k in o) keys.push(k);
print("for-in", keys.join(","));

let fo = 0;
for (const v of [5, 6, 7, 8]) fo = h(fo, v);
for (const ch of "héllo") fo = h(fo, ch.codePointAt(0));
for (const [k, v] of new Map([[1, 2], [3, 4]])) fo = h(fo, k * v);
print("for-of", fo);

// Long-running body with no calls: the shape a budget yield must cut.
let s = 0;
for (let k = 0; k < 200000; k++) { s = (s + k * k) % 1000003; }
print("hot", s);

// Closures created per iteration keep per-iteration let bindings.
const fs = [];
for (let k = 0; k < 5; k++) fs.push(() => k);
print("let-per-iter", fs.map(f => f()).join(""));
var vs = [];
for (var k2 = 0; k2 < 5; k2++) vs.push(() => k2);
print("var-shared", vs.map(f => f()).join(""));

// switch with fallthrough inside a loop.
let sw = "";
for (let k = 0; k < 6; k++) {
  switch (k % 4) {
    case 0: sw += "a";
    case 1: sw += "b"; break;
    case 2: continue;
    default: sw += "d";
  }
  sw += ".";
}
print("switch", sw);
