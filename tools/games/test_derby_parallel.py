"""Preserve the frozen suite and test real output/process ownership boundaries."""
import contextlib
import io
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import run_derby as runner


def options(**changes):
    values = dict(m32=False, telemetry=False, measure=False, course='seed',
                  ppm=False, heap_limit=None, oracle=False)
    values.update(changes)
    return SimpleNamespace(**values)


# Frozen selection from 8863acf5, independent of the new scheduler.
ORIGINAL = [(1, {}), (0, {}), (2, {})] + [
    (t, {'DERBY_NOCAM': '1'}) for t in (0, 1, 2)] + [
    (t, {'DERBY_PICK': '7'}) for t in (0, 2)] + [
    (t, {'DERBY_NOCAM': '1', 'DERBY_PAN': '0'}) for t in (0, 2)] + [
    (0, {'DERBY_NOCAM': '1', 'DERBY_PAN': '0,1', 'DERBY_PANFACE': '1',
         'DERBY_JS': 'VS[0]=700;VS[1]=150'}),
    (1, {'DERBY_NOCAM': '1', 'DERBY_SER_ORACLE': '1'})] + [
    (t, {'DERBY_NOCAM': '1', 'DERBY_PAN': '0', 'DERBY_SER_ORACLE': '1'}) for t in (0, 2)]


class ParallelContracts(unittest.TestCase):
    def test_exact_existing_selection_including_oracle_and_m32(self):
        for m32 in (False, True):
            for oracle in (False, True):
                for heap in (None, 163840):
                    expected = [(1, {})] if heap else ORIGINAL
                    if oracle:
                        expected = [(t, dict(e, DERBY_SER_ORACLE='1')) for t, e in expected
                                    if 'DERBY_SER_ORACLE' not in e]
                    if m32:
                        expected = [(t, e) for t, e in expected if 'DERBY_SER_ORACLE' not in e]
                    self.assertEqual(runner.run_matrix(options(m32=m32, oracle=oracle, heap_limit=heap)), expected)
        self.assertEqual(len(ORIGINAL), 14)

    def test_all_generator_and_binary_profiles_are_distinct_and_stable(self):
        paths = {runner.profile_cache(options(m32=m, telemetry=t, measure=d, course=c))
                 for m in (False, True) for t in (False, True)
                 for d in (False, True) for c in ('seed', 'straight', 'oval')}
        self.assertEqual(len(paths), 24)
        self.assertEqual(runner.profile_cache(options()), runner.profile_cache(options(heap_limit=524288)))

    def test_same_profile_excludes_writers_but_other_profile_can_proceed(self):
        with tempfile.TemporaryDirectory() as tmp:
            lock = Path(tmp)/'profile/workspace.lock'
            other = Path(tmp)/'other/workspace.lock'
            started, acquired = threading.Event(), threading.Event()
            def contender():
                started.set()
                with runner.workspace_lock(lock):
                    acquired.set()
            with runner.workspace_lock(lock):
                thread = threading.Thread(target=contender)
                thread.start()
                self.assertTrue(started.wait(2))
                self.assertFalse(acquired.wait(.05))
                with runner.workspace_lock(other):
                    self.assertFalse(acquired.is_set())
            thread.join(2)
            self.assertFalse(thread.is_alive())
            self.assertTrue(acquired.is_set())

    def exercise(self, failure=None):
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp)
            stale = cache/'results/cases/stale.csv'
            stale.parent.mkdir(parents=True)
            stale.write_text('old subset result')
            ready = threading.Event()
            barrier = threading.Barrier(2)
            seen, commands, replays = [], [], []
            guard = threading.Lock()
            def build(*args, **kwargs):
                ready.set()
                return cache/'binary'
            def capture(binary, env):
                self.assertTrue(ready.is_set(), 'generation/link must finish first')
                with guard:
                    seen.append(dict(env))
                if len(seen) <= 2:
                    barrier.wait(2)
                Path(env['DERBY_CSV']).write_text(env['DERBY_TIER'])
                index = int(Path(env['DERBY_CSV']).parent.name)
                code = 7 if failure == 'exit' and index == 13 else 0
                finish = 'different' if failure == 'finish' and index == 13 else 'same'
                return subprocess.CompletedProcess([str(binary)], code, f'finish: {finish}\n', 'fault' if code else '')
            def command(cmd, **kwargs):
                commands.append(cmd)
                return subprocess.CompletedProcess(cmd, 0, '', '')
            def replay(binary, env, out):
                self.assertIn('finish: same', out)
                self.assertEqual(len(seen), 14, 'pool must join before dependent replays')
                replays.append(out)
            with contextlib.ExitStack() as stack:
                for name, value in [('build', build), ('capture_case', capture),
                                    ('lower', lambda src, dst: dst), ('force_course', lambda app, course: app),
                                    ('check_demo_matches_play', replay), ('check_seeds', replay)]:
                    stack.enter_context(patch.object(runner, name, value))
                stack.enter_context(patch.object(runner.subprocess, 'run', command))
                stack.enter_context(patch.object(runner.os, 'cpu_count', return_value=2))
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                runner.run_suite(options(), cache)
            self.assertFalse(stale.exists())
            self.assertEqual(len(seen), 14)
            self.assertEqual(len({e['DERBY_CSV'] for e in seen}), 14)
            self.assertEqual(len({e['DERBY_APP_DIR'] for e in seen}), 1)
            self.assertEqual(len(replays), 2)
            self.assertEqual([c for c in commands if 'tune_derby.mjs' in ' '.join(c)], [
                ['node','tools/games/tune_derby.mjs','20000','--check'],
                ['node','tools/games/tune_derby.mjs','20000','--check','--course=oval']])
            self.assertIn(['node','tools/games/derby_corner.mjs','app','500'],commands)

    def test_every_case_runs_with_isolated_csv_and_dependent_replays(self):
        self.exercise()

    def test_late_worker_error_cannot_pass_the_suite(self):
        with self.assertRaises(SystemExit):
            self.exercise('exit')

    def test_cross_mode_finish_mismatch_cannot_pass_the_suite(self):
        with self.assertRaises(SystemExit):
            self.exercise('finish')


if __name__ == '__main__':
    unittest.main()
