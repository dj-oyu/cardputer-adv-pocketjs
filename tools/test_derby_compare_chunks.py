"""Reject source drift and keep the product JS byte-identical."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from derby_compare_chunks import instrument
from derby_telemetry_chunks import generate

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT/'apps/derby'
NODE = r'''
import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const source = fs.readFileSync(process.argv[1], 'utf8');
const start = source.indexOf('// Seeds: the hardware');
const end = source.indexOf('// The screen (README', start);
assert(start >= 0 && end > start);
let writes = 0, resolveGet;
const context = vm.createContext({
  pocket: {random: {seed: () => 0x2545F491}, storage: {
    get: () => process.argv[2] === 'delayed'
      ? new Promise(resolve => { resolveGet = resolve; })
      : Promise.resolve({value: {v: 1, pts: 1500, race: 20}}),
    set: () => { ++writes; return Promise.resolve({}); }
  }},
  pts: 1000, raceNo: 1, scene: 'pad', dm: 0, mx: Math.max,
  nop: () => {}, enter: () => {}, log: () => {}
});
vm.runInContext(source.slice(start, end), context);
if (process.argv[2] === 'delayed') {
  context.dm = 1;
  context.raceNo = 1000001;
  resolveGet({value: {v: 1, pts: 1500, race: 20}});
}
await Promise.resolve();
const observed = vm.runInContext('({sn, raceNo, HW})', context);
assert.equal(observed.HW, 0x2545F491);
assert.equal(observed.raceNo, process.argv[2] === 'delayed' ? 1000001 : 20);
assert.equal(observed.sn, process.argv[2] === 'shipping' ? 20 : 19);
vm.runInContext('save()', context);
assert.equal(writes, process.argv[2] === 'shipping' ? 1 : 0);
'''


class CompareChunks(unittest.TestCase):
    def test_generated_only_and_storage_write_suppressed(self):
        original = (SOURCE/'derby_view.js').read_bytes()
        generated = instrument(original.decode()).encode()
        self.assertEqual(original, (SOURCE/'derby_view.js').read_bytes())
        self.assertIn(b'let sn = 19;', generated)
        self.assertNotIn(b"ST.set('derby.v1'", generated)
        self.assertIn(b'raceNo = v.race | 0;', generated)
        self.assertNotIn(b'raceNo = sn = v.race | 0;', generated)
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            generate(SOURCE, out, True)
            self.assertEqual((out/'derby_view.js').read_bytes(), generated)
            self.assertIn(str(out/'derby_view.js'), (out/'chunks.txt').read_text())
            subprocess.run(['node', '--input-type=module', '-e', NODE,
                            str(out/'derby_view.js'), 'fixed'], check=True)
            subprocess.run(['node', '--input-type=module', '-e', NODE,
                            str(out/'derby_view.js'), 'delayed'], check=True)
            subprocess.run(['node', '--input-type=module', '-e', NODE,
                            str(SOURCE/'derby_view.js'), 'shipping'], check=True)
            generate(SOURCE, out, False)
            self.assertFalse((out/'derby_view.js').exists())
            self.assertIn(str(SOURCE/'derby_view.js'), (out/'chunks.txt').read_text())
        self.assertEqual(original, (SOURCE/'derby_view.js').read_bytes())

    def test_source_drift_fails(self):
        original = (SOURCE/'derby_view.js').read_text()
        with self.assertRaises(ValueError):
            instrument(original.replace('let sn = 0;', 'let sn = 1;'))
        with self.assertRaises(ValueError):
            instrument(original.replace('raceNo = sn = v.race | 0;', 'raceNo = v.race | 0;'))
        with self.assertRaises(ValueError):
            instrument(original.replace("v.race > 0 && scene === 'pad')", "v.race > 0)"))


if __name__ == '__main__':
    unittest.main()
