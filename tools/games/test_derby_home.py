"""HOME keeps every original mode and cannot reuse stale validation results."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import run_derby as runner
import run_derby_home as home

EXPECTED = (
    ('derby-asan', ()),
    ('derby-telemetry', ('--telemetry', '--heap-limit', '524288')),
    ('derby-measure', ('--measure', '--heap-limit', '524288')),
    ('derby-m32', ('--m32',)),
    ('derby-m32-telemetry', ('--m32', '--telemetry', '--heap-limit', '163840')),
)


class CompleteHome(unittest.TestCase):
    def test_same_five_profiles_and_one_node_validation(self):
        self.assertEqual(home.MODES, EXPECTED)
        self.assertEqual(runner.NODE_TESTS, ('test_derby_lanes.mjs','test_derby_photo.mjs','test_derby_pan_order.mjs'))
        seen = []
        with tempfile.TemporaryDirectory() as tmp, contextlib.ExitStack() as stack:
            out = Path(tmp)
            stack.enter_context(patch.object(runner, 'validate_node_sources', return_value='fresh'))
            verify = stack.enter_context(patch.object(runner, 'require_node_inputs'))
            def profile(args, proof):
                self.assertEqual(proof, 'fresh')
                seen.append(args)
            stack.enter_context(patch.object(runner, 'run_profile', profile))
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            home.complete(out)
            self.assertEqual(len(seen), 5)
            self.assertEqual(len({runner.profile_cache(args) for args in seen}), 5)
            self.assertEqual([(x.m32,x.telemetry,x.measure,x.heap_limit) for x in seen], [
                (False,False,False,None),(False,True,False,524288),
                (False,False,True,524288),(True,False,False,None),(True,True,False,163840)])
            proof = json.loads((out/'derby-complete.json').read_text())
            self.assertEqual([x['name'] for x in proof['modes']], [x[0] for x in EXPECTED])
            self.assertEqual(len(list(out.glob('*.log'))), 6)
            self.assertEqual(verify.call_args.args, ('fresh',))
            runner.validate_node_sources.assert_called_once_with()

    def test_node_error_cannot_execute_modes_or_leave_old_success(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.ExitStack() as stack:
            out = Path(tmp)
            for name in ['derby-complete.json', *(x[0]+'.log' for x in EXPECTED)]:
                (out/name).write_text('old success')
            stack.enter_context(patch.object(runner, 'validate_node_sources', side_effect=subprocess.CalledProcessError(1,'node')))
            profiles = stack.enter_context(patch.object(runner, 'run_profile'))
            with self.assertRaises(subprocess.CalledProcessError):
                home.complete(out)
            profiles.assert_not_called()
            self.assertFalse((out/'derby-complete.json').exists())
            self.assertFalse((out/'derby-m32-telemetry.log').exists())

    def test_last_device_sized_mode_failure_cannot_write_success(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.ExitStack() as stack:
            out = Path(tmp)
            stack.enter_context(patch.object(runner, 'validate_node_sources', return_value='fresh'))
            stack.enter_context(patch.object(runner, 'require_node_inputs'))
            count = []
            def profile(args, proof):
                count.append(args)
                if args.m32 and args.telemetry:
                    raise SystemExit('late device-sized failure')
            stack.enter_context(patch.object(runner, 'run_profile', profile))
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
            with self.assertRaises(SystemExit):
                home.complete(out)
            self.assertEqual(len(count),5)
            self.assertFalse((out/'derby-complete.json').exists())
            self.assertIn('late device-sized failure',(out/'derby-m32-telemetry.log').read_text())

    def test_source_mutation_invalidates_in_memory_node_validation(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.ExitStack() as stack:
            root=Path(tmp)
            app=root/'apps/derby'
            app.mkdir(parents=True)
            source=app/'scene.js'
            source.write_text('first')
            for name in runner.NODE_TESTS:
                p=root/'tools/games'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('check')
            node=root/'node';node.write_text('node executable identity')
            stack.enter_context(patch.object(runner,'ROOT',root))
            stack.enter_context(patch.object(runner,'CACHE',root/'cache'))
            stack.enter_context(patch.object(runner.shutil,'which',return_value=str(node)))
            validation=runner.node_inputs()
            runner.require_node_inputs(validation)
            stack.enter_context(patch.object(runner,'run_suite',side_effect=lambda *args:source.write_text('changed')))
            with self.assertRaisesRegex(SystemExit,'Node validation inputs changed'):
                runner.run_profile(runner.parse_args([]),validation)

    def test_preview_export_lock_precedes_the_profile_writer_lock(self):
        calls=[]
        @contextlib.contextmanager
        def lock(path):
            calls.append(path)
            yield
        with patch.object(runner,'workspace_lock',lock),patch.object(runner,'require_node_inputs'),patch.object(runner,'run_suite'):
            args=runner.parse_args(['--ppm'])
            runner.run_profile(args,'fresh')
        self.assertEqual(calls,[runner.CACHE/'preview.lock',runner.profile_cache(args)/'workspace.lock'])


if __name__ == '__main__':
    unittest.main()
