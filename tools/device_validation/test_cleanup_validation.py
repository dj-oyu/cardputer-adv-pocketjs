"""Offline cleanup fixtures and mocked Git only. Never use a real repository/device."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import cleanup_validation as cleanup

SHA = 'a' * 40


class FakeGit:
    def __init__(self, fixture):
        self.f = fixture
        self.calls = []
        self.records = {str(fixture.wt): {'HEAD': SHA, 'detached': ''}}
        self.dirty = ''
        self.dirty_by_path = {}
        self.flags = 'H main.c\0H dependencies.lock\0'
        self.stages = '100644 ' + SHA + ' 0\tmain.c\0' + '100644 ' + SHA + ' 0\tdependencies.lock\0'
        self.fail_remove = False
        self.bad_common = False

    def __call__(self, repo, *args):
        self.calls.append((str(repo), args))
        if args == ('rev-parse', '--show-toplevel'):
            return str(self.f.repo) + '\n'
        if args == ('worktree', 'list', '--porcelain', '-z'):
            result = f'worktree {self.f.repo}\0HEAD {SHA}\0branch refs/heads/main\0\0'
            for path, fields in self.records.items():
                result += 'worktree ' + path + '\0'
                result += ''.join(key + (' ' + value if value else '') + '\0' for key, value in fields.items())
                result += '\0'
            return result
        if args == ('rev-parse', '--path-format=absolute', '--git-common-dir'):
            if self.bad_common and Path(repo) == self.f.wt:
                return str(self.f.temp / 'different-git')
            return str(self.f.repo / '.git')
        if args == ('rev-parse', '--absolute-git-dir'):
            return str(self.f.repo / '.git' / 'worktrees' / 'wt-integrated')
        if args == ('rev-parse', 'HEAD'):
            return SHA
        if args == ('status', '--porcelain', '--untracked-files=no'):
            return self.dirty_by_path.get(str(repo), self.dirty)
        if args == ('ls-files', '-v', '-z'):
            return self.flags
        if args == ('ls-files', '--stage', '-z'):
            return self.stages
        if args[:2] == ('worktree', 'remove'):
            if self.fail_remove:
                raise cleanup.Refused('Mock Git refused removal')
            if len(args) != 3:
                raise AssertionError('Removal must use exactly one path and no force flag')
            # Simulate disappearance by moving fixture source outside the run.
            # No deletion command or real Git worktree is used in these tests.
            target = Path(args[2])
            target.rename(self.f.temp / ('mock-removed-' + target.name))
            del self.records[str(target)]
            return ''
        raise AssertionError((repo, args))


class CleanupTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='cleanup-offline-fixture-')
        self.addCleanup(self.directory.cleanup)
        self.temp = Path(self.directory.name)
        self.repo = self.temp / 'source repo'
        self.root = self.temp / 'validation' / 'integrated-001'
        self.wt = self.root / 'wt-integrated'
        (self.repo / '.git').mkdir(parents=True)
        self.wt.mkdir(parents=True)
        (self.wt / '.git').write_text('gitdir: fake')
        (self.wt / 'main.c').write_text('source')
        (self.wt / 'dependencies.lock').write_text('tracked input')
        (self.root / 'integrated-build.log').write_text('preserve log')
        self.manifest = {'schema': 'pocketjs-integrated-build-v1', 'manifestType': 'integrated-build-only',
                         'status': 'failed', 'sourceRepository': str(self.repo),
                         'createdUtc': '2000-01-01T00:00:00Z',
                         'builds': [{'role': 'integrated', 'sha': SHA, 'status': 'failed',
                                     'worktree': str(self.wt), 'build': str(self.wt / 'build_integrated')}]}
        self.write_manifest()
        self.git = FakeGit(self)

    def write_manifest(self):
        (self.root / 'manifest.json').write_text(json.dumps(self.manifest), encoding='utf-8-sig')

    def inspect(self):
        return cleanup.inspect_run(self.repo, self.root, self.git)

    def refuse(self, pattern):
        with self.assertRaisesRegex(cleanup.Refused, pattern):
            self.inspect()
        self.assertFalse(any(args[:2] == ('worktree', 'remove') for _, args in self.git.calls))
        self.assertFalse(any(p.name.startswith('cleanup-archive-') for p in self.root.iterdir()))

    def generated(self, relative='build_integrated/log/build.log'):
        path = self.wt / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('keep this generated content')
        return path

    def test_preview_is_read_only_and_reports_exact_paths_sizes(self):
        path = self.generated()
        before = sorted(str(p) for p in self.temp.rglob('*'))
        plan = self.inspect()
        self.assertEqual(plan['mode'], 'preview')
        self.assertEqual(plan['worktrees'][0]['archive'], [
            {'path': str(self.wt / 'build_integrated'), 'relative': 'build_integrated',
             'files': 1, 'bytes': path.stat().st_size}])
        self.assertEqual(before, sorted(str(p) for p in self.temp.rglob('*')))
        self.assertFalse(any(args[:2] == ('worktree', 'remove') for _, args in self.git.calls))

    def test_removal_archives_generated_unknown_content_and_keeps_logs(self):
        self.generated()
        self.generated('build_integrated/my-important-notes.txt')
        self.generated('.cache/bmi270/.git/config')
        self.generated('managed_components/component/file.c')
        result = cleanup.remove_run(self.inspect(), self.git)
        archive = Path(result['archiveRoot'])
        self.assertFalse(self.wt.exists())
        self.assertEqual((archive / 'integrated/build_integrated/log/build.log').read_text(), 'keep this generated content')
        self.assertTrue((archive / 'integrated/build_integrated/my-important-notes.txt').exists())
        self.assertTrue((archive / 'integrated/.cache/bmi270/.git/config').exists())
        self.assertTrue((self.root / 'integrated-build.log').exists())
        self.assertEqual(json.loads((self.root / 'manifest.json').read_text(encoding='utf-8-sig')), self.manifest)
        self.assertTrue((archive / 'receipt.json').exists())
        self.assertEqual([args for _, args in self.git.calls if args[:2] == ('worktree', 'remove')],
                         [('worktree', 'remove', str(self.wt))])

    def test_old_building_manifest_is_still_active(self):
        self.manifest['status'] = 'building'
        self.write_manifest()
        self.refuse('Only terminal')

    def test_failed_manifest_with_configuring_entry_is_refused(self):
        self.manifest['builds'][0]['status'] = 'configuring'
        self.write_manifest()
        self.refuse('Active or unknown')

    def test_planned_manifest_refused(self):
        self.manifest['status'] = 'planned'
        self.write_manifest()
        self.refuse('Only terminal')

    def test_failed_run_with_uncreated_worktree_is_noop(self):
        self.wt.rename(self.temp / 'fixture-source-moved')
        self.git.records.clear()
        self.manifest['builds'][0]['status'] = 'planned'
        self.write_manifest()
        result = cleanup.remove_run(self.inspect(), self.git)
        self.assertEqual(result['mode'], 'no worktrees to remove')

    def test_registered_missing_worktree_is_refused(self):
        self.wt.rename(self.temp / 'fixture-source-moved')
        self.refuse('still registered')

    def comparison(self):
        old = self.wt
        self.wt = self.root / 'wt-baseline'
        old.rename(self.wt)
        candidate = self.root / 'wt-candidate'
        candidate.mkdir()
        for name in ('.git', 'main.c', 'dependencies.lock'):
            (candidate / name).write_bytes((self.wt / name).read_bytes())
        self.manifest['schema'] = 1
        self.manifest.pop('manifestType')
        self.manifest['builds'] = [
            {'role': role, 'sha': SHA, 'status': 'built', 'worktree': str(wt),
             'build': str(wt / 'build_device_validation')}
            for role, wt in [('baseline', self.wt), ('candidate', candidate)]]
        self.write_manifest()
        self.git.records = {str(wt): {'HEAD': SHA, 'detached': ''} for wt in (self.wt, candidate)}
        return candidate

    def test_comparison_removes_both_only_after_full_preflight(self):
        candidate = self.comparison()
        self.generated('build_device_validation/build.log')
        result = cleanup.remove_run(self.inspect(), self.git)
        self.assertFalse(self.wt.exists())
        self.assertFalse(candidate.exists())
        removed = [e for e in result['completed'] if 'removedWorktree' in e]
        self.assertEqual(len(removed), 2)
        self.assertTrue((Path(result['archiveRoot']) / 'baseline/build_device_validation/build.log').exists())

    def test_dirty_candidate_prevents_any_baseline_moves(self):
        candidate = self.comparison()
        path = self.generated('build_device_validation/build.log')
        self.git.dirty_by_path[str(candidate)] = ' M main.c'
        self.refuse('Tracked changes')
        self.assertTrue(path.exists())

    def test_active_last_flash_record_refused(self):
        (self.root / 'last-flash.json').write_text('{"status":"flashing"}')
        self.refuse('Last-flash')

    def test_integrated_device_lock_refused_even_for_built_manifest(self):
        self.manifest['status'] = 'built'
        self.write_manifest()
        (self.root / '.integrated-device.lock').mkdir()
        self.refuse('Integrated device lock exists')
        self.assertTrue((self.root / '.integrated-device.lock').exists())

    def test_active_integrated_flash_record_refused(self):
        (self.root / 'last-integrated-flash.json').write_text('{"status":"flashing"}')
        self.refuse('last-integrated-flash')

    def test_built_manifest_with_incomplete_entry_refused(self):
        self.manifest['status'] = 'built'
        self.manifest['builds'][0]['status'] = 'planned'
        self.write_manifest()
        self.refuse('Active or unknown')

    def test_invalid_device_result_shape_refused(self):
        stage = self.root / 'device-integrated-example'
        stage.mkdir()
        (stage / 'result.json').write_text('[]')
        self.refuse('active or unrecognized')

    def test_source_repository_mismatch(self):
        self.manifest['sourceRepository'] = str(self.temp / 'other-repo')
        self.write_manifest()
        self.refuse('sourceRepository')

    def test_wrong_manifest_worktree_and_build_paths(self):
        for key in ('worktree', 'build'):
            with self.subTest(key=key):
                original = self.manifest['builds'][0][key]
                self.manifest['builds'][0][key] = str(self.temp / 'user-data')
                self.write_manifest()
                self.refuse('exact harness layout')
                self.manifest['builds'][0][key] = original

    def test_unsupported_schema_and_duplicate_roles(self):
        self.manifest['schema'] = 2
        self.write_manifest()
        self.refuse('Unsupported')
        self.manifest['schema'] = 1
        self.manifest['builds'] *= 2
        self.write_manifest()
        self.refuse('Missing or duplicated')

    def test_attached_locked_and_changed_head_worktrees_refused(self):
        cases = [{'HEAD': SHA, 'branch': 'refs/heads/user'}, {'HEAD': SHA, 'detached': '', 'locked': 'busy'},
                 {'HEAD': 'b' * 40, 'detached': ''}, {'HEAD': SHA, 'detached': '', 'prunable': 'missing'}]
        for record in cases:
            with self.subTest(record=record):
                self.git.records[str(self.wt)] = record
                self.refuse('unlocked, detached')

    def test_dirty_dependencies_lock_refused(self):
        self.git.dirty = ' M dependencies.lock\n'
        self.refuse('Tracked changes')

    def test_unknown_untracked_and_ignored_files_refused(self):
        for relative in ('personal.txt', '.env', 'tools/__pycache__/local.pyc', '.cache/unexpected/notes.txt'):
            with self.subTest(relative=relative):
                path = self.generated(relative)
                self.refuse('Unknown untracked/ignored')
                path.unlink()  # Only this test fixture, never repository data.

    def test_nested_linked_worktree_metadata_refused(self):
        self.generated('.cache/bmi270/.git')
        self.refuse('Nested linked-worktree')

    def test_tracked_file_inside_generated_path_refused(self):
        self.git.stages += '100644 ' + SHA + ' 0\tbuild_integrated/source.c\0'
        self.refuse('contains tracked source')

    def test_hidden_tracked_changes_and_submodules_refused(self):
        self.git.flags = 'h main.c\0'
        self.refuse('assume-unchanged')
        self.git.flags = 'S main.c\0'
        self.refuse('skip-worktree')
        self.git.flags = 'H main.c\0'
        self.git.stages += '160000 ' + SHA + ' 0\tmodule\0'
        self.refuse('Submodules')

    def test_other_worktree_under_root_refused(self):
        self.git.records[str(self.root / 'user-work')] = {'HEAD': SHA, 'detached': ''}
        self.refuse('Unexpected worktree')

    def test_root_inside_repo_or_another_worktree_refused(self):
        with self.assertRaisesRegex(cleanup.Refused, 'non-overlapping'):
            cleanup.inspect_run(self.repo, self.repo / 'nested', self.git)
        self.git.records[str(self.root.parent)] = {'HEAD': SHA, 'detached': ''}
        self.refuse('outside every')

    def test_repository_common_directory_mismatch(self):
        self.git.bad_common = True
        self.refuse('ownership')

    def test_nested_symlink_refused_even_inside_generated_directory(self):
        path = self.generated()
        link = path.parent / 'alias'
        try:
            link.symlink_to(self.repo, target_is_directory=True)
        except OSError:
            self.skipTest('Symlink creation unavailable on this test host')
        self.refuse('Symlink/junction')

    def test_symlink_in_root_ancestor_refused(self):
        alias = self.temp / 'alias'
        try:
            alias.symlink_to(self.root.parent, target_is_directory=True)
        except OSError:
            self.skipTest('Symlink creation unavailable on this test host')
        with self.assertRaisesRegex(cleanup.Refused, 'Symlink/junction'):
            cleanup.inspect_run(self.repo, alias / self.root.name, self.git)

    def test_windows_reparse_attribute_is_refused_without_following(self):
        info = mock.Mock(st_mode=0o040755, st_file_attributes=0x400)
        with mock.patch.object(Path, 'lstat', return_value=info):
            with self.assertRaisesRegex(cleanup.Refused, 'reparse'):
                cleanup.plain(self.root)

    def test_wildcards_relative_and_traversal_paths_refused(self):
        for value in ('relative', str(self.temp / '*' ), str(self.temp / '..' / 'outside')):
            with self.subTest(value=value), self.assertRaises(cleanup.Refused):
                cleanup.full_path(value)

    def test_incomplete_or_active_device_stage_refused(self):
        stage = self.root / 'device-integrated-example'
        stage.mkdir()
        self.refuse('no terminal result')
        (stage / 'result.json').write_text('{"status":"preflight"}')
        self.refuse('active or unrecognized')

    def test_manifest_change_between_preview_and_removal_refused(self):
        plan = self.inspect()
        self.manifest['status'] = 'building'
        self.write_manifest()
        with self.assertRaisesRegex(cleanup.Refused, 'Only terminal'):
            cleanup.remove_run(plan, self.git)
        self.assertTrue(self.wt.exists())

    def test_git_failure_preserves_archive_and_receipt(self):
        self.generated()
        self.git.fail_remove = True
        with self.assertRaisesRegex(cleanup.Refused, 'Retained cleanup receipt'):
            cleanup.remove_run(self.inspect(), self.git)
        self.assertTrue(self.wt.exists())
        archives = [p for p in self.root.iterdir() if p.name.startswith('cleanup-archive-')]
        self.assertEqual(len(archives), 1)
        self.assertTrue((archives[0] / 'integrated/build_integrated/log/build.log').is_file())
        receipt = json.loads((archives[0] / 'receipt.json').read_text())
        self.assertIn('incomplete', receipt['mode'])

    def test_repeat_after_success_is_noop_and_preserves_archive(self):
        self.generated()
        first = cleanup.remove_run(self.inspect(), self.git)
        second = cleanup.remove_run(self.inspect(), self.git)
        self.assertEqual(second['mode'], 'no worktrees to remove')
        self.assertTrue(Path(first['archiveRoot']).exists())

    def test_cli_requires_attestation_before_even_reading_git(self):
        with mock.patch.object(cleanup, 'Git') as git, mock.patch('sys.stderr'):
            self.assertEqual(cleanup.main(['--repo', str(self.repo), '--run-root', str(self.root), '--remove']), 2)
            git.assert_not_called()

    def test_git_discards_inherited_repository_selectors(self):
        with mock.patch.dict(cleanup.os.environ, {'GIT_DIR': 'bad', 'GIT_WORK_TREE': 'bad', 'GIT_INDEX_FILE': 'bad'}), \
                mock.patch.object(cleanup.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, b'ok', b'')) as run:
            self.assertEqual(cleanup.Git()(self.repo, 'status'), 'ok')
            env = run.call_args.kwargs['env']
            self.assertNotIn('GIT_DIR', env)
            self.assertNotIn('GIT_WORK_TREE', env)
            self.assertNotIn('GIT_INDEX_FILE', env)
            self.assertEqual(env['GIT_OPTIONAL_LOCKS'], '0')

    def test_powershell_wrapper_forwards_explicit_remove_flags(self):
        script = (Path(__file__).parent / 'Cleanup-Validation.ps1').read_text()
        self.assertIn('if ($Remove -and !$ConfirmNoActiveProcesses)', script)
        self.assertIn("if ($Remove) { $arguments += '--remove' }", script)
        self.assertIn("Resolve-PythonApplication $Python", script)
        self.assertIn('if ($LASTEXITCODE -ne 0)', script)
        self.assertNotIn('Remove-Item', script)


if __name__ == '__main__':
    unittest.main()
