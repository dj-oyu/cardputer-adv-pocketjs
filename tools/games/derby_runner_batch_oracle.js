// Host only. Actual app rin() expression against real QuickJS/native boundary,
// bit for bit before procedural float conversion, through the existing hook.
(function () {
  const S = pocket.derby, native = S.runners;
  let record, checked = false, calls = 0, runners = 0;
  const sin = Math.sin;
  const dr = (name, ...inputs) => { if (+name.slice(1)) record.push(+name.slice(1), ...inputs); };
  const reference = __REFERENCE_RIN__;
  const require = (v, m) => { if (!v) throw Error('RUNNER_BATCH ' + m); };
  const reject = (buffer, count, code) => {
    let error;
    __serbegin();
    try { native(buffer, count); } catch (e) { error = e; }
    require(error, 'invalid input accepted');
    if (code) require(error.code === code, 'original draw error not propagated: ' + error);
    __sercmp([], false);
  };
  S.runners = function (buffer, count) {
    const rows = new Float64Array(buffer);
    record = [];
    for (let i = 0; i < count; ++i) {
      const j = i * 5;
      reference(rows[j], rows[j+1], rows[j+2], rows[j+3], rows[j+4]);
    }
    __serbegin();
    const result = native(buffer, count);
    __sercmp(record, false);
    ++calls; runners += count;
    if (!checked && count && rows[0]) {
      checked = true;
      reject(new Float64Array(40), 0); // API accepts a whole ArrayBuffer only
      reject(new ArrayBuffer(8), 0);
      reject(new ArrayBuffer(0), 0);
      reject(buffer, -1); reject(buffer, 9); reject(buffer, .5); reject(buffer, NaN);
      const bad = new Float64Array(40);
      for (const n of [NaN, Infinity, -Infinity]) {
        bad.fill(0); bad[9] = n;
        reject(bad.buffer, 2); // invalid later record fails before any draw
      }
      for (const n of [-1, .5, 2147483648]) {
        bad.fill(0); bad[5] = n; reject(bad.buffer, 2);
      }
      bad.fill(0); bad[0] = 2147483647;
      reject(bad.buffer, 1, 'CLOSED');
      bad.fill(0); bad[0] = rows[0]; bad[2] = Number.MAX_VALUE;
      reject(bad.buffer, 1, 'INVALID_ARGUMENT'); // derived draw overflow
      // Valid first draw followed by a stale handle: sequential partial
      // submission is deliberate, and the original draw exception survives.
      for (let j = 0; j < 5; ++j) bad[j] = rows[j];
      bad[5] = 2147483647;
      record = [];
      reference(bad[0], bad[1], bad[2], bad[3], bad[4]);
      __serbegin(); let error;
      try { native(bad.buffer, 2); } catch (e) { error = e; }
      require(error && error.code === 'CLOSED', 'partial draw error');
      __sercmp(record, false);
      if (typeof ArrayBuffer.prototype.transfer === 'function') {
        const detached = new ArrayBuffer(320); detached.transfer(); reject(detached, 0);
      }
      const resized = new ArrayBuffer(320, {maxByteLength: 640});
      if (typeof resized.resize === 'function') {
        resized.resize(640); reject(resized, 0);
        resized.resize(320); native(resized, 0); // re-read size on every call
      }
      console.log('RUNNER_BATCH boundary negatives PASS');
    }
    return result;
  };
  globalThis.__runnerBatchStats = () => ({calls, runners, checked});
  globalThis.__runnerBatchFuzz = () => {
    let state = 19;
    const random = () => ((state = (Math.imul(state, 1664525) + 1013904223) >>> 0) / 4294967296);
    const rows = new Float64Array(40);
    __serdry(1);
    try {
      for (let k = 0; k < 2000; ++k) {
        const count = k % 9;
        record = [];
        for (let i = 0; i < count; ++i) {
          const j = i * 5;
          rows[j] = k % 7 ? i + 1 : 0;
          rows[j+1] = (random() - .5) * 800;
          rows[j+2] = k % 11 ? random() * 100 : -0;
          rows[j+3] = (random() - .5) * 400;
          rows[j+4] = (random() - .5) * 2e6;
          reference(rows[j], rows[j+1], rows[j+2], rows[j+3], rows[j+4]);
        }
        __serbegin(); native(rows.buffer, count); __sercmp(record, true);
      }
    } finally { __serdry(0); }
    console.log('RUNNER_BATCH 2000 randomized native-boundary cases complete');
  };
})();
