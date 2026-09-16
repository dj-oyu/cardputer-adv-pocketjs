// vmrun-flags: --module
const path = './fixtures/yield_dynamic_body.mjs';
print('import-start');
const a = import(path).then(m => print('first', m.value));
const b = import(path).then(m => print('second', m.value));
await Promise.all([a, b]);
print('import-done');
