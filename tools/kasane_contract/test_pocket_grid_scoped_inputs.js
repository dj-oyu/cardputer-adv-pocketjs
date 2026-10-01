// Functional capture/lifetime regression tests against the real QuickJS runtime.
// Getters mutate synchronously; concurrent mutation is outside this contract.
(() => {
  const host = gridTestHost;
  const grid = kasane.grid;
  const assert = (condition, message) => {
    if (!condition) throw Error(message);
  };
  const throws = (body, message, text) => {
    let caught = false;
    try { body(); } catch (error) {
      caught = true;
      if (text) assert(String(error).includes(text), message + ': ' + error);
    }
    assert(caught, message + ': did not throw');
  };
  const invalidInputs = (body, method, message) => {
    let caught = false;
    try { body(); } catch (error) {
      caught = true;
      const expected = 'TypeError: INVALID_ARGUMENT kasane.grid.' + method +
        ': grid bind or execution failed';
      assert(error instanceof TypeError && String(error) === expected,
        message + ': ' + error);
    }
    assert(caught, message + ': did not throw');
  };
  const sameException = (body, marker, message) => {
    let caught = false;
    try { body(); } catch (error) {
      caught = true;
      assert(error === marker, message + ': exception was replaced');
    }
    assert(caught, message + ': did not throw');
  };
  const filled = value => Array(8).fill(value);
  const at = gridFold.index({x: 1, y: 8});
  const shifted = gridFold.index({base: {param: 0}, x: 1, y: 8});
  const shape = {width: 8, height: 1, tapWidth: 1, tapHeight: 1, output: at};
  const copy = grid.register(gridFold.fold(shape,
    g => g.add(g.acc, g.load(0, shifted))));
  const sum = grid.register(gridFold.fold(shape,
    g => g.add(g.acc, g.add(g.load(0, at), g.load(2, at)))));
  const recurrence = grid.register(gridFold.fold(shape,
    g => g.add(g.load(1, at), g.constant(1))));
  const images = new Map();
  const read = handle => host.pixels(images.get(handle));
  const expectPixels = (handle, expected, message) => {
    const actual = read(handle);
    assert(actual.length === expected.length &&
      actual.every((pixel, i) => pixel === (expected[i] & 65535)),
      message + ': ' + actual + ' != ' + expected);
  };
  const run = (handle, buffers, params, expected, options) => {
    const backend = options === undefined ? grid.run(handle, buffers, params) :
      grid.run(handle, buffers, params, options);
    if (!images.has(handle)) images.set(handle, grid.resource(handle));
    expectPixels(handle, expected, 'current input pixels');
    host.ack(true);
    assert(!host.pending(), 'successful ACK');
    return backend;
  };
  const measure = (handle, buffers, params, repeats = 2, strategy = 'AUTO') => {
    const result = grid.measure(handle, buffers, params, repeats, strategy);
    assert(result.equal && result.repeats === repeats &&
      result.strategy === strategy && result.scalarUs >= 0 && result.pieUs >= 0,
      'measurement result');
    return result;
  };

  // Measurement has its own current inputs and does not need a previous run.
  const initial = new Int16Array(16).fill(7);
  const beforeRun = grid.measure(copy, {0: initial}, [8]);
  assert(beforeRun.equal && beforeRun.repeats === 8, 'measure default repeats');
  assert(grid.profile(copy).runs === 0, 'measure before run has no run profile');
  throws(() => grid.resource(copy), 'measure must not create a committed image');
  for (const strategy of ['AUTO', 'GATHER', 'AFFINE'])
    measure(copy, {0: initial}, [0], 2, strategy);
  throws(() => grid.measure(copy), 'missing explicit buffers');
  throws(() => grid.measure(copy, 2), 'old repeat-only signature');
  throws(() => grid.measure(copy, 2, 'GATHER'), 'old strategy signature');
  throws(() => grid.measure(copy, {0: initial}, [0], 0), 'zero repeats');
  throws(() => grid.measure(copy, {0: initial}, [0], 17), 'repeat limit');
  throws(() => grid.measure(copy, {0: initial}, [0], 1, 'BAD'), 'unknown strategy');
  throws(() => grid.measure(copy, {0: initial}, [9]), 'current parameter bounds');
  throws(() => grid.measure(copy, {0: new Int16Array(7)}, [0]),
    'current input bounds');
  run(copy, {0: initial}, [0], filled(7));

  // A later map getter and a parameter getter both precede pointer capture.
  let input = new Int16Array(16).fill(1);
  const other = new Int16Array(8).fill(3);
  run(sum, {0: input, get 2() { input.fill(20); return other; }}, [], filled(23));
  const params = [0];
  Object.defineProperty(params, 0, {get() { input.fill(31); return 8; }});
  run(copy, {0: input}, params, filled(31));
  input.fill(2);
  run(copy, {0: input}, [0], filled(42), {
    get backend() { input.fill(42); return 'AUTO'; }
  });
  const order = [];
  run(copy, {
    get 0() { order.push('source'); return input; },
    get 2() { order.push('later'); input.fill(51); return undefined; },
    get 7() { order.push('last'); input.fill(52); return null; }
  }, [0], filled(52));
  assert(order.join(',') === 'source,later,last', 'map getters collected once');

  // Collection roots the returned view once; changing the map cannot replace it.
  const collected = new Int16Array(8).fill(53);
  const replacement = new Int16Array(8).fill(54);
  const replaceMap = {0: collected, get 7() {
    this[0] = replacement;
    collected.fill(55);
    return undefined;
  }};
  run(copy, replaceMap, [0], filled(55));
  assert(replaceMap[0] === replacement, 'map replacement actually occurred');
  let measureReads = 0;
  measure(copy, {get 0() {
    ++measureReads;
    return new Int16Array(8).fill(56);
  }, get 7() { host.gc(); return undefined; }}, [0]);
  assert(measureReads === 1, 'measure fetched a source more than once');

  // Every supplied Int16 view is rooted while subsequent getters run GC.
  run(copy, {
    get 0() {
      const ephemeral = new Int16Array(8).fill(61);
      ephemeral.self = ephemeral;
      return ephemeral;
    },
    get 2() { host.gc(); return undefined; }
  }, [0], filled(61));
  host.gc();
  const freesBefore = host.externalFrees();
  run(copy, {
    get 0() {
      const ephemeral = new Int16Array(host.externalOdd(8, 62));
      ephemeral.self = ephemeral;
      return ephemeral;
    },
    get 2() {
      host.gc();
      assert(host.externalFrees() === freesBefore, 'ephemeral view lost its root');
      return undefined;
    }
  }, [0], filled(62));
  host.gc();
  assert(host.externalFrees() === freesBefore + 1, 'run retained external input');

  // Read-only overlapping inputs and even, non-vector-aligned offsets are valid.
  const aliased = new Int16Array(16);
  for (let i = 0; i < aliased.length; ++i) aliased[i] = i * 11 - 50;
  const untouched = Array.from(aliased);
  run(sum, {0: aliased.subarray(0, 8), 2: aliased.subarray(1, 9)}, [],
    untouched.slice(0, 8).map((n, i) => n + untouched[i + 1]));
  assert(Array.from(aliased).join() === untouched.join(), 'aliased input modified');
  for (const offset of [2, 4, 6, 10, 14]) {
    const backing = new ArrayBuffer(48);
    const view = new Int16Array(backing, offset, 8);
    for (let i = 0; i < 8; ++i) view[i] = i * 101 - 300;
    const expected = Array.from(view);
    assert(run(copy, {0: view}, [0], expected) === 'PIE', 'offset PIE route');
    run(copy, {0: view}, [0], expected, {backend: 'SCALAR'});
    for (const strategy of ['AUTO', 'GATHER', 'AFFINE'])
      measure(copy, {0: view}, [0], 2, strategy);
    assert(Array.from(view).join() === expected.join(), 'offset input modified');
  }

  // Native metadata extraction must not invoke shadowed guest properties.
  const shadowed = new Int16Array(8).fill(65);
  for (const property of ['buffer', 'byteLength', 'byteOffset', 'length'])
    Object.defineProperty(shadowed, property, {get() {
      throw Error('guest metadata getter invoked: ' + property);
    }});
  run(copy, {0: shadowed}, [0], filled(65));
  measure(copy, {0: shadowed}, [0]);

  // Capture uses current RAB lengths, including tracking views at an offset.
  for (const offset of [0, 4]) {
    const buffer = new ArrayBuffer(offset + 8, {maxByteLength: offset + 64});
    const view = new Int16Array(buffer, offset);
    run(copy, {0: view, get 2() {
      buffer.resize(offset + 16); view.fill(71 + offset); return undefined;
    }}, [0], filled(71 + offset));
    buffer.resize(offset + 32);
    run(copy, {0: view, get 2() {
      buffer.resize(offset + 16); view.fill(81 + offset); return undefined;
    }}, [0], filled(81 + offset));
    buffer.resize(offset + 8);
    measure(copy, {0: view, get 2() {
      buffer.resize(offset + 16); view.fill(91 + offset); return undefined;
    }}, [0]);
    const shrinkParams = [0];
    Object.defineProperty(shrinkParams, 0, {get() {
      buffer.resize(offset + 14); return 0;
    }});
    throws(() => grid.run(copy, {0: view}, shrinkParams),
      'tracking shrink must reject insufficient current length');
    buffer.resize(offset + 32);
    throws(() => grid.measure(copy, {0: view, get 7() {
      buffer.resize(offset + 14); return undefined;
    }}, [0]), 'measure tracking shrink current length');
  }
  {
    const buffer = new ArrayBuffer(32, {maxByteLength: 64});
    const fixed = new Int16Array(buffer, 4, 8);
    invalidInputs(() => grid.run(copy, {0: fixed, get 2() {
      buffer.resize(18); return undefined;
    }}, [0]), 'run', 'fixed RAB became out of bounds');
    assert(fixed.length === 0, 'fixed RAB actually out of bounds');
    // A temporarily OOB view can become valid before the capture boundary.
    run(copy, {0: fixed, get 7() {
      buffer.resize(20); fixed.fill(101); return undefined;
    }}, [0], filled(101));
    invalidInputs(() => grid.measure(copy, {0: fixed, get 2() {
      buffer.resize(18); return undefined;
    }}, [0]), 'measure', 'measure fixed RAB became out of bounds');
  }
  {
    const buffer = new ArrayBuffer(32, {maxByteLength: 64});
    const tracking = new Int16Array(buffer, 8);
    buffer.resize(4);
    run(copy, {0: tracking, get 2() {
      buffer.resize(24); tracking.fill(102); return undefined;
    }}, [0], filled(102));
  }
  {
    const buffer = new ArrayBuffer(16386, {maxByteLength: 16386});
    const tracking = new Int16Array(buffer);
    run(copy, {0: tracking, get 2() {
      buffer.resize(16); tracking.fill(103); return undefined;
    }}, [0], filled(103));
    throws(() => grid.run(copy, {0: tracking, get 2() {
      buffer.resize(16386); return undefined;
    }}, [0]), 'input limit uses grown current length');
  }

  // The aggregate cell cap counts every supplied view, even an unused input.
  const halfCap = new Int16Array(4096).fill(104);
  run(copy, {0: halfCap, 2: new Int16Array(4096)}, [0], filled(104));
  measure(copy, {0: halfCap, 2: new Int16Array(4096)}, [0]);
  throws(() => grid.run(copy, {0: halfCap, 2: new Int16Array(4097)}, [0]),
    'aggregate input cap plus one');
  throws(() => grid.measure(copy, {0: halfCap, 2: new Int16Array(4097)}, [0]),
    'measure aggregate input cap plus one');

  // Shared inputs are snapshots for one call, including growable offset views.
  for (const offset of [0, 2]) {
    const buffer = new SharedArrayBuffer(offset + 16);
    const view = new Int16Array(buffer, offset, 8).fill(111 + offset);
    run(copy, {0: view}, [0], filled(111 + offset));
    measure(copy, {0: view}, [0]);
    assert(view.every(n => n === 111 + offset), 'shared input modified');
    const growing = new SharedArrayBuffer(offset + 8,
      {maxByteLength: offset + 32});
    const tracking = new Int16Array(growing, offset);
    run(copy, {0: tracking, get 2() {
      growing.grow(offset + 16); tracking.fill(121 + offset); return undefined;
    }}, [0], filled(121 + offset));
    growing.grow(offset + 32);
    measure(copy, {0: tracking}, [8]);
    const fixed = new Int16Array(growing, offset, 8).fill(131 + offset);
    run(copy, {0: fixed}, [0], filled(131 + offset));
  }
  {
    let external = new Int16Array(host.externalOdd(8, 141));
    run(copy, {0: external}, [0], filled(141));
    measure(copy, {0: external}, [0]);
    const freeCount = host.externalFrees();
    external = null;
    host.gc();
    assert(host.externalFrees() === freeCount + 1, 'measure retained external input');
    expectPixels(copy, filled(141), 'external input released after run and measure');
  }
  const empty = new Int16Array(host.emptyExternal());
  assert(empty.length === 0, 'NULL external constructor makes an empty view');
  run(copy, {0: new Int16Array(8).fill(142), 2: empty}, [0], filled(142));
  throws(() => grid.run(copy, {0: empty}, [0]), 'empty required input rejected');

  // Input failures retain the established public wrapper after getter cleanup.
  // The backend getter keeps its original direct-propagation behavior.
  const marker = {marker: 'getter failure'};
  invalidInputs(() => grid.run(copy, {get 0() { throw marker; }}, [0]),
    'run', 'first map getter');
  invalidInputs(() => grid.run(copy, {0: initial, get 7() { throw marker; }}, [0]),
    'run', 'late map getter');
  const throwingParams = [0];
  Object.defineProperty(throwingParams, 0, {get() { throw marker; }});
  invalidInputs(() => grid.run(copy, {0: initial}, throwingParams), 'run',
    'parameter getter');
  sameException(() => grid.run(copy, {0: initial}, [0],
    {get backend() { throw marker; }}), marker, 'backend getter');
  invalidInputs(() => grid.measure(copy, {0: initial, get 7() { throw marker; }}, [0]),
    'measure', 'measure map getter');
  invalidInputs(() => grid.measure(copy, {0: initial}, throwingParams), 'measure',
    'measure parameter getter');
  run(copy, {get 0() {
    throws(() => grid.run(copy, {0: initial}, [0]), 'recursive run', 'BUSY');
    throws(() => grid.measure(copy, {0: initial}, [0]), 'recursive measure', 'BUSY');
    return initial;
  }}, [0], filled(7));
  measure(copy, {get 0() {
    throws(() => grid.run(copy, {0: initial}, [0]), 'run within measure', 'BUSY');
    return initial;
  }}, [0]);
  for (const invalid of [new Uint16Array(8), new Uint8Array(16),
    new DataView(new ArrayBuffer(16)), new Proxy(new Int16Array(8), {}), [], {}]) {
    invalidInputs(() => grid.run(copy, {0: invalid}, [0]), 'run', 'non-Int16 view');
    invalidInputs(() => grid.measure(copy, {0: invalid}, [0]), 'measure',
      'measure non-Int16 view');
  }

  for (const measuring of [false, true]) {
    let laterReads = 0;
    const invalidMap = {0: new Uint8Array(16), get 2() {
      ++laterReads;
      throw Error('later getter must not be called after invalid input type');
    }};
    throws(() => measuring ? grid.measure(copy, invalidMap, [0]) :
      grid.run(copy, invalidMap, [0]), 'early input type rejection');
    assert(laterReads === 0, 'invalid type did not short-circuit collection');
  }

  // Detachment in a later getter is rejected, rather than using stale pointers.
  for (const measuring of [false, true]) {
    for (const fromParams of [false, true]) {
      const view = new Int16Array(8).fill(151);
      const buffers = fromParams ? {0: view} : {0: view, get 7() {
        host.detach(view.buffer); host.gc(); return undefined;
      }};
      const p = [0];
      if (fromParams) Object.defineProperty(p, 0, {get() {
        host.detach(view.buffer); host.gc(); return 0;
      }});
      invalidInputs(() => measuring ? grid.measure(copy, buffers, p) :
        grid.run(copy, buffers, p), measuring ? 'measure' : 'run',
        'later getter detachment');
      assert(view.length === 0, 'detachment occurred');
    }
  }

  // Saved image and ACK state remain independent of input lifetime and measure.
  grid.profile(copy);
  let pendingInput = new Int16Array(8).fill(161);
  grid.run(copy, {0: pendingInput}, [0]);
  assert(host.pending(), 'run has a pending candidate');
  host.detach(pendingInput.buffer);
  pendingInput = null;
  host.gc();
  expectPixels(copy, filled(161), 'pending image after input detach and GC');
  const routeBefore = JSON.stringify(grid.explain(copy));
  const invalidatesBefore = host.invalidates();
  const current = new Int16Array(16).fill(171);
  current.fill(172, 8);
  const freshParams = [0];
  Object.defineProperty(freshParams, 0, {get() { current.fill(173); return 8; }});
  measure(copy, {0: current}, freshParams);
  host.detach(current.buffer);
  host.gc();
  assert(host.pending(), 'measure must preserve pending ACK');
  assert(host.invalidates() === invalidatesBefore, 'measure must not invalidate');
  assert(JSON.stringify(grid.explain(copy)) === routeBefore, 'measure changed saved route');
  expectPixels(copy, filled(161), 'measure must not change pending image');
  assert(grid.profile(copy).runs === 1, 'measure must not add or reset run profile');
  host.ack(false);
  assert(host.pending(), 'failed ACK preserves candidate');
  expectPixels(copy, filled(161), 'failed ACK retains detached-input image');
  host.ack(true);
  assert(!host.pending(), 'successful ACK commits candidate');
  expectPixels(copy, filled(161), 'committed image after input detach and GC');
  measure(copy, {0: new Int16Array(8).fill(181)}, [0]);
  assert(grid.profile(copy).runs === 0 && !host.pending(), 'measure leaves committed state');
  expectPixels(copy, filled(161), 'measure must not change committed image');

  // DEST stays private. Unsupported DEST-dependent measurements reject without
  // seeding scratch from, or writing to, the last committed output generation.
  const privateDest = {get 1() { throw Error('DEST map entry was read'); }};
  assert(run(recurrence, privateDest, [], filled(1)) === 'scalar',
    'DEST-dependent scalar route');
  for (let i = 0; i < 2; ++i) {
    throws(() => grid.measure(recurrence, privateDest, [], 2),
      'DEST-dependent measure rejects without PIE');
    expectPixels(recurrence, filled(1), 'failed measure touched committed DEST');
  }
  grid.run(recurrence, privateDest, []);
  expectPixels(recurrence, filled(2), 'run recurrence uses committed DEST');
  throws(() => grid.measure(recurrence, privateDest, [], 2),
    'DEST-dependent measure rejects while pending');
  assert(host.pending(), 'failed measure cleared pending recurrence');
  expectPixels(recurrence, filled(2), 'failed measure touched candidate DEST');
  host.ack(true);
  assert(grid.profile(recurrence).runs === 2, 'rejected measure changed recurrence profile');

  // Native session teardown can invalidate a slot during a guest callback.
  // It must be revalidated before any captured input or plan is executed.
  throws(() => grid.run(copy, {0: initial}, [0], {get backend() {
    host.reset();
    return 'AUTO';
  }}), 'run rejects a handle invalidated by a backend getter');
  const afterReset = grid.register(gridFold.fold(shape,
    g => g.add(g.acc, g.load(0, shifted))));
  throws(() => grid.measure(afterReset, {0: initial, get 7() {
    host.reset();
    return undefined;
  }}, [0]), 'measure rejects a handle invalidated by a map getter');
})();
