// Union of the builtin names the shipped apps reference (grep of apps/*/,
// 2026-10-26), receivers attributed by hand.
var M = Math, A = Array.prototype, S = String.prototype, N = Number.prototype;
var t = [M.sin, M.cos, M.sqrt, M.pow, M.ceil, M.floor, M.round, M.abs, M.min, M.max, M.imul, M.fround, M.exp, M.PI,
  A.push, A.slice, A.indexOf, A.splice, A.sort, A.map, A.shift, A.forEach, A.concat, A.filter, A.unshift, A.join,
  Array.isArray, Object.keys, Object.assign, Object.getPrototypeOf, Object.prototype.toString, Object.prototype.hasOwnProperty,
  S.split, S.replace, S.toUpperCase, S.toLowerCase, S.trim, S.repeat, S.indexOf, S.slice, S.charCodeAt,
  N.toFixed, N.toString, Number.isInteger, Function.prototype.apply, Function.prototype.call,
  Promise.prototype.then, Promise.prototype.catch, Promise.resolve, JSON.stringify, JSON.parse, Date.now];
