// pocket.io probe; "|" lists codes that are all right answers. README.md.
globalThis.frame = () => {};
const p = pocket, U = Uint8Array, log = (s) => console.log(s);
const shot = async (name, run, want) => {
  let got;
  try { const v = await run(); got = v && v.length !== undefined ? 'ok ' + v.length + 'B' : 'ok'; }
  catch (e) { got = e.code || String(e); }
  log((want.split('|').indexOf(got) < 0 ? 'HUH ' : 'OK  ') + name + ' -> ' + got);
};
const i2c = (a, hz, port) => () => p.io.i2c.open({ port: port || 'ext.i2c', address: a, hz: hz || 400000 });
const DENY = 'PERMISSION_DENIED', BAD = 'INVALID_ARGUMENT';

(async () => {
  for (const n of ['io.i2c', 'io.spi', 'io.uart', 'io.gpio', 'io.ir']) {
    const c = p.capabilities.get(n);
    log(n + ' supported=' + c.supported + ' available=' + c.available);
  }
  const L = p.capabilities.get('io.i2c').limits;
  log('i2c refuses ' + L.reservedAddresses + ', waits <=' + L.maxWaitMs + 'ms, scan=' + L.scan);
  log('ir ' + JSON.stringify(p.capabilities.get('io.ir').limits));
  for (const q of p.io.ports()) log('port ' + q.id + ' ' + q.protocols[0] + ' ' + q.limits.wiring);

  await shot('0x34 keyboard', i2c(0x34), DENY);
  await shot('0x69 imu', i2c(0x69), DENY);
  await shot('0x18 codec', i2c(0x18), DENY);
  await shot('0x00 i2c-reserved', i2c(0x00), DENY);
  await shot('hz 250000', i2c(0x50, 250000), BAD);
  await shot('port "lcd"', i2c(0x50, 400000, 'lcd'), 'NOT_FOUND');

  const dev = await i2c(0x50)();
  await shot('empty 0x50', () => dev.transfer({ write: new U([0]), readBytes: 1 }), 'IO_ERROR|TIMEOUT');
  await shot('300B write', () => dev.transfer({ write: new U(300) }), 'LIMIT_EXCEEDED');
  await shot('timeoutMs 5000', () => dev.transfer({ readBytes: 1 }, { timeoutMs: 5000 }), BAD);
  dev.close();
  await shot('after close', () => dev.transfer({ readBytes: 1 }), 'CLOSED');

  const g = await i2c(0x50, 100000, 'grove')();
  await shot('grove.g1 vs grove', () => p.io.gpio.open({ port: 'grove.g1', mode: 'input' }), 'BUSY');
  g.close();

  const pin = await p.io.gpio.open({ port: 'ext.int', mode: 'input' });
  log('ext.int reads ' + pin.read());
  pin.close();

  const u = await p.io.uart.open({ port: 'ext.uart', baud: 115200 });
  log('uart queued ' + (await u.write(new U([80, 74, 83, 10]))) + 'B');
  await shot('read, none sending', () => u.read(8, { timeoutMs: 200 }), 'TIMEOUT');
  u.close();

  const s = await p.io.spi.open({ port: 'ext.spi', mode: 0, hz: 4000000 });
  await shot('spi 4B', () => s.transfer(new U([0, 0, 0, 0])), 'ok 4B');
  await shot('spi 1024B in 1ms', () => s.transfer(new U(1024), { timeoutMs: 1 }), 'TIMEOUT');
  s.close();

  await shot('ir 5kHz carrier', () => p.io.ir.send({ carrierHz: 5000, durationsUs: [500, 500] }), BAD);
  await shot('ir NEC lead-in', () => p.io.ir.send({ carrierHz: 38000, durationsUs: [9000, 4500, 560, 560] }), 'ok');
  log('probe done');
})().catch((e) => log('threw ' + (e && e.code ? e.code : e)));
