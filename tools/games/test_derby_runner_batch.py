"""Actual PAN regression with the opt-in layout adapter (no device)."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest

from derby_runner_batch import instrument

ROOT = Path(__file__).resolve().parents[2]


class BatchGeneration(unittest.TestCase):
    def test_actual_pan_contract(self):
        pan = (ROOT / 'apps/derby/derby_pan.js').read_text()
        source = (ROOT / 'tools/games/test_derby_pan_order.mjs').read_text()
        begin = source.index('function runPanTests(')
        end = source.index('\nconst read = name')
        harness = source[begin:end].replace(
            'const live={}, pocket={derby:{view(){},ser(){}}}',
            "const live=Object.fromEntries(Array.from({length:8},(_,i)=>['r'+i,i+1])), "
            "pocket={derby:{view(){},ser(){},runners(buffer,count){const a=new Float64Array(buffer);"
            "for(let i=0;i<count;i++){const j=i*5;rin(a[j]-1,a[j+1],a[j+2],a[j+3],a[j+4]);}}}}")
        code = "import assert from 'node:assert/strict';\n" + harness
        names = ['derby_watch.js', 'derby_view.js', 'derby_scene.js']
        args = [(ROOT / 'apps/derby' / name).read_text() for name in names]
        code += '\nconst result=runPanTests(' + ','.join(map(json.dumps, args + [instrument(pan)])) + ',assert);\n'
        code += 'console.log("PASS generated PAN layout",result);\n'
        subprocess.run(['node', '--input-type=module'], input=code, text=True, check=True, cwd=ROOT)
        self.assertEqual(pan, (ROOT / 'apps/derby/derby_pan.js').read_text())

    def test_fail_closed_source_drift(self):
        source = (ROOT / 'apps/derby/derby_pan.js').read_text()
        with self.assertRaises(ValueError):
            instrument(source.replace('rin(l, X, r * U', 'rin(l, X, 1 * U'))

    def test_compose_telemetry_without_source_writes(self):
        paths = list((ROOT / 'apps/derby').glob('*')) + list((ROOT / 'tools').rglob('*.py*'))
        before = {p: p.read_bytes() for p in paths if p.is_file()}
        with tempfile.TemporaryDirectory() as tmp:
            trace, batch = Path(tmp) / 'trace', Path(tmp) / 'batch'
            subprocess.run([sys.executable, '-B', str(ROOT / 'tools/derby_telemetry_chunks.py'),
                            str(ROOT / 'apps/derby'), str(trace)], check=True)
            subprocess.run([sys.executable, '-B', str(ROOT / 'tools/derby_runner_batch_chunks.py'),
                            str(trace), str(batch)], check=True)
            rows = dict(line.split(maxsplit=1) for line in (batch / 'chunks.txt').read_text().splitlines())
            self.assertEqual(Path(rows['play']), trace / 'derby_play.js')
            self.assertEqual(Path(rows['pan']).read_text(), instrument((ROOT / 'apps/derby/derby_pan.js').read_text()))
            for key in ['prog', 'view', 'scene', 'demo']:
                self.assertEqual(Path(rows[key]), ROOT / f'apps/derby/derby_{key}.js')
        after_paths = list((ROOT / 'apps/derby').glob('*')) + list((ROOT / 'tools').rglob('*.py*'))
        self.assertEqual(before, {p: p.read_bytes() for p in after_paths if p.is_file()})


if __name__ == '__main__':
    unittest.main()
