// pocket.kasane.shiki (spec v1.3 sec.9) self-check: README.md. The same file
// runs in the host harness (tools/kasane_contract/run_pocket_shiki_api_qjs.py).
'use strict';
globalThis.shikiApiTest = function (pocket, log) {
  const S = pocket.kasane.shiki;
  let pass = 0, fail = 0;
  const code = e => e && (e.code || String(e.message).split(' ')[0]);
  const ok = (name, c) => { if (c) pass++; else { fail++; log('SA FAIL ' + name); } };
  const throws = (name, want, f) => {
    try { f(); fail++; log('SA FAIL ' + name + ' (no error)'); }
    catch (e) {
      const got = want === 'TypeError' ? (e instanceof TypeError && !e.code ? 'TypeError' : code(e)) : code(e);
      if (got === want) pass++; else { fail++; log('SA FAIL ' + name + ' got ' + got + ' ' + e.message); }
    }
  };
  // sec.3 bytes: regions [type,q,k,write], code [op,dst,a,b,imm,aux].
  const asm = (regions, code, inputs) => {
    inputs = inputs || [];
    const head = 12 + inputs.length + 4 * regions.length, body = (head + 3) & ~3;
    const b = new Uint8Array(body + 12 * code.length), dv = new DataView(b.buffer);
    b.set([83, 75, 73, 49, 1, 0, code.length, inputs.length, 0, regions.length, 0, 0]);
    b.set(inputs, 12);
    regions.forEach((r, k) => b.set(r, 12 + inputs.length + 4 * k));
    code.forEach((i, k) => { const o = body + 12 * k; b.set(i.slice(0, 4), o); dv.setInt32(o + 4, i[4] || 0, true); dv.setUint16(o + 8, i[5] || 0, true); });
    return b;
  };
  const caps = pocket.capabilities && pocket.capabilities.get && pocket.capabilities.get('kasane.shiki');
  if (caps) ok('capability', caps.supported === true && caps.available === true && caps.limits.maxRecords === 65535);
  // Frames and the engine's state are taken on the first draw and kept: warm
  // them up so the heap's baseline holds them.
  const warm = S.register(asm([], [[0, 0, 0, 0, 1]]));
  S.beginFrame(0); S.draw(warm); S.unregister(warm);
  const h0 = S.heap();
  ok('heap', h0.bytes === 49152 && h0.used >= 0 && h0.largest > 0 && h0.largest <= h0.bytes);
  // data(): windows and their checks (sec.9.3, v1.4 field planes: 3 records
  // of 2 fields are 2 planes of 8, the window holds all 16 elements).
  const w = S.data({type: 'i32', q: 0, records: 3, record: 2});
  ok('window', w instanceof Int32Array && w.length === 16 && w.every(v => v === 0));
  throws('type', 'INVALID_ARGUMENT', () => S.data({type: 'f32', q: 0, records: 2, record: 1}));
  throws('q', 'INVALID_ARGUMENT', () => S.data({type: 'i16', q: 31, records: 2, record: 1}));
  throws('record', 'INVALID_ARGUMENT', () => S.data({type: 'i16', q: 0, records: 2, record: 0}));
  throws('whole', 'INVALID_ARGUMENT', () => S.data({type: 'i16', q: 0, records: 2.5, record: 2}));
  throws('empty', 'INVALID_ARGUMENT', () => S.data({type: 'i16', q: 0, records: 0, record: 1}));
  throws('v1.3 shape', 'INVALID_ARGUMENT', () => S.data({type: 'i16', q: 0, length: 2, record: 1}));
  throws('budget', 'LIMIT_EXCEEDED', () => S.data({type: 'i32', q: 0, records: 20000, record: 1}));
  // fields(): one view per field, R long, over the planes.
  const [fa, fb] = S.fields(w);
  ok('fields', fa instanceof Int32Array && fa.length === 3 && fb.length === 3 && fa.buffer === w.buffer);
  // EACH; FETCH r0..r1; ADD r2 = r0 + r1; PUT r2 into field 1.
  fa.set([1, 2, 3]); fb.set([10, 20, 30]);
  w[3] = 77; w[15] = 88;                                   // padding: never touched
  const prog = asm([[2, 0, 2, 1]], [[23, 0, 0, 0], [24, 0, 0, 0, 0, 2], [2, 2, 0, 1], [25, 2, 0, 1, 0, 1], [6]]);
  const h = S.register(prog, [], {data: [w]});
  ok('handle', typeof h === 'number' && h > 0);
  S.beginFrame(0);
  S.draw(h);
  ok('put', fb.join() === '11,22,33' && fa.join() === '1,2,3' && w[8] === 11);
  ok('padding', w[3] === 77 && w[15] === 88);
  throws('fields of a view', 'INVALID_ARGUMENT', () => S.fields(fa));
  ok('heap used', S.heap().used > h0.used);
  throws('busy', 'BUSY', () => S.release(w));
  throws('other view', 'INVALID_ARGUMENT', () => S.release(new Int32Array(w.buffer)));
  throws('mismatch', 'INVALID_ARGUMENT', () => S.register(asm([[0, 0, 2, 1]], [[23, 0, 0, 0], [6]]), [], {data: [w]}));
  throws('other view bound', 'INVALID_ARGUMENT', () => S.register(prog, [], {data: [fa]}));
  throws('count', 'INVALID_ARGUMENT', () => S.register(prog, [], {data: []}));
  // The transfer gate (sec.9.3): every length, and the window survives.
  throws('transfer', 'TypeError', () => w.buffer.transfer());
  throws('transfer 0', 'TypeError', () => w.buffer.transfer(0));
  throws('transfer 8', 'TypeError', () => w.buffer.transfer(8));
  throws('fixed', 'TypeError', () => w.buffer.transferToFixedLength());
  ok('kept', w.length === 16 && w[8] === 11);
  // Handles: the procedural calls do not see a Shiki handle.
  throws('procedural draw', 'CLOSED', () => pocket.kasane.procedural.draw(h, []));
  S.unregister(h);
  throws('stale', 'CLOSED', () => S.unregister(h));
  S.release(w);
  ok('detached', w.length === 0 && fa.length === 0);
  throws('released', 'CLOSED', () => S.release(w));
  throws('released fields', 'CLOSED', () => S.fields(w));
  throws('released bind', 'CLOSED', () => S.register(prog, [], {data: [w]}));
  ok('heap back', S.heap().used === h0.used);
  // 16 regions a session.
  const many = [];
  for (let k = 0; k < 16; k++) many.push(S.data({type: 'u16', q: 0, records: 1, record: 1}));
  throws('16 regions', 'LIMIT_EXCEEDED', () => S.data({type: 'u16', q: 0, records: 1, record: 1}));
  many.forEach(x => S.release(x));
  // Drawing: PLOT from inputs (Q16), LIMIT and INVALID as errors.
  const plot = S.register(asm([], [[1, 0, 0], [1, 1, 1], [8, 0, 0, 1, 0, 2016]], [16, 16]));
  S.beginFrame(0); S.draw(plot, 120, 60);
  S.beginFrame(0);
  throws('invalid', 'INVALID_ARGUMENT', () => S.draw(plot, 9999, 60));
  S.beginFrame(0);
  throws('non-finite', 'INVALID_ARGUMENT', () => S.draw(plot, NaN, 60));
  const spin = S.register(asm([], [[5, 0, 0, 0, 255], [5, 0, 0, 0, 255], [6], [6]]));
  S.beginFrame(0);
  throws('limit', 'LIMIT_EXCEEDED', () => S.draw(spin));
  // drawMany (prototype, shiki-bundle.md sec.5): INPUT r0, PUT it into a
  // one-record i32 Q16 region; the region keeps the last draw's input.
  const box = S.data({type: 'i32', q: 16, records: 1, record: 1});
  const put = S.register(asm([[2, 16, 1, 1]], [[23, 0, 0, 0], [1, 0, 0], [25, 0, 0, 0, 0, 1], [6]], [16]), [], {data: [box]});
  S.beginFrame(0);
  S.drawMany(new Uint32Array([put, put]), new Float64Array([1.5, 2.25]), 1);
  ok('many real', box[0] === 2.25 * 65536);
  S.drawMany(new Uint32Array([put]), new Int32Array([98304]), 1);
  ok('many fixed', box[0] === 98304);
  S.draw(put, 0.5);
  ok('many = draw', box[0] === 32768);
  throws('many length', 'INVALID_ARGUMENT', () => S.drawMany(new Uint32Array([put, put]), new Float64Array([1]), 1));
  throws('many array', 'INVALID_ARGUMENT', () => S.drawMany([put], new Float64Array([1]), 1));
  throws('many type', 'INVALID_ARGUMENT', () => S.drawMany(new Uint32Array([put]), new Float32Array([1]), 1));
  S.beginFrame(0);
  throws('many stale', 'CLOSED', () => S.drawMany(new Uint32Array([put, 0x3fffffff]), new Float64Array([1, 1]), 1));
  S.unregister(put); S.release(box);
  S.beginFrame(0); S.draw(plot, 120, 60); S.commit();   // one commit a turn
  S.unregister(plot); S.unregister(spin);
  throws('bad program', 'INVALID_ARGUMENT', () => S.register(new Uint8Array([1, 2, 3])));
  throws('no built-in', 'INVALID_ARGUMENT', () => S.register('derby.none'));
  ok('shared', S.beginFrame === pocket.kasane.procedural.beginFrame && S.grid === pocket.kasane.grid);
  return [pass, fail];
};
if (typeof pocket !== 'undefined' && pocket.kasane && pocket.kasane.shiki) {
  let done = false;
  globalThis.frame = function () {
    if (done) return;
    done = true;
    const [p, f] = shikiApiTest(pocket, s => console.log(s));
    console.log('SA END pass=' + p + ' fail=' + f);
  };
}
