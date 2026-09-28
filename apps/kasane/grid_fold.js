// Registration-time symbolic fold for the typed int16 grid IR experiment.
// Values are immutable expressions. Only the returned expression updates acc;
// native code runs the rectangular loops after registration.
(function (global) {
  const CONST = 1, LOAD = 2, ADD = 3, MUL = 4, MIN = 5, NO_PARAM = 255;
  const int = (n, low, high) => {
    if (!Number.isSafeInteger(n) || n < low || n > high) throw RangeError("grid integer");
    return n;
  };
  const fields = (value, allowed, name) => {
    if (!value || typeof value !== "object" || Array.isArray(value) ||
        Object.keys(value).some(key => !allowed.includes(key)))
      throw TypeError(name);
    return value;
  };
  const normalizedCoefficient = (v) => {
    if (!Array.isArray(v) || v.length !== 3) throw TypeError("grid coefficient");
    const param = v[2] === NO_PARAM ? NO_PARAM : int(v[2], 0, 7);
    return [int(v[0], -2147483648, 2147483647),
            int(v[1], -32768, 32767), param];
  };
  const coefficient = (v) => {
    if (typeof v === "number")
      return [int(v, -2147483648, 2147483647), 0, NO_PARAM];
    fields(v, ["base", "scale", "param"], "grid coefficient");
    const hasParam = v.param !== undefined;
    if (!hasParam && v.scale !== undefined)
      throw TypeError("grid coefficient scale needs param");
    return [int(v.base === undefined ? 0 : v.base, -2147483648, 2147483647),
            hasParam ? int(v.scale === undefined ? 1 : v.scale, -32768, 32767) : 0,
            hasParam ? int(v.param, 0, 7) : NO_PARAM];
  };
  const index = (...args) => {
    if (args.length !== 1) throw TypeError("grid index needs one axes object");
    const axes = fields(args[0], ["base", "x", "y", "tapX", "tapY"],
                        "grid index axes");
    return [axes.base, axes.x, axes.y, axes.tapX, axes.tapY]
      .map(term => coefficient(term === undefined ? 0 : term));
  };
  const checkedIndex = (value) => {
    if (!Array.isArray(value) || value.length !== 5) throw TypeError("grid index");
    return value.map(normalizedCoefficient);
  };
  const views = new WeakMap();
  const view = (...args) => {
    if (args.length !== 1) throw TypeError("grid view needs one object");
    const {buffer, ...axes} = fields(args[0],
      ["buffer", "base", "x", "y", "tapX", "tapY"], "grid view");
    const value = Object.freeze({});
    views.set(value, {buffer: int(buffer, 0, 7),
                      index: index(axes)});
    return value;
  };
  const fold = (spec, build) => {
    if (typeof build !== "function") throw TypeError("grid fold body");
    const shape = [spec.width, spec.height, spec.tapWidth, spec.tapHeight]
      .map((n) => int(n, 1, 65535));
    const nodes = new WeakMap();
    const make = (node) => {
      const value = Object.freeze({});
      nodes.set(value, node);
      return value;
    };
    const acc = make({op: 0});
    const ref = (value) => {
      const node = value && nodes.get(value);
      if (!node) throw TypeError("grid expression from another fold");
      return node;
    };
    const api = Object.freeze({
      acc,
      index,
      load: (buffer, at) => {
        const source = at === undefined ? views.get(buffer) : null;
        if (at === undefined && !source) throw TypeError("grid view");
        return make({op: LOAD, buffer: source ? source.buffer : int(buffer, 0, 7),
                     index: checkedIndex(source ? source.index : at)});
      },
      constant: (value) => make({op: CONST,
                                  immediate: int(value, -32768, 32767)}),
      add: (a, b) => make({op: ADD, a: ref(a), b: ref(b)}),
      mul: (a, b) => make({op: MUL, a: ref(a), b: ref(b)}),
      min: (a, b) => make({op: MIN, a: ref(a), b: ref(b)})
    });
    const result = ref(build(api));
    if (!result.op) throw TypeError("grid fold must update accumulator");

    // Walk only the returned DAG. A symbolic expression has no effects until
    // compiled; a construction that is not returned must not enter the IR.
    const ordered = [], seen = new Set();
    const visit = (node) => {
      if (!node.op || seen.has(node)) return;
      seen.add(node);
      if (node.a) visit(node.a);
      if (node.b) visit(node.b);
      ordered.push(node);
    };
    visit(result);
    const uses = new Map();
    for (const node of ordered) {
      if (node.a && node.a.op) uses.set(node.a, (uses.get(node.a) || 0) + 1);
      if (node.b && node.b.op) uses.set(node.b, (uses.get(node.b) || 0) + 1);
    }
    const regs = new Map(), free = [];
    let next = 1;
    const body = [];
    const register = (node) => node.op ? regs.get(node) : 0;
    for (const node of ordered) {
      if (body.length >= 16) throw RangeError("grid body limit");
      const a = node.a ? register(node.a) : 0;
      const b = node.b ? register(node.b) : 0;
      let dst;
      if (node === result) dst = 0;
      else if (next < 8) dst = next++;
      else if (free.length) dst = free.pop();
      else {
        // A dying operand can be overwritten after this instruction reads it.
        const dying = [node.a, node.b].find((operand) => operand && operand.op &&
          uses.get(operand) === (node.a === node.b ? 2 : 1));
        if (!dying) throw RangeError("grid register pressure");
        dst = regs.get(dying);
      }
      body.push({op: node.op, dst, a, b, buffer: node.buffer || 0,
                 immediate: node.immediate || 0,
                 index: node.index || index({})});
      for (const operand of [node.a, node.b]) {
        if (!operand || !operand.op) continue;
        const remaining = uses.get(operand) - 1;
        uses.set(operand, remaining);
        if (!remaining && regs.get(operand) !== dst) free.push(regs.get(operand));
      }
      if (dst) regs.set(node, dst);
    }
    return {count: body.length, result_reg: 0,
            final_shift: int(spec.shift === undefined ? 0 : spec.shift, 0, 30),
            initial: int(spec.initial === undefined ? 0 : spec.initial,
                         -2147483648, 2147483647),
            body, output: checkedIndex(spec.output), shape};
  };
  global.gridFold = Object.freeze({fold, index, view});
})(globalThis);
