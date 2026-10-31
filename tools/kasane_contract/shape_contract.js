// pocket.shapes contract checks (docs/api/shapes.md), run by
// tools/kasane_contract/test_pocket_shape.c.
'use strict';
const eq = (got, want, what) => { if (got !== want) throw Error(what + ': got ' + got + ', want ' + want); };
const near = (got, want, what) => { if (!(Math.abs(got - want) < 1e-5)) throw Error(what + ': got ' + got + ', want ' + want); };
const throws = (f, code, what) => {
  try { f(); } catch (e) { if (e.code === code) return; throw Error(what + ': threw ' + e.code + ' ' + e.message); }
  throw Error(what + ': did not throw ' + code);
};
const P = new Float32Array(64), D = new Float32Array(64);
const put = (a, i, pts) => pts.forEach((p, k) => { a[2 * (i + k)] = p[0]; a[2 * (i + k) + 1] = p[1]; });
eq(hone, shapes, 'hone and legacy shapes have one namespace/owner');
eq(hone.ellipsoid, shapes.ellipsoid, 'hone shares native methods');

// The framework bridge shares its C TRS evaluator; drawing remains hone's job.
{
  const pose=new Float32Array([1,2,3,0,0,0,2,3,4]), out=new Float32Array(12);
  ayatsuri.compose(pose,null,out);
  eq(out.join(','),'2,0,0,1,0,3,0,2,0,0,4,3','C root pose');
  const child=new Float32Array([1,2,3,0,0,0,1,1,1]);
  ayatsuri.compose(child,out,out);
  eq(out.join(','),'2,0,0,3,0,3,0,8,0,0,4,15','C parent/output alias');
  const memory=new Float32Array(16), sharedPose=memory.subarray(0,9), sharedOut=memory.subarray(4);
  sharedPose.set(pose);ayatsuri.compose(sharedPose,undefined,sharedOut);
  eq(sharedOut.join(','),'2,0,0,1,0,3,0,2,0,0,4,3','C overlapping JS views snapshot inputs');
  out.fill(37);const previous=out.join(',');
  pose[6]=0;throws(()=>ayatsuri.compose(pose,null,out),'INVALID_ARGUMENT','C zero scale');
  eq(out.join(','),previous,'invalid C input leaves output unchanged');pose[6]=2;
  const parent=new Float32Array([3e38,0,0,0,0,1,0,0,0,0,1,0]);
  throws(()=>ayatsuri.compose(pose,parent,out),'LIMIT_EXCEEDED','C float32 overflow');
  eq(out.join(','),previous,'C overflow leaves every output byte unchanged');
  parent[0]=NaN;throws(()=>ayatsuri.compose(pose,parent,out),'INVALID_ARGUMENT','C nonfinite parent');
  throws(()=>ayatsuri.compose(new Float32Array(8),null,out),'INVALID_ARGUMENT','C short pose');
  throws(()=>ayatsuri.compose(pose,null,new Float64Array(12)),'INVALID_ARGUMENT','C narrow output');
  if(typeof out.buffer.sliceToImmutable==='function'){
    const readonly=new Float32Array(out.buffer.sliceToImmutable());
    throws(()=>ayatsuri.compose(pose,null,readonly),'INVALID_ARGUMENT','C readonly output');
  }
  ayatsuri.compose(child,null,out);
  eq(out[3],1,'C evaluation recovers without hidden state');
}

// project: x, y over w; w <= 0 gives NaN; ranges; Float32Array only.
{
  const src = new Float32Array([1, 2, 3, 4, 5, 6, 0, 0, -1]), dst = new Float32Array(6);
  // x' = (2x + 10) / (z + 2), y' = (3y) / (z + 2)
  shapes.project([2, 0, 0, 10, 0, 3, 0, 0, 0, 0, 1, 2], src, dst);
  near(dst[0], 12 / 5, 'project x'); near(dst[1], 6 / 5, 'project y');
  near(dst[2], 18 / 8, 'project x 2'); near(dst[3], 15 / 8, 'project y 2');
  eq(Number.isNaN(dst[4]) && Number.isNaN(dst[5]), false, 'w = 1 is in front');
  shapes.project([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0], src, dst, 2, 1);
  eq(Number.isNaN(dst[4]) && Number.isNaN(dst[5]), true, 'w <= 0 is NaN');
  throws(() => shapes.project([1, 2, 3], src, dst), 'INVALID_ARGUMENT', 'short matrix');
  throws(() => shapes.project(new Float32Array(12), src, [0, 0]), 'INVALID_ARGUMENT', 'plain target');
  throws(() => shapes.project(new Float32Array(12), src, new Float64Array(6)), 'INVALID_ARGUMENT', 'Float64Array target');
  throws(() => shapes.project(new Float64Array(12), src, dst), 'INVALID_ARGUMENT', 'Float64Array matrix');
  throws(() => shapes.fill(new Float64Array(8), [0, 4], [], 1), 'INVALID_ARGUMENT', 'Float64Array points');
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

// shade: body albedo is independent of light/view, and only the requested
// output range changes. This kernel never needs an open procedural frame.
{
  const N = new Int16Array([0, 0, 32767, 32767, 0, 0, 0, 0, -32767]);
  const A = new Uint16Array([0xf800, 0x07e0, 0x001f]);
  const T = new Uint8Array([0, 128, 255]);
  const L = new Float32Array([0, 0, 1, 0, 0, 1, 1, 1, 1]);
  const M = new Float32Array([0, 1, 0, 16, 0, 1, 0.5, 0.25]);
  const O = new Uint16Array(3);
  shapes.shade(N, A, T, L, M, O);
  eq(O[0], 0xf800, 'front diffuse red'); eq(O[1], 0, 'side diffuse black');
  eq(O[2], 0, 'back diffuse black'); eq(A[1], 0x07e0, 'albedo unchanged');
  M[0] = 1; M[1] = 0;
  shapes.shade(N, A, T, L, M, O);
  eq(O.join(','), A.join(','), 'ambient returns body colours at every orientation');
  O.fill(0x1234); M[0] = 0; M[1] = 1;
  shapes.shade(N, A, T, L, M, O, 1, 1);
  eq(O.join(','), '4660,0,4660', 'partial range only');
  shapes.shade(N, A, T, L, M, O, 3, 0);
  eq(O.join(','), '4660,0,4660', 'empty final range');
  throws(() => shapes.shade(N, A, T, L, M, O, 3, 1), 'LIMIT_EXCEEDED', 'shade range');
  const n2 = new Int16Array([0, 0, 32767]);
  const a2 = new Uint16Array([0]); const t2 = new Uint8Array([0]); const o2 = new Uint16Array(1);
  M[1] = 0; M[2] = 1;
  shapes.shade(n2, a2, t2, L, M, o2);
  eq(o2[0], 0xffff, 'front broad specular');
  L[3] = 1; L[5] = 0; M[3] = 4;
  shapes.shade(n2, a2, t2, L, M, o2);
  eq(o2[0] < 0xffff && o2[0] > 0, true, 'view moves highlight');
  L[0] = 0; L[2] = -1; L[3] = 0; L[5] = 1;
  M[2] = 0; M[4] = 1; M[5] = 1; M[6] = 0; M[7] = 0;
  shapes.shade(n2, a2, t2, L, M, o2);
  eq(o2[0], 0xf800, 'thin warm back transmission');
  t2[0] = 255;
  shapes.shade(n2, a2, t2, L, M, o2);
  eq(o2[0], 0, 'thick sample suppresses transmission');
  // Specular strength is independent of thickness, not a thick-skin rule.
  L[2] = 1; M[2] = 1;
  shapes.shade(n2, a2, t2, L, M, o2);
  eq(o2[0], 0xffff, 'thickness does not determine specular');
  const inPlace = new Uint16Array([0xf800, 0x07e0, 0x001f]);
  M[0] = 1; M[2] = 0; M[4] = 0;
  shapes.shade(N, inPlace, T, L, M, inPlace);
  eq(inPlace.join(','), A.join(','), 'exact albedo alias supported');
  const raw = new ArrayBuffer(12), overlapN = new Int16Array(raw), overlapO = new Uint16Array(raw, 2, 2);
  overlapN.set([0, 0, 32767, 0, 0, 32767]);
  throws(() => shapes.shade(overlapN, new Uint16Array(2), new Uint8Array(2), L, M, overlapO),
    'INVALID_ARGUMENT', 'overlapping normals/output refused');
  O.fill(0x1234); const badN = new Int16Array(N); badN[6] = badN[7] = badN[8] = 0;
  throws(() => shapes.shade(badN, A, T, L, M, O), 'INVALID_ARGUMENT', 'late zero normal');
  eq(O.join(','), '4660,4660,4660', 'invalid shade is atomic');
  shapes.shade(badN, A, T, L, M, O, 0, 1);
  eq(O[0], 0xf800, 'unrequested bad normals do not prevent a local update');
  const offsetsN = new Int16Array(9); offsetsN.set([0, 0, 32767], 3);
  const offsetsO = new Uint16Array(3); offsetsO.fill(0x1234);
  shapes.shade(offsetsN.subarray(3, 6), A.subarray(0, 1), T.subarray(0, 1), L, M,
    offsetsO.subarray(1, 2));
  eq(offsetsO.join(','), '4660,63488,4660', 'typed view byte offsets respected');
  const tinyL = new Float32Array(L); tinyL[2] = 1e-40;
  shapes.shade(N, A, T, tinyL, M, O);
  eq(O.join(','), A.join(','), 'nonzero subnormal light is normalized');
  badN[8] = -32768;
  throws(() => shapes.shade(badN, A, T, L, M, O), 'INVALID_ARGUMENT', 'SNORM reserved minimum');
  throws(() => shapes.shade(new Float32Array(9), A, T, L, M, O), 'INVALID_ARGUMENT', 'normal narrow type');
  throws(() => shapes.shade(N, A, new Uint8Array(2), L, M, O), 'INVALID_ARGUMENT', 'thickness length');
  M[3] = NaN;
  throws(() => shapes.shade(N, A, T, L, M, O), 'INVALID_ARGUMENT', 'material finite');
  M[3] = 16; L[0] = L[1] = L[2] = 0;
  throws(() => shapes.shade(N, A, T, L, M, O), 'INVALID_ARGUMENT', 'nonzero light');
  throws(() => shapes.shade(new Int16Array(3075), new Uint16Array(1025), new Uint8Array(1025), L, M,
    new Uint16Array(1025)), 'LIMIT_EXCEEDED', 'shade sample cap');
}
