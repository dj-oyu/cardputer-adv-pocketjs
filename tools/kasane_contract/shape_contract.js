// pocket.shapes contract checks (docs/api/shapes.md), run by
// tools/kasane_contract/test_pocket_shape.c.
'use strict';
const eq = (got, want, what) => { if (got !== want) throw Error(what + ': got ' + got + ', want ' + want); };
const near = (got, want, what) => { if (!(Math.abs(got - want) < 1e-5)) throw Error(what + ': got ' + got + ', want ' + want); };
const throws = (f, code, what) => {
  try { f(); } catch (e) { if (e.code === code) return; throw Error(what + ': threw ' + e.code + ' ' + e.message); }
  throw Error(what + ': did not throw ' + code);
};
const P = new Float32Array(64), D = new Float64Array(64);
const put = (a, i, pts) => pts.forEach((p, k) => { a[2 * (i + k)] = p[0]; a[2 * (i + k) + 1] = p[1]; });

// project: x, y over w; w <= 0 gives NaN; both array types; ranges.
{
  const src = new Float32Array([1, 2, 3, 4, 5, 6, 0, 0, -1]), dst = new Float64Array(6);
  // x' = (2x + 10) / (z + 2), y' = (3y) / (z + 2)
  shapes.project([2, 0, 0, 10, 0, 3, 0, 0, 0, 0, 1, 2], src, dst);
  near(dst[0], 12 / 5, 'project x'); near(dst[1], 6 / 5, 'project y');
  near(dst[2], 18 / 8, 'project x 2'); near(dst[3], 15 / 8, 'project y 2');
  eq(Number.isNaN(dst[4]) && Number.isNaN(dst[5]), false, 'w = 1 is in front');
  shapes.project([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0], src, dst, 2, 1);
  eq(Number.isNaN(dst[4]) && Number.isNaN(dst[5]), true, 'w <= 0 is NaN');
  throws(() => shapes.project([1, 2, 3], src, dst), 'INVALID_ARGUMENT', 'short matrix');
  throws(() => shapes.project(new Float32Array(12), src, [0, 0]), 'INVALID_ARGUMENT', 'plain target');
  throws(() => shapes.project(new Float32Array(12), src, dst, 2, 2), 'LIMIT_EXCEEDED', 'past the end');
}

// fill: a rectangle, row by row, both ends drawn.
put(P, 0, [[10, 10], [20, 10], [20, 15], [10, 15]]);
shapes.fill(P, [0, 4], [], 0x1234);
eq(__segs(), '10,10,20,10,4660;10,11,20,11,4660;10,12,20,12,4660;10,13,20,13,4660;10,14,20,14,4660;10,15,20,15,4660', 'rectangle');
// A hole splits the rows it crosses; the hole's edge columns stay drawn.
put(D, 0, [[0, 0], [20, 0], [20, 2], [0, 2]]);
put(D, 4, [[5, 1], [10, 1], [10, 1.5], [5, 1.5]]);
shapes.fill(D, [0, 4], [4, 4], 7);
eq(__segs(), '0,0,20,0,7;0,1,5,1,7;10,1,20,1,7;0,2,20,2,7', 'hole');
// options: top / bottom narrow the rows, step 2 keeps even rows.
shapes.fill(P, [0, 4], undefined, 1, {top: 11.5, bottom: 14, step: 2});
eq(__segs(), '10,12,20,12,1;10,14,20,14,1', 'top, bottom, step');
// An ellipse; the screen clips rows and columns.
shapes.fill(P, [120, 2, 200, 3], [], 2);
const rows = __segs().split(';');
eq(rows.length, 5, 'ellipse rows 0..4 (above 0 clipped; the tip at 5 has no width)');
eq(rows[2], '0,2,239,2,2', 'ellipse widest row clipped to the screen');
// A polygon with a non-finite point draws nothing; such a hole is ignored.
put(P, 8, [[0, 0], [NaN, 0], [5, 5]]);
shapes.fill(P, [8, 3], [], 3);
eq(__segs(), '', 'NaN shape');
shapes.fill(P, [0, 4], [8, 3], 3, {bottom: 10});
eq(__segs(), '10,10,20,10,3', 'NaN hole');
throws(() => shapes.fill(P, [0, 2], [], 1), 'INVALID_ARGUMENT', 'two-point polygon');
throws(() => shapes.fill(P, [0, 4], [], 70000), 'INVALID_ARGUMENT', 'colour');
throws(() => shapes.fill(P, [0, 4], [0, 4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 4], 1), 'INVALID_ARGUMENT', 'seven holes');
throws(() => shapes.fill(P, [30, 4], [], 1), 'LIMIT_EXCEEDED', 'polygon past the points');
throws(() => shapes.fill(P, [0, 4], [], 1, {step: 3}), 'INVALID_ARGUMENT', 'step');
throws(() => shapes.fill(P, [1, 2, 0, 3], [], 1), 'INVALID_ARGUMENT', 'zero radius');

// outline: open and closed, pieces, hidden, clipped to the screen.
put(P, 0, [[10, 10], [30, 10], [30, 30]]);
shapes.outline(P, 0, 3, 5);
eq(__segs(), '10,10,30,10,5;30,10,30,30,5', 'open');
shapes.outline(P, 0, 3, 5, {closed: true});
eq(__segs(), '10,10,30,10,5;30,10,30,30,5;30,30,10,10,5', 'closed');
shapes.outline(P, 0, 2, 5, {pieces: 2});
eq(__segs(), '10,10,20,10,5;20,10,30,10,5', 'pieces');
put(P, 4, [[22, 0], [40, 0], [40, 20], [22, 20]]);
shapes.outline(P, 0, 2, 5, {pieces: 2, hidden: [4, 4]});
eq(__segs(), '10,10,20,10,5', 'a hidden piece');
put(P, 8, [[-100, 10], [100, 10], [100, 500]]);
shapes.outline(P, 8, 3, 6);
eq(__segs(), '0,10,100,10,6;100,10,100,134,6', 'clipped');
put(P, 8, [[-100, -10], [-5, -50]]);
shapes.outline(P, 8, 2, 6);
eq(__segs(), '', 'off screen');
throws(() => shapes.outline(P, 0, 1, 1), 'INVALID_ARGUMENT', 'one point');
throws(() => shapes.outline(P, 0, 3, 1, {pieces: 9}), 'INVALID_ARGUMENT', 'pieces');

// The frame limit: the call that hits it throws and leaves nothing.
shapes.fill(P, [120, 2, 200, 3], [], 2);
const before = __segs().split(';').length;
__limit(3);
throws(() => shapes.fill(P, [120, 2, 200, 3], [], 2), 'LIMIT_EXCEEDED', 'frame limit');
eq(__segs(), '', 'rolled back');
throws(() => shapes.fill(P, [0, 4], [], 1), 'BUSY', 'frame abandoned until beginFrame');
__begin(); __limit(4096);
eq(before, 5, 'the ellipse again');
