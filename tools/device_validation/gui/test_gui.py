"""Offline fixtures only. Never opens a serial port, uses IDF, or launches PowerShell."""
import hashlib
import json
import os
from pathlib import Path, PosixPath
import tempfile
import threading
import time
import unittest
from unittest.mock import patch, MagicMock

from fastapi.testclient import TestClient

import core
from server import create_app

COMMIT = '2' * 40
PORT = {'device': 'COM9999', 'description': 'Synthetic fixture', 'hwid': 'fixture-only',
        'vid': 0xFFFF, 'pid': 0xFFFF, 'serial_number': None, 'location': 'fixture'}


def app_bytes(suffix=b'target'):
    data = bytearray(40)
    data[0] = 0xE9
    data[12] = 9
    data[32:36] = b'\x32\x54\xcd\xab'
    return bytes(data) + suffix


def wait(broker, response):
    limit = time.monotonic() + 5
    while time.monotonic() < limit:
        job = broker.get_job(response['job_id'])
        if job['status'] in ('succeeded', 'failed'):
            return job
        time.sleep(0.005)
    raise AssertionError('fixture job did not finish')


class BrokerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.project = self.root / 'project'
        self.project.mkdir()
        self.private = self.root / 'private'
        self.private.mkdir()
        self.run = self.root / 'retained-run'
        self.run.mkdir()
        (self.run / 'manifest.json').write_text(json.dumps({'schema': 'pocketjs-integrated-build-v1',
            'status': 'built', 'commit': COMMIT}))
        self.binary = self.run / 'cardputer_pocketjs.bin'
        self.binary.write_bytes(app_bytes())
        self.recovery = self.project / 'app.bin'
        self.recovery.write_bytes(app_bytes(b'recovery'))
        self.evidence = {'status': 'preflight-verified', 'role': 'integrated', 'sourceSha': COMMIT,
            'runRoot': str(self.run), 'binary': str(self.binary), 'binarySha256': core.digest(self.binary),
            'manifestSha256': core.digest(self.run / 'manifest.json')}
        self.commands = []
        self.port_rows = [dict(PORT)]
        self.idf = core.IdF(True, None, 'fixture-python.exe', 'fixture-idf', 'fixture-powershell.exe', {})
        def readonly(command, **kw):
            self.commands.append(command)
            if 'verify_integrated_run.py' in command[1]:
                return json.dumps(self.evidence)
            if 'image-info' in command:
                return 'ESP32-S3\n'
            raise AssertionError('unexpected command: ' + repr(command))
        self.executions = []
        def execute(command, env, log):
            self.executions.append(command)
            log('synthetic command success')
            return 0
        self.broker = core.Broker(self.project, self.private, idf=self.idf,
                                 ports=lambda: self.port_rows, readonly=readonly, execute=execute)
        self.addCleanup(self.broker.close)
        self.run_row = core.run_metadata(self.run)
        self.candidate = core.app_metadata(self.recovery, self.project)
        self.broker.runs[self.run_row['id']] = self.run_row
        self.broker.candidates[self.candidate['id']] = self.candidate

    def selection(self, **changes):
        return dict({'action': 'flash', 'run_id': self.run_row['id'],
                     'recovery_id': self.candidate['id'], 'port': PORT['device'],
                     'confidence': 'unverified-candidate', 'cycles': 3, 'grid': False}, **changes)

    def plan(self, **changes):
        job = wait(self.broker, self.broker.create_plan(self.selection(**changes)))
        self.assertEqual(job['status'], 'succeeded', job['error'])
        return job['result']['plan']

    def execute(self, plan):
        return self.broker.execute_plan(plan['id'], plan['digest'], plan['required_ack'])

    def flash_record(self):
        data = {'schema': 'pocketjs-integrated-flash-v1',
                'status': 'flash-verified-awaiting-manual-reboot', 'role': 'integrated',
                'port': PORT['device'], 'flashId': 'a' * 32,
                **{k: self.evidence[k] for k in ('runRoot', 'sourceSha', 'binarySha256', 'manifestSha256')}}
        (self.run / 'last-integrated-flash.json').write_text(json.dumps(data))
        return data

    def test_plan_is_read_only_and_exact_pinned(self):
        p = self.plan()
        self.assertFalse(self.executions)
        self.assertEqual(p['region'], '0x10000')
        self.assertEqual(p['recoverySha256'], core.digest(self.recovery))
        self.assertIn('recovery-unverified', p['required_ack'])
        self.assertIn('-ExpectedManifestSha256', p['command'])
        self.assertIn('-ExpectedRecoverySha256', p['command'])
        self.assertIn('-ExpectedPortSignature', p['command'])
        self.assertIn('-AllowUnverifiedRecovery', p['command'])
        self.assertFalse(any('write-flash' in arg for command in self.commands for arg in command))

    def test_execute_once_and_no_arbitrary_command(self):
        p = self.plan()
        result = self.execute(p)
        with self.assertRaises(ValueError):
            self.execute(p)
        job = wait(self.broker, result)
        self.assertEqual(job['status'], 'succeeded')
        self.assertEqual(len(self.executions), 1)
        self.assertEqual(self.executions[0], p['command'])
        self.assertNotIn('-Command', self.executions[0])

    def test_missing_ack_or_wrong_digest_does_not_consume(self):
        p = self.plan()
        with self.assertRaises(ValueError):
            self.broker.execute_plan(p['id'], p['digest'], ['device-free'])
        with self.assertRaises(ValueError):
            self.broker.execute_plan(p['id'], '0' * 64, p['required_ack'])
        self.assertIn(p['id'], self.broker.plans)
        self.assertFalse(self.executions)

    def test_changed_recovery_blocks_before_process(self):
        p = self.plan()
        self.recovery.write_bytes(app_bytes(b'changed'))
        job = wait(self.broker, self.execute(p))
        self.assertEqual(job['status'], 'failed')
        self.assertIn('Recovery changed', job['error'])
        self.assertFalse(self.executions)

    def test_changed_evidence_blocks(self):
        p = self.plan()
        self.evidence['manifestSha256'] = 'f' * 64
        self.assertEqual(wait(self.broker, self.execute(p))['status'], 'failed')
        self.assertFalse(self.executions)

    def test_disconnected_or_replaced_port_blocks(self):
        for change in ([], [dict(PORT, hwid='replacement')]):
            with self.subTest(change=change):
                self.port_rows = [dict(PORT)]
                p = self.plan()
                self.port_rows = change
                self.assertEqual(wait(self.broker, self.execute(p))['status'], 'failed')
        self.assertFalse(self.executions)

    def test_public_plan_mutation_does_not_change_authorized_command(self):
        p = self.plan()
        original_command = list(p['command'])
        p['port'] = 'COM8888'
        p['command'] = ['malicious command']
        self.assertEqual(wait(self.broker, self.execute(p))['status'], 'succeeded')
        self.assertEqual(self.executions[0], original_command)

    def test_expiry_blocks(self):
        p = self.plan()
        self.broker.plans[p['id']]['expires'] = time.time() - 1
        with self.assertRaisesRegex(ValueError, 'expired'):
            self.execute(p)
        self.assertFalse(self.executions)

    def test_same_bytes_recovery_rejected(self):
        self.recovery.write_bytes(self.binary.read_bytes())
        job = wait(self.broker, self.broker.create_plan(self.selection()))
        self.assertEqual(job['status'], 'failed')
        self.assertIn('identical bytes', job['error'])

    def test_known_good_requires_attestation(self):
        p = self.plan(confidence='user-attested-known-good')
        self.assertIn('recovery-known-good', p['required_ack'])
        self.assertNotIn('-AllowUnverifiedRecovery', p['command'])

    def test_test_requires_matching_flash(self):
        job = wait(self.broker, self.broker.create_plan(self.selection(action='test', recovery_id='')))
        self.assertEqual(job['status'], 'failed')
        self.flash_record()
        p = self.plan(action='test', recovery_id='', grid=True)
        self.assertEqual(p['required_ack'], ['device-free', 'manual-reboot-home'])
        self.assertIn('-ExpectedFlashId', p['command'])
        self.assertIn('-Grid', p['command'])
        record = self.flash_record()
        record['flashId'] = 'b' * 32
        (self.run / 'last-integrated-flash.json').write_text(json.dumps(record))
        self.assertEqual(wait(self.broker, self.execute(p))['status'], 'failed')
        self.assertFalse(self.executions)

    def test_failed_process_releases_broker_reservation(self):
        self.broker.execute_fn = lambda *args: 7
        p = self.plan()
        job = wait(self.broker, self.execute(p))
        self.assertEqual(job['status'], 'failed')
        self.assertIn('exit 7', job['error'])
        self.assertFalse(self.broker.device_reserved)

    def test_device_reservation_blocks_double_action(self):
        first = self.plan()
        second = self.plan()
        started, finish = threading.Event(), threading.Event()
        def execute(*args):
            started.set()
            finish.wait(5)
            return 0
        self.broker.execute_fn = execute
        result = self.execute(first)
        self.assertTrue(started.wait(2))
        try:
            with self.assertRaisesRegex(ValueError, 'already'):
                self.execute(second)
            with self.assertRaisesRegex(ValueError, 'pending/running'):
                self.broker.create_plan(self.selection())
        finally:
            finish.set()
        self.assertEqual(wait(self.broker, result)['status'], 'succeeded')

    def test_import_external_run_and_port_enumeration_are_jobs(self):
        self.assertEqual(wait(self.broker, self.broker.import_run(str(self.run)))['status'], 'succeeded')
        self.assertEqual(wait(self.broker, self.broker.refresh_ports())['result']['ports'], [PORT])
        self.assertFalse(self.executions)

    def test_log_is_bounded(self):
        result = self.broker.submit('fixture', lambda job: [self.broker.log(job, 'x' * 3000) for _ in range(350)])
        job = wait(self.broker, result)
        self.assertEqual(len(job['log']), 300)
        self.assertEqual(len(job['log'][0]), 2048)
        self.assertLess((self.private / (job['id'] + '.log')).stat().st_size, 1024 * 1024 + 2049)

    def test_disk_logging_failure_does_not_fail_job(self):
        original = Path.open
        def fail_logs(path, *args, **kwargs):
            if path.parent == self.private and path.suffix == '.log':
                raise OSError('synthetic full disk')
            return original(path, *args, **kwargs)
        with patch.object(Path, 'open', fail_logs):
            job = wait(self.broker, self.broker.submit('fixture', lambda job: self.broker.log(job, 'still owned')))
        self.assertEqual(job['status'], 'succeeded')
        self.assertIn('Session disk logging unavailable', job['log'][-1])

    def test_process_waits_despite_logging_failure(self):
        process = MagicMock()
        process.stdout.readline.side_effect = [b'first line', b'second line', b'']
        process.returncode = 0
        def fail_log(line):
            raise OSError('synthetic log error')
        with patch.object(core.subprocess, 'Popen', return_value=process):
            self.assertEqual(core.Broker._execute_process(['fixture'], {}, fail_log), 0)
        process.wait.assert_called_once()
        process.stdout.close.assert_called_once()
        self.assertEqual(process.stdout.readline.call_count, 3)

    def test_process_waits_even_if_pipe_read_fails(self):
        process = MagicMock()
        process.stdout.readline.side_effect = OSError('synthetic read error')
        with patch.object(core.subprocess, 'Popen', return_value=process):
            with self.assertRaises(OSError):
                core.Broker._execute_process(['fixture'], {}, lambda line: None)
        process.wait.assert_called_once()

    def test_nonready_environment_allows_discovery_not_execution(self):
        self.broker.idf = core.IdF(False, 'fixture unavailable')
        with self.assertRaisesRegex(ValueError, 'unavailable'):
            self.broker.create_plan(self.selection())
        self.assertFalse(self.executions)

    def test_discovery_is_bounded_and_excludes_nonapps_symlinks(self):
        (self.project / 'bootloader.bin').write_bytes(app_bytes())
        (self.project / 'merged.bin').write_bytes(app_bytes())
        (self.project / 'bad.bin').write_bytes(b'not an image')
        external = self.root / 'not-registered'
        external.mkdir()
        (external / 'secret-app.bin').write_bytes(app_bytes())
        try:
            (self.project / 'linked').symlink_to(external, target_is_directory=True)
        except OSError:
            pass
        with patch.object(core, 'registered_worktrees', return_value=([self.project], [])):
            candidates, runs, warnings = core.scan(self.project, threading.Event())
        names = {x['name']: x['kind'] for x in candidates}
        self.assertEqual(names['app.bin'], 'app-candidate')
        self.assertEqual(names['bootloader.bin'], 'excluded')
        self.assertEqual(names['merged.bin'], 'excluded')
        self.assertEqual(names['bad.bin'], 'excluded')
        self.assertNotIn('secret-app.bin', names)
        with patch.object(core, 'registered_worktrees', return_value=([self.project], [])), patch.object(core, 'MAX_SCAN_ENTRIES', 1):
            _, _, warnings = core.scan(self.project, threading.Event())
            self.assertTrue(warnings)

    def test_scan_cancel_and_plain_paths(self):
        cancel = threading.Event()
        cancel.set()
        with patch.object(core, 'registered_worktrees', return_value=([self.project], [])):
            candidates, _, warnings = core.scan(self.project, cancel)
        self.assertFalse(candidates)
        self.assertTrue(warnings)
        with self.assertRaises(ValueError):
            core.plain_path('relative/path')
        with self.assertRaises(ValueError):
            core.plain_path('//network/share')

    def test_candidate_size_limit(self):
        with self.recovery.open('wb') as stream:
            stream.truncate(core.MAX_APP + 1)
        self.assertEqual(core.app_metadata(self.recovery, self.project)['kind'], 'excluded')

    def test_host_origin_auth_csp_no_generic_api(self):
        client = TestClient(create_app(self.broker, 'fixture-token', 9999), base_url='http://127.0.0.1:9999')
        self.assertEqual(client.get('/api/state').status_code, 401)
        headers = {'X-Session-Token': 'fixture-token'}
        self.assertEqual(client.get('/api/state', headers=headers).status_code, 200)
        self.assertEqual(client.get('/api/state', headers={**headers, 'Host': 'evil.example'}).status_code, 403)
        self.assertEqual(client.get('/api/state', headers={**headers, 'Origin': 'https://evil.example'}).status_code, 403)
        self.assertEqual(client.get('/api/state', headers={**headers, 'Sec-Fetch-Site': 'cross-site'}).status_code, 403)
        self.assertEqual(client.post('/api/ports', headers=headers, json={}).status_code, 403)
        headers['Origin'] = 'http://127.0.0.1:9999'
        self.assertEqual(client.post('/api/ports', headers=headers, json={}).status_code, 202)
        self.assertEqual(client.post('/api/ports', headers=headers, json={'command': 'danger'}).status_code, 422)
        self.assertEqual(client.post('/api/shell', headers=headers, json={}).status_code, 404)
        self.assertEqual(client.post('/api/ports', headers=headers, json={'x': 'x' * 17000}).status_code, 413)
        self.assertEqual(client.post('/api/ports', headers=headers, content='{}').status_code, 403)
        response = client.get('/api/state', headers=headers)
        self.assertEqual(response.headers['cache-control'], 'no-store')
        self.assertIn("frame-ancestors 'none'", response.headers['content-security-policy'])
        self.assertNotIn('access-control-allow-origin', response.headers)
        self.assertEqual(client.get('/docs').status_code, 404)
        self.assertEqual(client.get('/static/styles.css').status_code, 200)
        self.assertEqual(client.get('/static/app.js').status_code, 200)
        self.assertEqual(client.get('/static/server.py').status_code, 404)

    def test_execute_api_validates_schema_and_acknowledgements(self):
        client = TestClient(create_app(self.broker, 'fixture-token', 9999), base_url='http://127.0.0.1:9999')
        headers = {'X-Session-Token': 'fixture-token', 'Origin': 'http://127.0.0.1:9999'}
        p = self.plan()
        body = {'plan_id': p['id'], 'plan_digest': p['digest'], 'acknowledgements': []}
        self.assertEqual(client.post('/api/execute', headers=headers, json=body).status_code, 409)
        body['acknowledgements'] = p['required_ack']
        response = client.post('/api/execute', headers=headers, json=body)
        self.assertEqual(response.status_code, 202)
        self.assertEqual(client.post('/api/execute', headers=headers, json=body).status_code, 409)
        self.assertEqual(wait(self.broker, response.json())['status'], 'succeeded')


class StaticContractTests(unittest.TestCase):
    def test_powershell_plan_pins_before_device_operations(self):
        flash = (core.TOOLS / 'Flash-Integrated.ps1').read_text()
        test = (core.TOOLS / 'Test-Integrated.ps1').read_text()
        self.assertLess(flash.index('Integrated evidence differs'), flash.index('$portLock = Enter-DeviceLock'))
        self.assertLess(flash.index('Recovery image differs'), flash.index('$portLock = Enter-DeviceLock'))
        self.assertLess(test.index('Integrated flash evidence differs'), test.index("Invoke-Logged $python @('-c'"))
        self.assertIn("'--before','no-reset','--after','no-reset','--no-stub'", flash)
        self.assertLess(flash.index('port-before-write.log'), flash.index("@('write-flash'"))
        self.assertLess(flash.index('port-before-verify.log'), flash.index("@('verify-flash'"))
        self.assertLess(test.index('port-before-test.log'), test.index('Invoke-Logged $python $arguments'))
        self.assertNotIn('shell=True', (Path(__file__).parent / 'server.py').read_text())

    def test_shared_port_identity_verifier(self):
        from verify_port_identity import verify
        verify(PORT['device'], core.port_signature(PORT), [PORT])
        for rows in ([], [dict(PORT, hwid='different')], [PORT, PORT]):
            with self.assertRaises(ValueError):
                verify(PORT['device'], core.port_signature(PORT), rows)

    def test_optional_idf_path_inherits_separate_activated_python(self):
        with patch.object(core.os, 'name', 'nt'), patch.object(core, 'plain_path', side_effect=lambda p, **kw: PosixPath(p)), \
             patch.object(core, 'run_readonly', side_effect=['ESP-IDF v6.0.1\n', 'esptool fixture\n']), \
             patch.object(core.shutil, 'which', return_value='/fixture/powershell.exe'), \
             patch.dict(os.environ, {'IDF_PATH': '/fixture/inherited-idf', 'IDF_PYTHON_ENV_PATH': '/fixture/idf-python'}):
            result = core.resolve_idf('/fixture/explicit-idf')
        self.assertTrue(result.ready, result.error)
        self.assertEqual(result.python, '/fixture/idf-python/Scripts/python.exe')
        self.assertEqual(result.env['IDF_PATH'], '/fixture/explicit-idf')


if __name__ == '__main__':
    unittest.main()
