"""Transport-independent, bounded local validation broker. Never imports serial I/O.

Only enumerate ports and run fixed, retained validation entry points. This module
is intentionally independent of HTTP so a future adapter can reuse the same
plans/acknowledgements instead of introducing another device owner.
"""
from __future__ import annotations

from collections import deque
import copy
from dataclasses import dataclass, field
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import secrets
import shutil
import stat
import subprocess
import sys
import threading
import time
from typing import Callable

TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))
from verify_port_identity import list_port_records, signature as port_signature
MAX_APP = 0x300000
MAX_JSON = 2 * 1024 * 1024
MAX_SCAN_ENTRIES = 30000
MAX_CANDIDATES = 300
MAX_WORKTREES = 64
SKIP = {'.git', '.venv', 'venv', 'node_modules', 'managed_components', '__pycache__', '.cache'}
SHA = re.compile(r'^[0-9a-f]{40}$')
PORT = re.compile(r'^COM[1-9][0-9]*$')


def utc(timestamp=None):
    return datetime.fromtimestamp(timestamp or time.time(), timezone.utc).isoformat()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def plain_path(value, *, file=False):
    path = Path(value)
    require(path.is_absolute(), 'Use an absolute local path.')
    require(not str(path).startswith(('\\\\', '//')), 'Network/UNC paths are not supported.')
    for part in (*reversed(path.parents), path):
        info = part.lstat()
        require(not stat.S_ISLNK(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
                'Symlinks/junctions are not accepted: ' + str(part))
    require(path.is_file() if file else path.is_dir(), 'Missing file/folder: ' + str(path))
    return path.resolve(strict=True)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def json_file(path):
    path = plain_path(path, file=True)
    require(path.stat().st_size <= MAX_JSON, 'JSON file is too large.')
    raw = path.read_bytes()
    encoding = 'utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig'
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'Duplicate JSON key: ' + key)
            result[key] = value
        return result
    return json.loads(raw.decode(encoding), object_pairs_hook=unique)


def clean_env():
    return {k: v for k, v in os.environ.items()
            if not k.upper().startswith('GIT_') and k.upper() not in ('PYTHONPATH', 'PYTHONHOME')}


def run_readonly(args, *, env=None, timeout=120):
    result = subprocess.run(args, env=env or clean_env(), stdin=subprocess.DEVNULL,
                            capture_output=True, text=True, encoding='utf-8', errors='replace',
                            timeout=timeout, check=False)
    require(result.returncode == 0, (result.stderr or result.stdout)[-6000:] or 'Command failed.')
    require(len(result.stdout) <= 4 * 1024 * 1024, 'Read-only command output exceeded limit.')
    return result.stdout


def registered_worktrees(project):
    env = clean_env()
    env['GIT_OPTIONAL_LOCKS'] = '0'
    output = run_readonly(['git', '-c', 'core.fsmonitor=false', '-C', str(project),
                           'worktree', 'list', '--porcelain', '-z'], env=env, timeout=15)
    paths = [project]
    warnings = []
    for record in output.split('\0'):
        if record.startswith('worktree '):
            try:
                path = plain_path(record[9:])
                if path not in paths:
                    paths.append(path)
            except (ValueError, OSError) as error:
                warnings.append(str(error))
    if len(paths) > MAX_WORKTREES:
        warnings.append('Worktree limit reached; extra roots were skipped.')
    return paths[:MAX_WORKTREES], warnings


def app_metadata(path, worktree):
    path = plain_path(path, file=True)
    info = path.stat()
    reason = None
    if path.suffix.lower() != '.bin':
        reason = 'Select an app .bin file.'
    elif not 0 < info.st_size <= MAX_APP:
        reason = 'Empty or larger than the factory app partition.'
    else:
        with path.open('rb') as stream:
            header = stream.read(36)
        # ESP image header (24), first segment header (8), esp_app_desc magic (4).
        # Header matching is only discovery, never a claim of boot compatibility.
        if len(header) < 36 or header[0] != 0xE9 or int.from_bytes(header[12:14], 'little') != 9:
            reason = 'Header is not an ESP32-S3 image.'
        elif header[32:36] != b'\x32\x54\xcd\xab':
            reason = 'Missing application descriptor; bootloader/raw images are excluded.'
    return {'id': hashlib.sha256(str(path).encode()).hexdigest()[:24], 'path': str(path),
            'name': path.name, 'bytes': info.st_size, 'modifiedUtc': utc(info.st_mtime),
            'worktree': str(worktree), 'kind': 'excluded' if reason else 'app-candidate',
            'reason': reason}


def run_metadata(root):
    root = plain_path(root)
    value = json_file(root / 'manifest.json')
    require(isinstance(value, dict) and value.get('schema') == 'pocketjs-integrated-build-v1',
            'Select a retained Build-Integrated v1 run folder containing manifest.json.')
    commit = value.get('commit', '')
    require(isinstance(commit, str) and SHA.fullmatch(commit.lower()), 'Manifest has no full commit SHA.')
    return {'id': hashlib.sha256(str(root).encode()).hexdigest()[:24], 'path': str(root),
            'commit': commit.lower(), 'status': value.get('status', 'unknown')}


def scan(project, cancel):
    roots, warnings = registered_worktrees(project)
    candidates, runs, visited = {}, {}, set()
    entries = 0
    started = time.monotonic()
    for root in roots:
        pending = [(root, 0)]
        # This exact backup subtree is the only exception to the .cache skip.
        # Keep its depth relative to the worktree root and share every scan limit.
        try:
            backup = plain_path(root / '.cache' / 'flash_backup')
            pending.append((backup, 2))
        except FileNotFoundError:
            pass
        except (OSError, ValueError) as error:
            warnings.append(str(error))
        while pending and not cancel.is_set():
            folder, depth = pending.pop()
            if folder in visited:
                continue
            visited.add(folder)
            try:
                plain_path(folder)
                with os.scandir(folder) as listing:
                    for item in listing:
                        entries += 1
                        if (cancel.is_set() or entries > MAX_SCAN_ENTRIES or
                                time.monotonic() - started > 30 or len(candidates) >= MAX_CANDIDATES):
                            warnings.append('Discovery stopped at its safety limit or was cancelled; use a smaller project/worktree set.')
                            return list(candidates.values()), list(runs.values()), warnings
                        info = item.stat(follow_symlinks=False)
                        if item.is_symlink() or getattr(info, 'st_file_attributes', 0) & 0x400:
                            continue
                        path = Path(item.path)
                        if item.is_dir(follow_symlinks=False):
                            if item.name.lower() not in SKIP:
                                if depth < 10:
                                    pending.append((path, depth + 1))
                                elif 'Depth limit reached; deeper folders were skipped.' not in warnings:
                                    warnings.append('Depth limit reached; deeper folders were skipped.')
                        elif item.is_file(follow_symlinks=False):
                            if path.suffix.lower() == '.bin':
                                candidate = app_metadata(path, root)
                                candidates[candidate['id']] = candidate
                            elif item.name == 'manifest.json':
                                try:
                                    run = run_metadata(path.parent)
                                    runs[run['id']] = run
                                except (ValueError, OSError, TypeError):
                                    pass
            except (OSError, ValueError) as error:
                warnings.append(str(error))
    if cancel.is_set():
        warnings.append('Discovery cancelled; partial results only.')
    return sorted(candidates.values(), key=lambda x: x['path']), list(runs.values()), warnings


def enumerate_ports():
    # Shared with the downstream locked boundary check; enumeration only.
    return list_port_records()


@dataclass(frozen=True)
class IdF:
    ready: bool
    error: str | None
    python: str | None = None
    idf_path: str | None = None
    powershell: str | None = None
    env: dict = field(default_factory=dict, repr=False)

    def public(self):
        return {k: getattr(self, k) for k in ('ready', 'error', 'python', 'idf_path')}


def resolve_idf(idf_path=None):
    try:
        require(os.name == 'nt', 'Device execution is Windows-only. Discovery remains available.')
        root = idf_path or os.environ.get('IDF_PATH')
        require(root, 'Activate the existing ESP-IDF v6.0.1 PowerShell profile, or supply --idf-path.')
        root = plain_path(root)
        idf = plain_path(root / 'tools/idf.py', file=True)
        environment = os.environ.get('IDF_PYTHON_ENV_PATH')
        require(environment, 'Activate the existing ESP-IDF v6.0.1 profile first: IDF_PYTHON_ENV_PATH is missing. The GUI Python is separate.')
        environment = plain_path(environment)
        python = plain_path(environment / 'Scripts/python.exe', file=True)
        env = clean_env()
        env.update(IDF_PATH=str(root), IDF_PYTHON_ENV_PATH=str(environment), PYTHONIOENCODING='utf-8')
        version = run_readonly([str(python), str(idf), '--version'], env=env, timeout=30)
        require(re.search(r'ESP-IDF v6\.0\.1(?:\s|$)', version), 'Expected ESP-IDF v6.0.1: ' + version.strip())
        run_readonly([str(python), '-m', 'esptool', 'version'], env=env, timeout=30)
        powershell = shutil.which('pwsh.exe') or shutil.which('powershell.exe')
        require(powershell, 'PowerShell 5.1 or 7 is required.')
        return IdF(True, None, str(python), str(root), str(plain_path(powershell, file=True)), env)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        return IdF(False, str(error), idf_path=str(idf_path) if idf_path else os.environ.get('IDF_PATH'))


class Broker:
    def __init__(self, project, private_root, *, idf_path=None, idf=None, ports=enumerate_ports,
                 readonly=run_readonly, execute=None):
        self.project = plain_path(project)
        self.private_root = plain_path(private_root)
        require(self.project != self.private_root and self.project not in self.private_root.parents,
                'GUI state must be outside the project.')
        self.idf = idf or resolve_idf(idf_path)
        self.ports_fn, self.readonly = ports, readonly
        self.execute_fn = execute or self._execute_process
        self.lock = threading.RLock()
        self.jobs, self.plans, self.candidates, self.runs = {}, {}, {}, {}
        self.imported_candidates = {}
        self.ports = []
        self.discovery = {'status': 'idle'}
        self.scan_cancel = threading.Event()
        self.tasks = queue.Queue(maxsize=4)
        self.device_reserved = False
        self.closed = False
        self.worker = threading.Thread(target=self._work, name='validation-broker', daemon=False)
        self.worker.start()

    def close(self):
        self.scan_cancel.set()
        with self.lock:
            self.closed = True
        self.tasks.put(None)
        self.worker.join()  # Never terminate an in-flight flash/test.

    def state(self):
        with self.lock:
            return {'project': str(self.project), 'idf': self.idf.public(),
                    'jobs': [{k: v for k, v in self._public_job(j).items() if k != 'log'}
                             for j in self.jobs.values()],
                    'ports': list(self.ports), 'candidates': list(self.candidates.values()),
                    'runs': list(self.runs.values()), 'discovery': dict(self.discovery)}

    @staticmethod
    def _public_job(job):
        return {k: list(v) if k == 'log' else copy.deepcopy(v)
                for k, v in job.items() if not k.startswith('_')}

    def get_job(self, job_id):
        with self.lock:
            require(job_id in self.jobs, 'Unknown job.')
            return self._public_job(self.jobs[job_id])

    def submit(self, kind, operation):
        with self.lock:
            require(not self.closed, 'Server is shutting down.')
            require(not self.tasks.full(), 'Worker queue is full; wait for current work.')
            if len(self.jobs) >= 40:
                old = next((key for key, j in self.jobs.items() if j['status'] in ('succeeded', 'failed')), None)
                require(old is not None, 'Too many pending jobs.')
                del self.jobs[old]
                try:
                    (self.private_root / (old + '.log')).unlink(missing_ok=True)
                except OSError:
                    pass
            job_id = secrets.token_hex(12)
            self.jobs[job_id] = {'id': job_id, 'kind': kind, 'status': 'queued',
                                 'log': deque(maxlen=300), 'result': None, 'error': None}
            self.tasks.put_nowait((job_id, operation))
            return {'job_id': job_id}

    def log(self, job_id, line):
        line = line.rstrip()[:2048]
        with self.lock:
            self.jobs[job_id]['log'].append(line)
        path = self.private_root / (job_id + '.log')
        try:
            if not path.exists() or path.stat().st_size < 1024 * 1024:
                with path.open('a', encoding='utf-8') as stream:
                    stream.write(line + '\n')
        except OSError:
            # Disk-full or permission changes must never release a live device child.
            with self.lock:
                self.jobs[job_id]['log'].append('Session disk logging unavailable; in-memory log remains bounded.')

    def _work(self):
        while True:
            item = self.tasks.get()
            if item is None:
                return
            job_id, operation = item
            with self.lock:
                self.jobs[job_id]['status'] = 'running'
            try:
                result = operation(job_id)
                with self.lock:
                    self.jobs[job_id].update(status='succeeded', result=result)
            except Exception as error:
                self.log(job_id, str(error))
                with self.lock:
                    self.jobs[job_id].update(status='failed', error=str(error))

    def start_scan(self):
        with self.lock:
            require(self.discovery['status'] != 'running', 'Discovery is already running.')
            self.discovery = {'status': 'running'}
            self.scan_cancel.clear()
            def work(job):
                try:
                    candidates, runs, warnings = scan(self.project, self.scan_cancel)
                    with self.lock:
                        refreshed = {}
                        for key, previous in self.imported_candidates.items():
                            try:
                                current = app_metadata(Path(previous['path']), previous['worktree'])
                            except (OSError, ValueError) as error:
                                current = dict(previous, kind='excluded',
                                    reason='Manually added file is missing or unavailable: ' + str(error))
                            current['source'] = 'manual'
                            refreshed[key] = current
                        self.imported_candidates = refreshed
                        self._merge_candidates(candidates, warnings)
                        self.runs.update({x['id']: x for x in runs})
                        self.discovery = {'status': 'cancelled' if self.scan_cancel.is_set() else 'complete',
                                          'warning': '\n'.join(warnings[:20])}
                    self.log(job, f'Found {len(candidates)} binary files and {len(runs)} integrated runs. No file was selected.')
                    return {'candidates': len(candidates), 'runs': len(runs)}
                except Exception:
                    with self.lock:
                        self.discovery = {'status': 'failed'}
                    raise
            try:
                return self.submit('scan', work)
            except Exception:
                self.discovery = {'status': 'idle'}
                raise

    def _merge_candidates(self, discovered, warnings):
        # Explicit imports survive rescans; total visible candidates stay bounded.
        manual = self.imported_candidates
        others = [x for x in discovered if x['id'] not in manual]
        room = MAX_CANDIDATES - len(manual)
        if len(others) > room:
            warnings.append('Candidate limit reached; manually added files were retained first.')
        self.candidates = {x['id']: x for x in others[:room]}
        self.candidates.update(manual)

    def import_app(self, path):
        def work(job):
            candidate = app_metadata(Path(path), '')
            require(candidate['kind'] == 'app-candidate', candidate['reason'] or 'Not an app candidate.')
            candidate['source'] = 'manual'
            with self.lock:
                require(candidate['id'] in self.imported_candidates or len(self.imported_candidates) < 64,
                        'At most 64 manually added app files are supported per session.')
                self.imported_candidates[candidate['id']] = candidate
                warnings = []
                self._merge_candidates(list(self.candidates.values()), warnings)
                if warnings:
                    self.discovery['warning'] = '\n'.join(warnings)
            self.log(job, 'Added an explicit app candidate; no file selected or device opened. Image-info runs in plan preflight.')
            return {'candidate': candidate}
        return self.submit('import-app', work)

    def import_run(self, path):
        def work(job):
            run = run_metadata(path)
            with self.lock:
                self.runs[run['id']] = run
            self.log(job, 'Registered retained run; full integrity verification happens in the plan.')
            return {'run': run}
        return self.submit('import-run', work)

    def refresh_ports(self):
        def work(job):
            ports = self.ports_fn()
            with self.lock:
                self.ports = ports
            self.log(job, 'Enumerated ports without opening or resetting them.')
            return {'ports': ports}
        return self.submit('ports', work)

    def _port(self, name):
        require(isinstance(name, str) and PORT.fullmatch(name), 'Select a Windows COM port.')
        matches = [x for x in self.ports_fn() if x['device'] == name]
        require(len(matches) == 1, 'Selected port is absent or ambiguous. Refresh the port list.')
        return matches[0]

    def _evidence(self, root, commit):
        # Run the existing strict verifier under the GUI's isolated Python; it uses only stdlib.
        output = self.readonly([sys.executable, str(TOOLS / 'verify_integrated_run.py'),
                                '--run-root', root, '--commit', commit])
        value = json.loads(output)
        require(value.get('status') == 'preflight-verified' and value.get('role') == 'integrated'
                and value.get('sourceSha') == commit and value.get('runRoot') == root,
                'Unexpected integrated evidence.')
        return value

    def _image_info(self, path):
        value = self.readonly([self.idf.python, '-m', 'esptool', 'image-info', str(path)], env=self.idf.env)
        require('ESP32-S3' in value, 'esptool did not confirm an ESP32-S3 image.')
        return value[-12000:]

    def _flash_record(self, evidence, port):
        flash = json_file(Path(evidence['runRoot']) / 'last-integrated-flash.json')
        require(isinstance(flash, dict), 'Invalid flash record.')
        expected = {'schema': 'pocketjs-integrated-flash-v1', 'status': 'flash-verified-awaiting-manual-reboot',
                    'role': 'integrated', 'port': port, **{k: evidence[k] for k in
                    ('sourceSha', 'binarySha256', 'manifestSha256', 'runRoot')}}
        require(all(flash.get(k) == v for k, v in expected.items()) and
                isinstance(flash.get('flashId'), str) and re.fullmatch(r'[0-9a-f]{32}', flash['flashId']),
                'No matching verified flash for this run/commit/image/port. Flash first, then manually reboot.')
        return flash['flashId']

    def create_plan(self, selection):
        with self.lock:
            require(self.idf.ready, self.idf.error or 'IDF environment is unavailable.')
            require(not self.device_reserved, 'A device job is pending/running. Wait for it to finish.')
            require(selection['run_id'] in self.runs, 'Select an integrated run.')
            run = dict(self.runs[selection['run_id']])
            require(selection['action'] in ('flash', 'test'), 'Unknown action.')
            candidate = None
            if selection['action'] == 'flash':
                require(selection['recovery_id'] in self.candidates, 'Select a discovered recovery app.')
                candidate = dict(self.candidates[selection['recovery_id']])
                require(candidate['kind'] == 'app-candidate', 'Selected recovery is not an app candidate.')
            require(selection['confidence'] in ('unverified-candidate', 'user-attested-known-good'), 'Invalid recovery confidence.')
            require(type(selection['cycles']) is int and 1 <= selection['cycles'] <= 20, 'Cycles must be 1–20.')
            require(type(selection['grid']) is bool, 'Grid must be boolean.')
        def work(job):
            port = self._port(selection['port'])
            evidence = self._evidence(run['path'], run['commit'])
            self.log(job, 'Original integrated manifest, six artifacts and H/F OFF evidence verified.')
            image = plain_path(evidence['binary'], file=True)
            require(app_metadata(image, run['path'])['kind'] == 'app-candidate', 'Integrated binary is not an ESP32-S3 app.')
            self.log(job, self._image_info(image))
            now = time.time()
            plan = {'id': secrets.token_hex(16), 'action': selection['action'],
                    'runRoot': run['path'], 'commit': run['commit'], 'port': selection['port'],
                    'portIdentity': port, 'portSignature': port_signature(port),
                    'binary': str(image), 'binarySha256': evidence['binarySha256'],
                    'manifestSha256': evidence['manifestSha256'], 'region': '0x10000', 'maxBytes': MAX_APP,
                    'recoveryImage': None, 'recoverySha256': None, 'recoveryConfidence': None,
                    'cycles': selection['cycles'], 'grid': selection['grid'], 'expiresUtc': utc(now + 600),
                    'expires': now + 600, 'warnings': []}
            if candidate:
                recovery = plain_path(candidate['path'], file=True)
                require(app_metadata(recovery, candidate['worktree'])['kind'] == 'app-candidate', 'Recovery image changed or is not an app.')
                require(recovery != image, 'Choose a separate recovery app, not the integrated target.')
                recovery_hash = digest(recovery)
                require(recovery_hash != plan['binarySha256'], 'Recovery and target have identical bytes. Select another recovery candidate.')
                self.log(job, self._image_info(recovery))
                plan.update(recoveryImage=str(recovery), recoverySha256=recovery_hash,
                            recoveryConfidence=selection['confidence'])
                plan['required_ack'] = ['replace-app', 'device-free', 'manual-download', 'compatible-layout',
                    'recovery-unverified' if selection['confidence'] == 'unverified-candidate' else 'recovery-known-good']
                plan['warnings'] = ['App replacement is destructive. No automatic reset or restore.',
                    'Recovery chip/hash checks do not prove it boots; unverified candidates have no verified rollback guarantee.']
            else:
                plan['flashId'] = self._flash_record(evidence, plan['port'])
                plan['required_ack'] = ['device-free', 'manual-reboot-home']
                plan['warnings'] = ['Manually reboot the just-verified image and wait for home. No automatic reboot or recovery.',
                                    'LCD, physical keys and audio remain manual checks.']
            plan['command'] = self.command(plan)
            plan['digest'] = hashlib.sha256(json.dumps(plan, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
            with self.lock:
                # Bounded in-memory plans; no replay after use or process restart.
                self.plans = {k: v for k, v in self.plans.items() if v['expires'] > now}
                while len(self.plans) >= 8:
                    del self.plans[next(iter(self.plans))]
                self.plans[plan['id']] = copy.deepcopy(plan)
            self.log(job, 'Review the immutable plan and acknowledgements. No device was opened.')
            return {'plan': plan}
        return self.submit('plan', work)

    def command(self, plan):
        action = plan['action']
        command = [self.idf.powershell, '-NoLogo', '-NoProfile', '-NonInteractive', '-File',
                   str(TOOLS / ('Flash-Integrated.ps1' if action == 'flash' else 'Test-Integrated.ps1')),
                   '-ExpectedPortSignature', plan['portSignature'],
                   '-RunRoot', plan['runRoot'], '-Commit', plan['commit'], '-Port', plan['port'],
                   '-ExpectedBinarySha256', plan['binarySha256'], '-ExpectedManifestSha256', plan['manifestSha256']]
        if action == 'flash':
            command += ['-RecoveryImage', plan['recoveryImage'], '-ExpectedRecoverySha256', plan['recoverySha256'],
                        '-AllowFlash', '-ConfirmCompatibleBootloaderAndPartitions', '-ConfirmDeviceFree', '-ConfirmDownloadMode']
            if plan['recoveryConfidence'] == 'unverified-candidate':
                command += ['-AllowUnverifiedRecovery']
        else:
            command += ['-ExpectedFlashId', plan['flashId'], '-AllowDevice', '-ConfirmDeviceFree',
                        '-ConfirmRunningImage', '-Cycles', str(plan['cycles'])]
            if plan['grid']:
                command += ['-Grid']
        return command

    def execute_plan(self, plan_id, plan_digest, acknowledgements):
        with self.lock:
            require(plan_id in self.plans, 'Plan is missing, consumed or expired. Prepare it again.')
            plan = self.plans[plan_id]
            require(secrets.compare_digest(plan['digest'], plan_digest), 'Plan digest mismatch.')
            require(plan['expires'] > time.time(), 'Plan expired. Prepare it again.')
            require(set(acknowledgements) == set(plan['required_ack']), 'All plan acknowledgements are required.')
            require(not self.device_reserved, 'A device job is already pending/running.')
            self.device_reserved = True
            def work(job):
                try:
                    require(plan['expires'] > time.time(), 'Plan expired while queued. Prepare it again.')
                    require(port_signature(self._port(plan['port'])) == plan['portSignature'], 'Port identity changed; prepare a new plan.')
                    current = self._evidence(plan['runRoot'], plan['commit'])
                    for key in ('binarySha256', 'manifestSha256'):
                        require(current[key] == plan[key], 'Integrated evidence changed; prepare a new plan.')
                    if plan['action'] == 'flash':
                        require(digest(plain_path(plan['recoveryImage'], file=True)) == plan['recoverySha256'],
                                'Recovery changed; prepare a new plan.')
                    else:
                        require(self._flash_record(current, plan['port']) == plan['flashId'], 'Flash record changed; prepare a new plan.')
                    self.log(job, 'Starting approved ' + plan['action'] + '. Do not disconnect or close the server.')
                    result = self.execute_fn(self.command(plan), self.idf.env, lambda line: self.log(job, line))
                    require(result == 0, f'Validation process failed (exit {result}). Inspect retained run logs; no automatic recovery was attempted.')
                    self.log(job, 'Device command finished. The port belongs to you again.')
                    return {'action': plan['action'], 'runRoot': plan['runRoot'], 'port': plan['port'],
                            'binarySha256': plan['binarySha256'], 'status': 'command-succeeded',
                            'next': 'Manually reboot and wait for home before preparing Test.' if plan['action'] == 'flash'
                            else 'Inspect physical LCD, keys and audio; markers are not full conformance.'}
                finally:
                    with self.lock:
                        self.device_reserved = False
            try:
                result = self.submit(plan['action'], work)
                del self.plans[plan_id]  # Single-use, even after failure; double-click cannot repeat a flash.
                return result
            except Exception:
                self.device_reserved = False
                raise

    @staticmethod
    def _execute_process(command, env, log):
        # Fixed argument array, never shell=True, command strings, arbitrary serial or reset.
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0
        process = subprocess.Popen(command, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, creationflags=flags)
        assert process.stdout is not None
        try:
            while True:
                block = process.stdout.readline(4096)
                if not block:
                    break
                try:
                    log(block.decode('utf-8', errors='replace'))
                except Exception:
                    # Continue draining even if the UI/session log destination fails.
                    pass
        finally:
            # Ownership outlives logging/read failures. Never kill a flash to cancel it.
            process.wait()
            process.stdout.close()
        return process.returncode
