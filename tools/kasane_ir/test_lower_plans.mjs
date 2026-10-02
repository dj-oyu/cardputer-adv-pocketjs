// Source preservation and diagnostics through the firmware's lowering and
// ROM emitter CLI paths: node tools/kasane_ir/test_lower_plans.mjs
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import vm from 'node:vm';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {lowerText} from './lower_plans.mjs';
import {findPlans, compilePlan} from './plan_js.mjs';
import {romEntries, emitC} from './emit_rom_plans.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const lower = text => lowerText(text, 'source.js', null, 'demo').text;
const method = (name = 't') => `  /** @plan ${name} inputs: x */\n  ${name}() { move(x, 0); },\n`;
const container = (name, plan) => `const ${name} = {\n${method(plan)}};\n`;
const untouched = '// cache belongs to the app\nconst cache = {};\ncache.x = 1;\nlet state = {\n};\n';
assert.equal(lower(untouched), untouched);
assert.equal(lower(untouched + container('T', 't') + container('U', 'u') + 'const after = {};\n'),
  untouched + 'const after = {};\n');
assert.equal(lower(container('T', 't') + untouched + container('U', 'u') + 'const after = {};\n'),
  untouched + 'const after = {};\n');
assert.equal(lower('let T = {\n' + method() + '};\n'), '');
// A top-level plan removal must not make an unrelated object a candidate.
assert.equal(lower('/** @plan t inputs: x */\nfunction t() { move(x, 0); }\n' + untouched), untouched);
assert.equal(lower('/** @planDecoder rom */\nfunction prog() {\n}\n' + untouched), untouched);
// Ordinary properties, comments within a container, and comments outside
// it survive. Only a plan's own preceding // comments travel with it.
const mixed = 'const mixed = {\n  keep: 7,\n' + method() + '  other: {}\n};\n';
assert.equal(lower(mixed), 'const mixed = {\n  keep: 7,\n  other: {}\n};\n');
const comment = 'const T = {\n  /* Keep this object comment. */\n' + method() + '};\n';
assert.equal(lower(comment), 'const T = {\n  /* Keep this object comment. */\n};\n');
assert.equal(lower('// Keep the container comment.\n' + container('T', 't') + '// Keep the tail.\n'),
  '// Keep the container comment.\n// Keep the tail.\n');
const context = {};
vm.runInNewContext(lower(untouched + container('T', 't') + 'globalThis.result = cache.x;\n'), context);
assert.equal(context.result, 1);

const source = `// Application state is unrelated to the plans.
const cache = {};
cache.x = 1;
/** @planDecoder rom */
function prog() {
  throw Error('build only');
}
const T = {
  /** @plan approx inputs: x */
  approx() { move(x / 3, 0); },
  /** @plan exact inputs: x */
  exact() { move(x / 4, 0); }
};
globalThis.result = cache.x;
`;
const compiled = findPlans(source, 'source.js').map(compilePlan);
assert.equal(compiled[0].warnings.length, 1);
assert.match(compiled[0].warnings[0], /x\/3 compiled as x\*.*\(not exact\)/);
assert.deepEqual(compiled[1].warnings, []);
for (const [ids, rom, text] of [[null, 'demo', source], ['demo', null, source],
  [null, null, source.replace('@planDecoder rom', '@planDecoder')]]) {
  const r = lowerText(text, 'source.js', ids, rom);
  assert.equal(r.decoder, true);
  assert.deepEqual(r.report.map(p => p.warnings), compiled.map(p => p.warnings));
  assert.deepEqual(r.report.map(p => p.ir), compiled.map(p => p.text));
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'kasane-lowering-'));
try {
  const src = path.join(dir, 'source.js'), dst = path.join(dir, 'out.js');
  fs.writeFileSync(src, source);
  const run = (script, args, warning = true) => {
    const r = spawnSync(process.execPath, [path.join(here, script), ...args], {encoding: 'utf8'});
    assert.equal(r.status, 0, r.stderr || r.error?.message);
    if (warning) {
      assert.match(r.stderr, /source\.js: (?:demo\.)?approx: warning: x\/3 compiled as x\*.*\(not exact\)/);
      assert.equal(r.stderr.split('\n').filter(s => s.includes('warning:')).length, 1);
      assert.doesNotMatch(r.stderr, /exact: warning:/);
    } else assert.equal(r.stderr, '');
    return r;
  };
  // CMake invokes --file, not the directory/report form.
  run('lower_plans.mjs', ['--rom', 'demo', '--file', src, dst]);
  assert.equal(fs.readFileSync(dst, 'utf8'), lower(source));
  assert.equal(vm.runInNewContext(fs.readFileSync(dst, 'utf8') + '\nresult;'), 1);
  run('lower_plans.mjs', ['--ids', 'demo', '--file', src, dst]);
  assert.match(fs.readFileSync(dst, 'utf8'), /approx: 'demo\.approx'/);
  fs.writeFileSync(src, source.replace('@planDecoder rom', '@planDecoder'));
  run('lower_plans.mjs', ['--file', src, dst]);
  assert.match(fs.readFileSync(dst, 'utf8'), /function prog\(/);
  // No approximate operation means no warning, including a marker-free file.
  fs.writeFileSync(src, source.replace('x / 3', 'x / 4'));
  run('lower_plans.mjs', ['--rom', 'demo', '--file', src, dst], false);
  fs.writeFileSync(src, untouched.replaceAll('\n', '\r\n'));
  run('lower_plans.mjs', ['--rom', 'demo', '--file', src, dst], false);
  assert.equal(fs.readFileSync(dst, 'utf8'), fs.readFileSync(src, 'utf8'));

  // Directory lowering also retains warnings in its machine-readable report.
  const app = path.join(dir, 'demo'), out = path.join(dir, 'lowered');
  fs.mkdirSync(app);
  fs.writeFileSync(path.join(app, 'source.js'), source);
  run('lower_plans.mjs', [app, out]);
  assert.deepEqual(JSON.parse(fs.readFileSync(path.join(out, 'plans.json'), 'utf8')),
    lowerText(source, 'source.js', null, 'demo').report);

  // ROM generation has a separate compilation path, with the same warnings.
  fs.writeFileSync(src, source);
  const entries = romEntries('demo', src);
  assert.deepEqual(entries.map(e => e.warnings), compiled.map(p => p.warnings));
  const cfile = path.join(dir, 'rom.c'), json = path.join(dir, 'rom.json');
  run('emit_rom_plans.mjs', [cfile, `demo=${src}`, '--json', json]);
  assert.equal(fs.readFileSync(cfile, 'utf8'), emitC(entries, 'ksn_proc_rom_plans', ['source.js']));
  const report = JSON.parse(fs.readFileSync(json, 'utf8'));
  assert.deepEqual(report.map(p => p.warnings), compiled.map(p => p.warnings));
  assert.ok(report.every(p => p.file === src));
  fs.writeFileSync(src, source.replace('x / 3', 'x / 4'));
  run('emit_rom_plans.mjs', [cfile, `demo=${src}`, '--json', json], false);
  assert.ok(JSON.parse(fs.readFileSync(json, 'utf8')).every(p => p.warnings.length === 0));

  // Invalid source still stops both build paths with the existing R4 error.
  fs.writeFileSync(src, '/** @plan shadow inputs: i */\nfunction t() {\n' +
    'for (let i = 0; i < 2; i++) { move(i, 0); }\nmove(i, 0);\n}\n');
  for (const [script, args] of [['lower_plans.mjs', ['--rom', 'demo', '--file', src, dst]],
    ['emit_rom_plans.mjs', [cfile, `demo=${src}`]]]) {
    const r = spawnSync(process.execPath, [path.join(here, script), ...args], {encoding: 'utf8'});
    assert.notEqual(r.status, 0);
    assert.match(r.stderr, /source\.js: shadow: line 3: i shadows a name already in scope \[R4:/);
  }
} finally {
  fs.rmSync(dir, {recursive: true, force: true});
}
console.log('test_lower_plans: PASS');
