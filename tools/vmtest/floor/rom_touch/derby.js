// Builtins DERBY WATCH reaches (union of apps/derby/*.js member names,
// receivers attributed by hand; docs/vm/rom-intrinsics.md sec.2.2).
var M = Math, A = Array.prototype, S = String.prototype;
var t = [M.sin, M.cos, M.sqrt, M.ceil, M.imul, M.pow, M.floor, M.round, M.max, M.min, M.exp, M.PI,
  A.slice, A.push, A.indexOf, A.splice, A.sort, A.map, A.shift, Function.prototype.apply,
  Promise.prototype.then, Object.keys, S.split, S.toUpperCase, S.replace, Object.prototype.toString,
  Number.prototype.toFixed, Number.prototype.toString];
