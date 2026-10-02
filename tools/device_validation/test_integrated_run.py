"""Offline manifest/evidence fixtures and static device entry-point contracts."""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import verify_integrated_run as check

ROOT = Path(__file__).resolve().parent
COMMIT = '2' * 40


class RunFixture:
    def __init__(self, root):
        self.root = root
        self.worktree = root / 'wt-integrated'
        self.build = self.worktree / 'build_integrated'
        self.build.mkdir(parents=True)
        (self.worktree / 'main/ui').mkdir(parents=True)
        (self.worktree / 'apps/kasane').mkdir(parents=True)
        (self.worktree / 'partitions.csv').write_text('factory,app,factory,0x10000,0x300000,\n')
        ids = ['"local.hello"'] + ['NULL'] * 8 + ['"local.gridlab"'] + ['NULL'] * 4
        (self.worktree / 'main/ui/shell.c').write_text('app_ids[]={' + ','.join(ids) + '};')
        (self.worktree / 'apps/kasane/grid_lab.js').write_text('grid.measure(mode.handle, buffers, undefined, 8)')
        (self.worktree / 'dependencies.lock').write_text('resolved dependency inputs\n')
        self.response = self.build / 'toolchain/cflags'
        self.response.parent.mkdir()
        self.response.write_text('-DPOCKET_VM_TYPED_PUT_INT_FAST=0')
        self.database = [{'file': 'quickjs.c', 'directory': str(self.build),
                          'arguments': ['cc', '@toolchain/cflags', '-c', 'quickjs.c']}]
        for name in check.ARTIFACTS:
            (self.build / name).write_text('nonempty ' + name)
        (self.build / 'sdkconfig').write_text('CONFIG_IDF_TARGET="esp32s3"\n')
        (self.build / 'CMakeCache.txt').write_text('POCKET_APP_PRECOMPILE_EXPERIMENT:BOOL=OFF\nKASANE_P0_PROBE:BOOL=OFF\n')
        (self.build / 'compile_commands.json').write_text(json.dumps(self.database))
        self.evidence = check.verify_flags(self.database, 0)
        self.write_flags()
        self.entry = {'role': 'integrated', 'sha': COMMIT, 'status': 'built',
                      'configurationVerified': True, 'worktree': str(self.worktree),
                      'build': str(self.build), 'binary': str(self.build / check.ARTIFACTS[0]),
                      'cmakeOptions': ['-DSDKCONFIG=' + str(self.build) + '/sdkconfig',
                                       '-DSDKCONFIG_DEFAULTS=' + str(self.worktree) + '/sdkconfig.defaults',
                                       '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
                                       '-DCMAKE_C_FLAGS=-DPOCKET_VM_TYPED_PUT_INT_FAST=0',
                                       '-DPOCKET_APP_PRECOMPILE_EXPERIMENT=OFF'],
                      'dependencyLock': {'path': str(self.worktree / 'dependencies.lock'),
                                         'sha256': check.digest(self.worktree / 'dependencies.lock')}}
        self.run = {'schema': 'pocketjs-integrated-build-v1', 'manifestType': 'integrated-build-only',
                    'status': 'built', 'failure': None, 'commit': COMMIT,
                    'typedPutEnabled': False, 'precompileExperimentEnabled': False,
                    'runtimePrecompileIntegrated': False, 'builds': [self.entry]}
        self.refresh_artifacts()

    def write_flags(self, encoding='utf-8'):
        (self.root / 'integrated-build-flags.log').write_text(json.dumps(self.evidence), encoding=encoding)

    def refresh_artifacts(self):
        self.entry['artifacts'] = [{'name': name, 'path': str(self.build / name),
                                    'bytes': (self.build / name).stat().st_size,
                                    'sha256': check.digest(self.build / name)} for name in check.ARTIFACTS]
        self.entry['binaryBytes'] = (self.build / check.ARTIFACTS[0]).stat().st_size
        self.entry['binarySha256'] = check.digest(self.build / check.ARTIFACTS[0])
        self.entry['sdkconfigSha256'] = check.digest(self.build / 'sdkconfig')
        self.save()

    def save(self):
        (self.root / 'manifest.json').write_text(json.dumps(self.run), encoding='utf-8-sig')


class IntegratedRunTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.fixture = RunFixture(Path(self.temp.name))
        self.git = patch.object(check, 'git', side_effect=lambda wt, cmd, *args: COMMIT if cmd == 'rev-parse' else '')
        self.git.start()
        self.addCleanup(self.git.stop)

    def verify(self):
        return check.verify_run(self.fixture.root, COMMIT)

    def test_valid_completed_original_run(self):
        result = self.verify()
        self.assertEqual(result['role'], 'integrated')
        self.assertEqual(result['sourceSha'], COMMIT)
        self.assertEqual(result['manifestSha256'], check.digest(self.fixture.root / 'manifest.json'))

    def test_powershell_51_utf16_flag_log(self):
        self.fixture.write_flags('utf-16')
        self.assertEqual(self.verify()['status'], 'preflight-verified')

    def test_windows_crlf_configuration(self):
        for name in ('sdkconfig', 'CMakeCache.txt'):
            path = self.fixture.build / name
            path.write_bytes(path.read_bytes().replace(b'\n', b'\r\n'))
        self.fixture.refresh_artifacts()
        self.assertEqual(self.verify()['status'], 'preflight-verified')

    def test_schema_state_and_exact_boolean_guards(self):
        original = copy.deepcopy(self.fixture.run)
        for key, value in [('schema', 1), ('manifestType', 'comparison'), ('status', 'building'),
                           ('status', 'failed'), ('failure', 'stopped'), ('typedPutEnabled', True),
                           ('typedPutEnabled', 0), ('precompileExperimentEnabled', None),
                           ('runtimePrecompileIntegrated', 'false'), ('commit', '3' * 40)]:
            with self.subTest(key=key, value=value):
                self.fixture.run = copy.deepcopy(original)
                self.fixture.run[key] = value
                self.fixture.save()
                with self.assertRaises(ValueError):
                    self.verify()

    def test_entry_guards(self):
        original = copy.deepcopy(self.fixture.entry)
        for key, value in [('role', 'candidate'), ('status', 'building'), ('configurationVerified', 1),
                           ('configurationVerified', False), ('sha', '3' * 40), ('cmakeOptions', [])]:
            with self.subTest(key=key, value=value):
                self.fixture.run['builds'] = [dict(original, **{key: value})]
                self.fixture.save()
                with self.assertRaises(ValueError):
                    self.verify()

    def test_missing_or_duplicate_artifacts(self):
        self.fixture.entry['artifacts'][0] = copy.deepcopy(self.fixture.entry['artifacts'][1])
        self.fixture.save()
        with self.assertRaisesRegex(ValueError, 'Missing, duplicate'):
            self.verify()

    def test_each_artifact_is_hashed(self):
        for name in check.ARTIFACTS:
            path = self.fixture.build / name
            original = path.read_bytes()
            path.write_bytes(bytes([original[0] ^ 1]) + original[1:])
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'changed'):
                self.verify()
            path.write_bytes(original)

    def test_extra_build_rejected(self):
        self.fixture.run['builds'].append(self.fixture.entry)
        self.fixture.save()
        with self.assertRaisesRegex(ValueError, 'Exactly one'):
            self.verify()

    def test_changed_dependency_lock(self):
        (self.fixture.worktree / 'dependencies.lock').write_text('changed')
        with self.assertRaisesRegex(ValueError, 'Artifact changed'):
            self.verify()

    def test_response_file_tamper_even_when_h_stays_off(self):
        self.fixture.response.write_text('-O0 -DPOCKET_VM_TYPED_PUT_INT_FAST=0')
        with self.assertRaisesRegex(ValueError, 'evidence changed'):
            self.verify()

    def test_h_enabled_in_response_file(self):
        self.fixture.response.write_text('-DPOCKET_VM_TYPED_PUT_INT_FAST=1')
        with self.assertRaisesRegex(ValueError, 'H=0'):
            self.verify()

    def test_missing_response_file(self):
        self.fixture.response.unlink()
        with self.assertRaisesRegex(ValueError, 'Cannot read response'):
            self.verify()

    def test_diagnostic_config_and_f_cache(self):
        for name, contents in [('sdkconfig', 'CONFIG_IDF_TARGET="esp32s3"\nCONFIG_POCKET_VM_OOMPROBE=y\n'),
                               ('CMakeCache.txt', 'POCKET_APP_PRECOMPILE_EXPERIMENT:BOOL=ON\n'),
                               ('CMakeCache.txt', 'POCKET_APP_PRECOMPILE_EXPERIMENT:BOOL=OFF\nPOCKET_HEAPPROBE:BOOL=ON\n'),
                               ('CMakeCache.txt', 'POCKET_APP_PRECOMPILE_EXPERIMENT:BOOL=OFF\nKASANE_FLOWER_ALLOC_FAULT_PROBE:BOOL=ON\n')]:
            path = self.fixture.build / name
            original = path.read_bytes()
            path.write_text(contents)
            self.fixture.refresh_artifacts()
            with self.subTest(contents=contents), self.assertRaises(ValueError):
                self.verify()
            path.write_bytes(original)
            self.fixture.refresh_artifacts()

    def test_changed_source_or_head(self):
        for outputs in [('3' * 40, ''), (COMMIT, ' M apps/kasane/grid_lab.js')]:
            with patch.object(check, 'git', side_effect=outputs), self.assertRaisesRegex(ValueError, 'changed'):
                self.verify()

    def test_changed_navigation(self):
        (self.fixture.worktree / 'main/ui/shell.c').write_text('app_ids[]={"local.stress"};')
        with self.assertRaisesRegex(ValueError, 'navigation changed'):
            self.verify()

    def test_app_partition_and_bytecode_guards(self):
        (self.fixture.worktree / 'partitions.csv').write_text('factory,app,factory,0x20000,0x300000,\n')
        with self.assertRaisesRegex(ValueError, 'partition layout'):
            self.verify()
        (self.fixture.worktree / 'partitions.csv').write_text('factory,app,factory,0x10000,0x300000,\n')
        (self.fixture.build / 'experimental-bytecode').mkdir()
        (self.fixture.build / 'experimental-bytecode/app.bc').write_bytes(b'bytecode')
        with self.assertRaisesRegex(ValueError, 'F bytecode'):
            self.verify()

    def test_symlink_refused(self):
        source = self.fixture.build / 'cardputer_pocketjs.map'
        target = self.fixture.build / 'map-copy'
        source.rename(target)
        try:
            source.symlink_to(target)
        except OSError:
            self.skipTest('This platform does not permit test symlinks')
        with self.assertRaisesRegex(ValueError, 'Symlinks'):
            self.verify()

    def test_duplicate_json_keys_and_wrong_expected_commit(self):
        with self.assertRaisesRegex(ValueError, 'differs from -Commit'):
            check.verify_run(self.fixture.root, 'a' * 40)
        path = self.fixture.root / 'manifest.json'
        path.write_text('{"schema": 1, "schema": 2}')
        with self.assertRaisesRegex(ValueError, 'Duplicate JSON'):
            self.verify()


class EntryPointGuards(unittest.TestCase):
    def test_unverified_recovery_is_explicit_and_keeps_file_checks(self):
        flash = (ROOT / 'Flash-Integrated.ps1').read_text()
        self.assertIn('[Parameter(Mandatory=$true)][string]$RecoveryImage', flash)
        self.assertIn('[switch]$AllowUnverifiedRecovery', flash)
        self.assertIn("recoveryConfidence=$(if ($AllowUnverifiedRecovery) { 'unverified-candidate' } else { 'user-attested-known-good' })", flash)
        self.assertIn('no verified rollback guarantee', flash)
        self.assertIn("{ 'unverified-recovery-app.bin' } else { 'known-good-app.bin' }", flash)
        self.assertIn('(Get-Item -LiteralPath $RecoveryImage).Length -le 0', flash)
        self.assertIn('(Get-Item -LiteralPath $RecoveryImage).Length -gt 0x300000', flash)
        self.assertIn('(Get-FileHash -Algorithm SHA256 -LiteralPath $recoveryCopy).Hash -ne $recoveryHash', flash)
        self.assertIn("@('-m','esptool','image-info',$recoveryCopy)", flash)
        self.assertLess(flash.index("'recovery-image-info.log'"), flash.index("@('write-flash'"))
        fixture = (ROOT / 'Test-IntegratedOffline.ps1').read_text()
        self.assertIn("$record.recoveryConfidence -ne 'user-attested-known-good'", fixture)
        self.assertIn("$record.recoveryConfidence -ne 'unverified-candidate'", fixture)

    def test_git_ignores_inherited_repository_and_config_selectors(self):
        selectors = {'GIT_DIR': '/wrong/repository', 'GIT_WORK_TREE': '/wrong/tree',
                     'GIT_INDEX_FILE': '/wrong/index', 'GIT_COMMON_DIR': '/wrong/common',
                     'GIT_CONFIG_COUNT': '1', 'GIT_CONFIG_KEY_0': 'core.worktree',
                     'GIT_CONFIG_VALUE_0': '/wrong/worktree', 'GIT_OPTIONAL_LOCKS': '1',
                     'git_object_directory': '/wrong/objects', 'PATH': 'preserve-this-path'}
        with patch.dict(os.environ, selectors), patch.object(check.subprocess, 'run') as native:
            native.return_value = subprocess.CompletedProcess([], 0, stdout=COMMIT + '\n')
            self.assertEqual(check.git(Path('/expected/tree'), 'rev-parse', 'HEAD'), COMMIT)
            command = native.call_args.args[0]
            environment = native.call_args.kwargs['env']
            self.assertEqual(command, ['git', '-c', 'core.fsmonitor=false', '-C',
                                       str(Path('/expected/tree')), 'rev-parse', 'HEAD'])
            self.assertEqual({key: value for key, value in environment.items()
                              if key.upper().startswith('GIT_')}, {'GIT_OPTIONAL_LOCKS': '0'})
            self.assertEqual(environment['PATH'], 'preserve-this-path')
            self.assertEqual(os.environ['GIT_DIR'], '/wrong/repository')

    def test_device_guards_and_no_old_schema_relaxation(self):
        common = (ROOT / 'Common.ps1').read_text()
        self.assertIn('$run.schema -ne 1', common)
        for name, guard in [('Flash-Integrated.ps1', 'if (!$AllowFlash'), ('Test-Integrated.ps1', 'if (!$AllowDevice')]:
            script = (ROOT / name).read_text()
            self.assertLess(script.index(guard), script.index('Get-IdfPython'))
            self.assertIn('$ConfirmDeviceFree', script)
            self.assertIn('Enter-IntegratedRunLock', script)
            self.assertIn('Enter-DeviceLock', script)
            self.assertIn('last-integrated-flash.json', script)
            self.assertIn('sourceSha', script)
            self.assertNotIn('COM3', script)
            self.assertNotIn('Read-Run ', script)
        flash = (ROOT / 'Flash-Integrated.ps1').read_text()
        self.assertIn("'--before','no-reset','--after','no-reset','--no-stub'", flash)
        self.assertIn("@('write-flash','0x10000',$image)", flash)
        self.assertIn("@('verify-flash','0x10000',$image)", flash)
        self.assertNotIn("'erase-flash'", flash)
        self.assertIn('ConfirmCompatibleBootloaderAndPartitions', flash)
        self.assertIn('ConfirmDownloadMode', flash)
        self.assertLess(flash.index('Read-IntegratedEvidence'), flash.index("@('write-flash'"))
        test = (ROOT / 'Test-Integrated.ps1').read_text()
        self.assertIn("'integrated_smoke.py'", test)
        self.assertNotIn('measure_grid_pipeline_device.py', test)
        self.assertNotIn('safe_smoke.py', test)
        self.assertLess(test.index('Enter-DeviceLock'), test.index('Assert-IntegratedFlashRecord'))

    def test_cli_requires_arguments_without_device_import(self):
        result = subprocess.run([sys.executable, str(ROOT / 'verify_integrated_run.py')],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('--run-root', result.stderr)
        self.assertNotIn('serial', result.stderr)


if __name__ == '__main__':
    unittest.main()
