// Executable JS expressions for MACs, dependent scans and DP recurrences.
// `expected` is computed independently of gridFold's generated instructions.
(function (global) {
  const {fold, index, view} = global.gridFold;
  const sat = (sum, shift) => Math.max(-32768, Math.min(32767,
    Math.floor(sum / 2 ** shift)));
  const examples = [];

  { // Named axes and parameter coefficients lower to the existing affine IR.
    const named = index({base: 3, x: -1,
                         y: {base: 2, scale: 4, param: 1}, tapX: 1});
    const expectedIR = [[3, 0, 255], [-1, 0, 255], [2, 4, 1],
                        [1, 0, 255], [0, 0, 255]];
    if (JSON.stringify(named) !== JSON.stringify(expectedIR))
      throw Error("named index changed IR");
    const spec = {width: 1, height: 1, tapWidth: 1, tapHeight: 1,
                  output: index({x: 1})};
    const fromView = fold(spec, g => g.add(g.acc,
      g.load(view({buffer: 0, base: 3, x: 1}))));
    const direct = fold(spec, g => g.add(g.acc,
      g.load(0, index({base: 3, x: 1}))));
    if (JSON.stringify(fromView.body) !== JSON.stringify(direct.body))
      throw Error("grid view changed index IR");
    for (const invalid of [
      () => index(3, -1, [2, 4, 1], 1, 0),
      () => view(0, {x: 1}),
      () => index({columns: 2}),
      () => index({x: [0, 1, 1]}),
      () => index({x: {scale: 2}}),
      () => index({y: {param: 8}}),
      () => fold({width: 1, height: 1, tapWidth: 1, tapHeight: 1,
                  shift: NaN, output: index({x: 1})},
                 g => g.add(g.acc, g.constant(1)))
    ]) {
      let rejected = false;
      try { invalid(); } catch (error) { rejected = error instanceof Error; }
      if (!rejected) throw Error("invalid grid notation accepted");
    }
  }

  { // 2x2 box downsample, also used by the grid device probe.
    const source = Array.from({length: 32 * 24}, (_, i) => (i * 73 + 19) % 65536 - 32768);
    const pixels = view({buffer: 0, x: 2, y: {param: 0, scale: 2},
                         tapX: 1, tapY: {param: 0}});
    let builds = 0;
    const program = fold({width: 16, height: 12, tapWidth: 2, tapHeight: 2,
                          shift: 2, output: index({x: 1, y: {param: 1}})}, g => {
      builds++;
      return g.add(g.acc, g.mul(g.load(pixels),
                                g.constant(1)));
    });
    if (builds !== 1) throw Error("fold body must run only at registration");
    const expected = [];
    for (let y = 0; y < 12; y++) for (let x = 0; x < 16; x++) {
      let sum = 0;
      for (let dy = 0; dy < 2; dy++) for (let dx = 0; dx < 2; dx++)
        sum += source[(2 * y + dy) * 32 + 2 * x + dx];
      expected.push(sat(sum, 2));
    }
    examples.push({name: "box2x2", program, params: [32, 16],
                   buffers: {0: source}, expected, pie: true});
  }

  { // A direct sum needs no explicit multiply in the IR.
    const source = Array.from({length: 8 * 4}, (_, i) => i % 13 - 6);
    const program = fold({width: 8, height: 1, tapWidth: 4, tapHeight: 1,
                          output: index({x: 1})}, g =>
      g.add(g.acc, g.load(0, index({x: 4, tapX: 1}))));
    const expected = [];
    for (let x = 0; x < 8; x++)
      expected.push(source.slice(4 * x, 4 * x + 4).reduce((a, b) => a + b, 0));
    examples.push({name: "directSum", program, buffers: {0: source}, expected, pie: true});
  }

  { // The body is no longer a fixed LOAD/CONST/MUL/ADD sequence.
    const source = Array.from({length: 8}, (_, x) => x - 4);
    const program = fold({width: 8, height: 1, tapWidth: 1, tapHeight: 1,
                          output: index({x: 1})}, g =>
      g.add(g.acc, g.mul(g.add(g.load(0, index({x: 1})),
                               g.constant(0)),
                         g.add(g.constant(1), g.constant(2)))));
    examples.push({name: "normalizedMac", program, buffers: {0: source},
                   expected: source.map(value => value * 3), pie: true});
  }

  { // A shared load supplies both operands of a dynamic square.
    const source = Array.from({length: 8}, (_, x) => x - 4);
    const program = fold({width: 8, height: 1, tapWidth: 1, tapHeight: 1,
                          output: index({x: 1})}, g => {
      const sample = g.load(0, index({x: 1}));
      return g.add(g.acc, g.mul(sample, sample));
    });
    examples.push({name: "sharedLoadSquare", program, buffers: {0: source},
                   expected: source.map(value => value * value), pie: true});
  }

  { // Two products of one sampled value factor into a single MAC.
    const source = Array.from({length: 8}, (_, x) => x - 4);
    const program = fold({width: 8, height: 1, tapWidth: 1, tapHeight: 1,
                          output: index({x: 1})}, g => {
      const sample = g.load(0, index({x: 1}));
      return g.add(g.acc,
                   g.add(g.mul(sample, g.constant(7)),
                         g.mul(sample, g.constant(-4))));
    });
    examples.push({name: "factoredSharedLoad", program, buffers: {0: source},
                   expected: source.map(value => value * 3), pie: true});
  }

  { // The coefficient sum needs two MACs when it exceeds signed 16-bit.
    const source = Array.from({length: 8}, (_, x) => x - 4);
    const program = fold({width: 8, height: 1, tapWidth: 1, tapHeight: 1,
                          output: index({x: 1})}, g => {
      const sample = g.load(0, index({x: 1}));
      return g.add(g.acc,
                   g.add(g.mul(sample, g.constant(30000)),
                         g.mul(sample, g.constant(30000))));
    });
    examples.push({name: "largeCoefficientDual", program,
                   buffers: {0: source},
                   expected: source.map(value => sat(value * 60000, 0)), pie: true});
  }

  { // A nonzero offset is a second, constant MAC term.
    const source = Array.from({length: 8}, (_, x) => x - 4);
    const program = fold({width: 8, height: 1, tapWidth: 1, tapHeight: 1,
                          output: index({x: 1})}, g =>
      g.add(g.acc, g.add(g.load(0, index({x: 1})),
                         g.constant(2))));
    examples.push({name: "offsetTapDual", program, buffers: {0: source},
                   expected: source.map(value => value + 2), pie: true});
  }

  { // Two different source positions feed the same QACC in original order.
    const source = Array.from({length: 8}, (_, x) => x * 3 - 10);
    const program = fold({width: 8, height: 1, tapWidth: 1, tapHeight: 1,
                          output: index({x: 1})}, g =>
      g.add(g.acc,
            g.add(g.mul(g.load(0, index({x: 1})), g.constant(2)),
                  g.load(0, index({base: 7, x: -1})))));
    examples.push({name: "mirrorBlendDual", program, buffers: {0: source},
                   expected: source.map((value, x) => value * 2 + source[7 - x]),
                   pie: true});
  }

  { // Both independent loads can use aligned contiguous PIE vectors.
    const source = Array.from({length: 16}, (_, x) => x * 5 - 30);
    const program = fold({width: 8, height: 1, tapWidth: 1, tapHeight: 1,
                          output: index({x: 1})}, g =>
      g.add(g.acc, g.add(
        g.mul(g.load(0, index({x: 1})), g.constant(2)),
        g.load(0, index({base: 8, x: 1})))));
    examples.push({name: "contiguousPairDual", program, buffers: {0: source},
                   expected: source.slice(0, 8).map((v, x) => v * 2 + source[x + 8]),
                   pie: true});
  }

  { // A lane-specific coefficient is followed by a reverse source load.
    const source = Array.from({length: 8}, (_, x) => x * 7 - 20);
    const weights = Array.from({length: 8}, (_, x) => 1 + x % 4);
    const program = fold({width: 8, height: 1, tapWidth: 1, tapHeight: 1,
                          output: index({x: 1})}, g =>
      g.add(g.acc, g.add(
        g.mul(g.load(0, index({x: 1})),
              g.load(2, index({x: 1}))),
        g.load(0, index({base: 7, x: -1})))));
    examples.push({name: "dynamicWeightDual", program,
                   buffers: {0: source, 2: weights},
                   expected: source.map((v, x) => v * weights[x] + source[7 - x]),
                   pie: true});
  }

  { // A loaded coefficient shared by all lanes uses coefficient broadcast.
    const source = Array.from({length: 8}, (_, x) => x * 9 - 15);
    const weight = [3];
    const program = fold({width: 8, height: 1, tapWidth: 1, tapHeight: 1,
                          output: index({x: 1})}, g =>
      g.add(g.acc, g.add(
        g.mul(g.load(0, index({x: 1})),
              g.load(2, index({}))),
        g.load(0, index({base: 7, x: -1})))));
    examples.push({name: "broadcastWeightDual", program,
                   buffers: {0: source, 2: weight},
                   expected: source.map((v, x) => v * 3 + source[7 - x]),
                   pie: true});
  }

  { // Interleaved samples exercise the single-tap VUNZIP lowering.
    const source = Array.from({length: 16}, (_, i) => i * 17 - 100);
    const program = fold({width: 8, height: 1, tapWidth: 1, tapHeight: 1,
                          output: index({x: 1})}, g =>
      g.add(g.acc, g.mul(g.load(0, index({x: 2})),
                         g.constant(3))));
    examples.push({name: "stride2Fixed", program, buffers: {0: source},
                   expected: Array.from({length: 8}, (_, x) => source[2*x] * 3),
                   pie: true});
  }

  { // One source cell is broadcast to all eight output lanes.
    const source = [7];
    const program = fold({width: 8, height: 1, tapWidth: 1, tapHeight: 1,
                          output: index({x: 1})}, g =>
      g.add(g.acc, g.mul(g.load(0, index({})),
                         g.constant(3))));
    examples.push({name: "broadcastSource", program, buffers: {0: source},
                   expected: Array(8).fill(21), pie: true});
  }

  { // Different per-lane weights retain the two-vector dynamic MAC path.
    const source = Array.from({length: 8}, (_, i) => i - 3);
    const weights = Array.from({length: 8}, (_, i) => 2*i - 5);
    const program = fold({width: 8, height: 1, tapWidth: 1, tapHeight: 1,
                          output: index({x: 1})}, g =>
      g.add(g.acc, g.mul(g.load(0, index({x: 1})),
                         g.load(2, index({x: 1})))));
    examples.push({name: "laneWeights", program,
                   buffers: {0: source, 2: weights},
                   expected: source.map((v, i) => v * weights[i]), pie: true});
  }

  { // A 3x3 image convolution with a coefficient load for every tap.
    const source = Array.from({length: 18 * 14}, (_, i) => (i * 47) % 401 - 200);
    const weights = [1024, 2048, 1024, 2048, 4096, 2048, 1024, 2048, 1024];
    const program = fold({width: 16, height: 12, tapWidth: 3, tapHeight: 3,
                          shift: 14, output: index({x: 1, y: 16})}, g =>
      g.add(g.acc, g.mul(g.load(0, index({x: 1, y: 18, tapX: 1, tapY: 18})),
                         g.load(2, index({tapX: 1, tapY: 3})))));
    const expected = [];
    for (let y = 0; y < 12; y++) for (let x = 0; x < 16; x++) {
      let sum = 0;
      for (let dy = 0; dy < 3; dy++) for (let dx = 0; dx < 3; dx++)
        sum += source[(y + dy) * 18 + x + dx] * weights[dy * 3 + dx];
      expected.push(sat(sum, 14));
    }
    examples.push({name: "convolution3x3", program,
                   buffers: {0: source, 2: weights}, expected, pie: true});
  }

  { // The shipping MP3 converter's 32-tap FIR expression over eight outputs.
    // `window[31 + x - tap]` corresponds to fir_window[j-k] after rebasing.
    const window = Array.from({length: 39}, (_, i) => (i * 193) % 2001 - 1000);
    const weights = Array.from({length: 32}, (_, i) => (i % 7 - 3) * 413);
    const program = fold({width: 8, height: 1, tapWidth: 32, tapHeight: 1,
                          shift: 14, output: index({x: 1})}, g =>
      g.add(g.acc, g.mul(g.load(0, index({base: 31, x: 1, tapX: -1})),
                         g.load(2, index({tapX: 1})))));
    const expected = [];
    for (let x = 0; x < 8; x++) {
      let sum = 0;
      for (let tap = 0; tap < 32; tap++)
        sum += window[31 + x - tap] * weights[tap];
      expected.push(sat(sum, 14));
    }
    examples.push({name: "mp3Fir32", program,
                   buffers: {0: window, 2: weights}, expected, pie: true});
  }

  { // Both coordinates of the shipping Q14 point transform, one output plane
    // at a time. Its signed int32 translation becomes the fold's initial QACC.
    const points = Array.from({length: 80}, (_, i) =>
      i % 2 ? (i * 17) % 211 - 105 : (i * 29) % 241 - 120);
    const axes = [
      {name: "affineX", weights: [15872, 2048], translation: 225000},
      {name: "affineY", weights: [-1024, 15360], translation: -327680}
    ];
    for (const axis of axes) {
      const program = fold({width: 40, height: 1, tapWidth: 2, tapHeight: 1,
                            shift: 14, initial: axis.translation,
                            output: index({x: 1})}, g =>
        g.add(g.acc, g.mul(g.load(0, index({x: 2, tapX: 1})),
                           g.load(2, index({tapX: 1})))));
      const expected = [];
      for (let x = 0; x < 40; x++)
        expected.push(sat(axis.translation + points[2 * x] * axis.weights[0] +
                          points[2 * x + 1] * axis.weights[1], 14));
      examples.push({name: axis.name, program,
                     buffers: {0: points, 2: axis.weights}, expected, pie: true});
    }
  }

  { // A row-wise matrix-vector dot product.
    const matrix = Array.from({length: 8 * 4}, (_, i) => (i * 7) % 23 - 11);
    const vector = [5, -2, 7, 3];
    const program = fold({width: 8, height: 1, tapWidth: 4, tapHeight: 1,
                          output: index({x: 1})}, g =>
      g.add(g.acc, g.mul(g.load(0, index({x: 4, tapX: 1})),
                         g.load(2, index({tapX: 1})))));
    const expected = [];
    for (let row = 0; row < 8; row++) {
      let sum = 0;
      for (let col = 0; col < 4; col++) sum += matrix[row * 4 + col] * vector[col];
      expected.push(sat(sum, 0));
    }
    examples.push({name: "matrixVector", program,
                   buffers: {0: matrix, 2: vector}, expected, pie: true});
  }

  { // A genuine sequential fold: the accumulator is multiplied each tap.
    // It must retain scalar semantics rather than be mistaken for a MAC.
    const source = Array.from({length: 8 * 4}, (_, i) => i % 7 - 3);
    const program = fold({width: 8, height: 1, tapWidth: 4, tapHeight: 1,
                          output: index({x: 1})}, g =>
      g.add(g.mul(g.acc, g.constant(2)),
            g.load(0, index({x: 4, tapX: 1}))));
    const expected = [];
    for (let x = 0; x < 8; x++) {
      let acc = 0;
      for (let tap = 0; tap < 4; tap++) acc = 2 * acc + source[x * 4 + tap];
      expected.push(sat(acc, 0));
    }
    examples.push({name: "sequentialFold", program,
                   buffers: {0: source}, expected, pie: false});
  }

  { // Both branches must read the same incoming acc, not a mutation of it.
    const program = fold({width: 8, height: 1, tapWidth: 2, tapHeight: 1,
                          output: index({x: 1})}, g => {
      const a = g.add(g.acc, g.constant(1));
      const b = g.add(g.acc, g.constant(2));
      return g.add(a, b);
    });
    examples.push({name: "branchedAcc", program, buffers: {},
                   expected: Array(8).fill(9), pie: false});
  }

  { // One immutable value may feed both operands of the final operation.
    const program = fold({width: 8, height: 1, tapWidth: 1, tapHeight: 1,
                          output: index({x: 1})}, g => {
      const a = g.add(g.acc, g.constant(2));
      return g.add(a, a);
    });
    examples.push({name: "sharedExpression", program, buffers: {},
                   expected: Array(8).fill(4), pie: false});
  }

  { // Sixteen IR instructions fit by reusing registers after their last use.
    const program = fold({width: 8, height: 1, tapWidth: 1, tapHeight: 1,
                          output: index({x: 1})}, g => {
      let value = g.acc;
      for (let n = 0; n < 8; ++n) value = g.add(value, g.constant(1));
      return value;
    });
    examples.push({name: "longExpression", program, buffers: {},
                   expected: Array(8).fill(8), pie: false});
  }

  { // A value from another builder (or an object shaped like one) is invalid.
    let foreign;
    fold({width: 1, height: 1, tapWidth: 1, tapHeight: 1,
          output: index({x: 1})}, g => {
      foreign = g.constant(1);
      return g.add(g.acc, foreign);
    });
    for (const value of [foreign, {reg: 0}]) {
      let rejected = false;
      try {
        fold({width: 1, height: 1, tapWidth: 1, tapHeight: 1,
              output: index({x: 1})}, g => g.add(g.acc, value));
      } catch (error) { rejected = error instanceof TypeError; }
      if (!rejected) throw Error("foreign grid value accepted");
    }
  }

  { // Eight independent prefix rows: each lane retains left-to-right order.
    const width = 16, height = 8, pitch = width + 1;
    const source = Array.from({length: width * height}, (_, i) => i % 9 - 4);
    const initialDest = Array(pitch * height).fill(0);
    for (let y = 0; y < height; y++) initialDest[y * pitch] = 10 + y;
    const program = fold({width, height, tapWidth: 1, tapHeight: 1,
                          output: index({base: 1, x: 1, y: pitch})}, g =>
      g.add(g.acc, g.add(g.load(1, index({x: 1, y: pitch})),
                         g.load(0, index({x: 1, y: width})))));
    const expected = [];
    const rows = initialDest.slice();
    for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
      const at = y * pitch + x + 1;
      rows[at] = sat(rows[at - 1] + source[y * width + x], 0);
      expected.push(rows[at]);
    }
    examples.push({name: "prefixRows", program,
                   buffers: {0: source}, initialDest, expected,
                   pie: false, scanPie: true});
  }

  { // A fixed-point IIR: eight rows run in parallel, each row in order.
    const width = 12, height = 8, pitch = width + 1;
    const source = Array.from({length: width * height}, (_, i) => i % 5 - 2);
    const initialDest = Array(pitch * height).fill(0);
    for (let y = 0; y < height; y++)
      initialDest[y * pitch] = y & 1 ? -120 - y : 300 + y;
    const program = fold({width, height, tapWidth: 1, tapHeight: 1,
                          output: index({base: 1, x: 1, y: pitch})}, g =>
      g.add(g.acc, g.add(g.mul(g.load(1, index({x: 1, y: pitch})),
                                 g.constant(2)),
                         g.load(0, index({x: 1, y: width})))));
    const expected = [], rows = initialDest.slice();
    for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
      const at = y * pitch + x + 1;
      rows[at] = sat(2 * rows[at - 1] + source[y * width + x], 0);
      expected.push(rows[at]);
    }
    examples.push({name: "iirSaturating", program, buffers: {0: source},
                   initialDest, expected, pie: false, scanPie: true});
  }

  { // The same dependency is recognized when source and multiply are reordered.
    const width = 12, height = 8, pitch = width + 1;
    const source = Array.from({length: width * height}, (_, i) => i % 7 - 3);
    const initialDest = Array(pitch * height).fill(0);
    for (let y = 0; y < height; y++) initialDest[y * pitch] = 50 - 4 * y;
    const program = fold({width, height, tapWidth: 1, tapHeight: 1,
                          output: index({base: 1, x: 1, y: pitch}), shift: 1}, g =>
      g.add(g.acc,
            g.add(g.load(0, index({x: 1, y: width})),
                  g.mul(g.load(1, index({x: 1, y: pitch})),
                        g.constant(-2)))));
    const expected = [], rows = initialDest.slice();
    for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
      const at = y * pitch + x + 1;
      rows[at] = sat(-2 * rows[at - 1] + source[y * width + x], 1);
      expected.push(rows[at]);
    }
    examples.push({name: "iirReordered", program, buffers: {0: source},
                   initialDest, expected, pie: false, scanPie: true});
  }

  { // 2D path counts: left and upper outputs have already been stored.
    const width = 5, height = 5, pitch = width + 1;
    const initialDest = Array(pitch * (height + 1)).fill(0);
    for (let i = 0; i <= width; i++) initialDest[i] = 1;
    for (let i = 0; i <= height; i++) initialDest[i * pitch] = 1;
    const program = fold({width, height, tapWidth: 1, tapHeight: 1,
                          output: index({base: pitch + 1, x: 1, y: pitch})}, g =>
      g.add(g.acc, g.add(g.load(1, index({base: pitch, x: 1, y: pitch})),
                         g.load(1, index({base: 1, x: 1, y: pitch})))));
    const expected = [], cells = initialDest.slice();
    for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
      const at = (y + 1) * pitch + x + 1;
      cells[at] = sat(cells[at - 1] + cells[at - pitch], 0);
      expected.push(cells[at]);
    }
    examples.push({name: "pathCount2D", program, buffers: {},
                   initialDest, expected, pie: false});
  }

  { // Min-plus convolution is a reduction over taps, not a cell dependency.
    const a = [7, -3, 4], b = Array.from({length: 10}, (_, i) => i * 3 - 11);
    const program = fold({width: 8, height: 1, tapWidth: 3, tapHeight: 1,
                          initial: 32767, output: index({x: 1})}, g =>
      g.min(g.acc, g.add(g.load(0, index({tapX: 1})),
                         g.load(2, index({base: 2, x: 1, tapX: -1})))));
    const expected = [];
    for (let x = 0; x < 8; x++)
      expected.push(Math.min(...a.map((v, k) => v + b[x + 2 - k])));
    examples.push({name: "minPlus", program,
                   buffers: {0: a, 2: b}, expected, pie: false});
  }

  { // A shortest-path grid: add the cost after choosing left or up.
    const width = 5, height = 5, pitch = width + 1;
    const initialDest = Array(pitch * (height + 1)).fill(1000);
    initialDest[0] = 0;
    const cost = Array.from({length: width * height}, (_, i) => i % 7 + 1);
    const program = fold({width, height, tapWidth: 1, tapHeight: 1,
                          output: index({base: pitch + 1, x: 1, y: pitch})}, g =>
      g.add(g.acc, g.add(g.min(g.load(1, index({base: pitch, x: 1, y: pitch})),
                               g.load(1, index({base: 1, x: 1, y: pitch}))),
                         g.load(0, index({x: 1, y: width})))));
    const expected = [], cells = initialDest.slice();
    for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
      const at = (y + 1) * pitch + x + 1;
      cells[at] = sat(Math.min(cells[at - 1], cells[at - pitch]) +
                      cost[y * width + x], 0);
      expected.push(cells[at]);
    }
    examples.push({name: "shortestPath2D", program,
                   buffers: {0: cost}, initialDest, expected, pie: false});
  }

  global.gridFoldExamples = examples;
})(globalThis);
