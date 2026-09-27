// Procedural megademo in a Kasane news set. Numeric opcodes match ksn_proc_op.
// The program is registered once; one procedural resource is placed as an image
// node and its rectangle moves between the studio monitor and the full screen.
(function () {
  'use strict';
  const SET = 0, INPUT = 1, ADD = 2, MUL = 3, SIN = 4;
  const REPEAT = 5, END = 6, MOVE = 7, LINE = 9;
  const colors = [
    [0x07ff, 0x3dff, 0xb81f, 0xffff],
    [0xfde0, 0xfb00, 0x07ff, 0xffff],
    [0xf81f, 0x781f, 0x07ff, 0xffff]
  ];
  const backdrops = [0x080c, 0x000d, 0x100b];
  const f32 = Math.fround;

  function checkPhase(phase) {
    if (!Number.isInteger(phase) || phase < 0 || phase > 2)
      throw RangeError('phase must be 0..2');
  }
  function checkLayer(layer) {
    if (!Number.isInteger(layer) || layer < 0 || layer > 4)
      throw RangeError('layer must be 0..4');
  }
  function checkFrame(frame) {
    if (!Number.isInteger(frame) || frame < 0 || frame > 47)
      throw RangeError('frame must be 0..47');
  }

  function program(phase, layer) {
    checkPhase(phase);
    checkLayer(layer);
    const neon = colors[phase];
    const code = [];
    function emit(op, dst, a, b, value, color) {
      if (code.length >= 64) throw RangeError('program exceeds 64 instructions');
      code.push([op, dst, a, b, f32(value), color]);
    }
    const set = (r, v) => emit(SET, r, 0, 0, v, 0);
    const input = (r, i) => emit(INPUT, r, i, 0, 0, 0);
    const move = (x, y) => emit(MOVE, 0, x, y, 0, 0);
    const line = (x, y, c) => emit(LINE, 0, x, y, 0, c);
    const add = (d, a, b) => emit(ADD, d, a, b, 0, 0);
    const mul = (d, a, b) => emit(MUL, d, a, b, 0, 0);
    const repeat = n => emit(REPEAT, 0, n, 0, 0, 0);
    const end = () => emit(END, 0, 0, 0, 0, 0);

    if (layer === 0) {
      input(0, 0); input(1, 1);
      set(2, phase === 1 ? 42 : 5); set(3, phase === 1 ? 125 : 129);
      set(4, phase === 1 ? 13 : 12); set(5, phase === 1 ? -13 : -12);
      set(6, phase === 1 ? 3 : 7); set(7, -7);
      repeat(phase === 1 ? 6 : 8);
      move(0, 2); line(1, 2, neon[0]); line(1, 3, neon[0]);
      line(0, 3, neon[0]); line(0, 2, neon[0]);
      add(0, 0, 4); add(1, 1, 5); add(2, 2, 6); add(3, 3, 7);
      end();
    } else if (layer === 1) {
      input(0, 0); input(1, 1);
      set(2, phase === 1 ? 64 : 68); set(3, 134);
      set(4, 23); set(5, 4.8);
      repeat(12);
      move(1, 2); line(0, 3, neon[1]);
      add(0, 0, 4); add(1, 1, 5);
      end();
    } else if (layer === 2) {
      set(0, 0); set(1, 239); input(2, 0);
      set(3, 1.5); set(4, 1.47);
      repeat(10);
      move(0, 2); line(1, 2, neon[2]);
      add(2, 2, 3); mul(3, 3, 4);
      end();
    } else if (layer === 3) {
      set(0, 0); set(1, 4); set(2, f32(f32(0.073) + f32(phase * 0.015)));
      input(3, 0); set(4, phase === 1 ? 7 : 13);
      set(5, phase === 2 ? 42 : 28);
      repeat(60);
      mul(6, 0, 2); add(6, 6, 3); emit(SIN, 6, 6, 0, 0, 0);
      mul(7, 6, 4); add(7, 7, 5);
      line(0, 7, neon[3]); add(0, 0, 1);
      end();
    } else {
      input(0, 0); set(1, 29); set(2, 0); set(3, 239);
      input(4, 1); set(6, 2);
      repeat(4);
      move(2, 0); line(3, 0, neon[2]);
      add(5, 0, 6); move(2, 5); line(3, 5, neon[0]);
      add(0, 0, 1); add(2, 2, 4); add(3, 3, 4);
      end();
    }
    return code;
  }

  function inputs(frame, layer) {
    checkFrame(frame);
    checkLayer(layer);
    const phase = Math.floor(frame / 16);
    const beat = frame % 16;
    const pulse = f32(Math.sin(f32(f32(beat) * f32(0.3926990817))));
    const result = [0, 0, 0, 0];
    if (layer === 0) {
      const inset = phase === 1 ? 19 : (phase === 2 ? 3 : 8);
      const sway = f32(pulse * (phase === 2 ? 9 : 3));
      result[0] = f32(inset + sway);
      result[1] = f32(f32(239 - inset) + sway);
    } else if (layer === 1) {
      result[0] = f32(-18 + f32(pulse * 4));
      result[1] = f32(91 + f32(pulse * 3));
    } else if (layer === 2) {
      result[0] = phase === 1 ? f32(70 + pulse) : f32(65 + f32(pulse * 2));
    } else if (layer === 3) {
      result[0] = f32(f32(beat) * f32(0.31));
    } else {
      result[0] = (phase === 2 ? 9 : 17) + ((frame * 7) % 19);
      result[1] = (frame % 5 - 2) * (phase === 2 ? 2 : 1);
    }
    return result;
  }

  // A fixed connected trace, transformed by the native Q14 batch path once
  // per draw. Source points and coefficients are built only at registration.
  function pointBatch(phase, layer) {
    checkPhase(phase);
    checkLayer(layer);
    if (layer !== 3) return null;
    const x = [], y = [];
    for (let i = 0; i < 40; ++i) {
      x.push(18 + i * 5);
      y.push(92 + ((i * 13) % 23) - 11);
    }
    const coeff = [
      [16384, 0, 0, 16384, 0, 0],
      [16000, 1600, -1000, 16384, -6 * 16384, 6 * 16384],
      [15360, -2304, 1300, 15500, 18 * 16384, -10 * 16384]
    ];
    return {kind: 'affineQ14Points', x: x, y: y,
      coeff: coeff[phase].slice(), color: colors[phase][0]};
  }

  globalThis.procMegademo = {
    program: program,
    inputs: inputs,
    pointBatch: pointBatch,
    backdrop: function (frame) {
      checkFrame(frame);
      return backdrops[Math.floor(frame / 16)];
    }
  };

  if (typeof pocket === 'undefined' || !pocket.kasane ||
      !pocket.kasane.procedural ||
      typeof pocket.kasane.procedural.resource !== 'function')
    throw Error('Kasane procedural image resource is required');

  const view = pocket.kasane;
  const host = view.procedural;
  const resource = host.resource();
  const handles = [];
  for (let phase = 0; phase < 3; ++phase) {
    const scene = [];
    for (let layer = 0; layer < 5; ++layer) {
      const batch = pointBatch(phase, layer);
      scene.push(batch ? host.register(program(phase, layer), batch)
                       : host.register(program(phase, layer)));
    }
    handles.push(scene);
  }

  const monitor = [64, 20, 176, 83]; // 16:9, as is the 240x135 source.
  const full = [0, 0, 240, 135];
  const HOLD_MONITOR = 20, ZOOM_IN = 44, HOLD_FULL = 52, ZOOM_OUT = 44;
  const CYCLE = HOLD_MONITOR + ZOOM_IN + HOLD_FULL + ZOOM_OUT;
  let image;
  function build(tx) {
    tx.background(0x07101cff);
    tx.gradient({bounds: [0, 0, 240, 90], axis: 'x',
      from: 0x0d263bff, to: 0x18384aff});
    tx.rect({bounds: [0, 0, 240, 15], color: 0x061521ff});
    tx.text({bounds: [8, 2, 180, 14], text: 'POCKET NEWS  /  STUDIO 01',
      font: 'caption', color: 0xd7f4ffff});
    tx.rect({bounds: [11, 20, 17, 92], color: 0x40a8b8ff});
    tx.rect({bounds: [20, 28, 25, 86], color: 0x21536cff});
    tx.rect({bounds: [181, 21, 231, 88], color: 0x0b1c2bff});
    tx.text({bounds: [186, 30, 228, 44], text: 'ON AIR',
      font: 'caption', color: 0xff795cff});
    tx.rect({bounds: [186, 49, 224, 51], color: 0x3d99aaff});
    tx.rect({bounds: [186, 58, 217, 60], color: 0x285f77ff});
    tx.rect({bounds: [186, 67, 226, 69], color: 0x285f77ff});
    // The bezel sits below the image in paint order. As the image grows, it
    // naturally covers the studio monitor without a second visibility state.
    tx.rect({bounds: [60, 16, 180, 87], color: 0x02070bff});
    tx.rect({bounds: [62, 18, 178, 85], color: 0x8aa7b1ff});
    image = tx.image({resource: resource, bounds: monitor, clip: full,
      sourceWidth: 240, sourceHeight: 135});
    // Broadcast graphics stay in front in both the inset and full-screen view.
    tx.rect({bounds: [0, 106, 240, 135], color: 0x06111ee8});
    tx.rect({bounds: [0, 106, 240, 109], color: 0x45cddaff});
    tx.rect({bounds: [8, 113, 44, 128], color: 0xd93436ff});
    tx.text({bounds: [12, 115, 41, 127], text: 'LIVE',
      font: 'caption', color: 0xffffffff});
    tx.text({bounds: [50, 112, 233, 124],
      text: 'MEGADEMO  /  THE CANVAS REPORT',
      font: 'caption', color: 0xffffffff});
    tx.text({bounds: [50, 124, 232, 134],
      text: 'PROCEDURAL PICTURE IN A UI SCENE',
      font: 'caption', color: 0x80d9e8ff});
  }

  function smooth(t) { return t * t * (3 - 2 * t); }
  function zoomAt(tick) {
    const pos = tick % CYCLE;
    if (pos < HOLD_MONITOR) return 0;
    if (pos < HOLD_MONITOR + ZOOM_IN)
      return smooth((pos - HOLD_MONITOR) / ZOOM_IN);
    if (pos < HOLD_MONITOR + ZOOM_IN + HOLD_FULL) return 1;
    return 1 - smooth((pos - HOLD_MONITOR - ZOOM_IN - HOLD_FULL) /
                      ZOOM_OUT);
  }
  function boundsAt(zoom) {
    return [
      Math.round(monitor[0] + (full[0] - monitor[0]) * zoom),
      Math.round(monitor[1] + (full[1] - monitor[1]) * zoom),
      Math.round(monitor[2] + (full[2] - monitor[2]) * zoom),
      Math.round(monitor[3] + (full[3] - monitor[3]) * zoom)
    ];
  }

  let tick = 0;
  globalThis.frame = function () {
    const frameNo = tick % 48;
    const phase = Math.floor(frameNo / 16);
    host.beginFrame(backdrops[phase]);
    for (let layer = 0; layer < 5; ++layer)
      host.draw(handles[phase][layer], inputs(frameNo, layer));
    host.commit();
    if (tick === 0) view.replace(build);
    else {
      const bounds = boundsAt(zoomAt(tick));
      view.patch(function (tx) { image.setRect(tx, bounds); });
    }
    ++tick;
  };
})();
