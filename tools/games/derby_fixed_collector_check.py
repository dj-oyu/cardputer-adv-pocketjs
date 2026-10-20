"""Actual generated fixture -> pinned v2 collector, with memory-only transport.

The fixture host supplies APP_ID and post-cleanup STOP/HOME as a shell model.
READY, three summaries and END are actual generated JS/native C output. This
checks their protocol contract; it does not verify USB or device HOME state.
"""
from pathlib import Path
import hashlib
import importlib.util
import re
import sys

UPSTREAM = 'dj-oyu/esp-device-broker-mcp'
COMMIT = '88890ed79b9a7e48d99ffe5982cd03862c5eabec'
SOURCE = 'src/cardputer_mcp/derby_fixed.py'
SHA256 = 'c23d435f123b65e5a65473d526803cc0d77208a8cd43ff43ee11fb32fda6e6ab'
FAULT_SHA256 = 'da8d2c2cc8ac222dad5da8ed5c6ffc6560d4458e4b051da26903a10d91366402'


def load_collector():
    directory = Path(__file__).parent/'fixtures/broker_v2'
    path = directory/'derby_fixed.py'
    if (hashlib.sha256(path.read_bytes()).hexdigest() != SHA256 or
        hashlib.sha256((directory/'device_faults.py').read_bytes()).hexdigest() != FAULT_SHA256):
        raise ValueError('pinned v2 collector changed')
    import types
    package = 'derby_fixed_broker_v2'
    if package not in sys.modules:
        module = types.ModuleType(package)
        module.__path__ = [str(directory)]
        sys.modules[package] = module
    spec = importlib.util.spec_from_file_location(package+'.derby_fixed', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class Rejected(Exception):
    pass


class MemoryReader:
    """No serial imports or port access. Model framing and fixed navigation.

    Every complete wire line reaches phase_observer in order, including lines
    coalesced after READY/HOME, as in broker serial_backend.Reader.read_once.
    """
    def __init__(self, raw, chunk):
        self.parts = iter(raw[at:at+chunk] for at in range(0, len(raw), chunk))
        self.now = 0.0
        self.deadline = 200.0
        self.pending = b''
        self.lines = []
        self.row = 0
        self.launch_timeouts = []
        self.wire_observer = self.phase_observer = None

    def clock(self):
        return self.now

    def failure(self, code):
        return Rejected(code)

    def read_once(self):
        data = next(self.parts, b'')
        self.now += 0.0001 if data else 1
        if data:
            self.wire_observer(data)
            lines = (self.pending+data).split(b'\n')
            self.pending = lines.pop()
            for raw in lines:
                line = raw.rstrip(b'\r')
                self.phase_observer(line)
                self.lines.append(line)
        return bool(data)

    def command(self, key, marker, *, timeout=12, step='command'):
        if key == b'\x1b': return [b'HOME_READY']
        if key == b'a': return [b'CATEGORY 0']
        if key in (b'u', b'd'):
            self.row = max(0, self.row-1) if key == b'u' else self.row+1
            return [b'APP '+str(self.row).encode()]
        if key != b'e': raise Rejected('unexpected test command')
        self.launch_timeouts.append(timeout)
        stop = min(self.now+timeout, self.deadline)
        examined = 0
        while self.now < stop:
            self.read_once()
            while examined < len(self.lines):
                line = self.lines[examined]
                examined += 1
                if re.fullmatch(marker, line): return self.lines[:examined]
        raise Rejected('serial_marker_timeout')

    def capture_quiet_boundary(self):
        while self.read_once():
            if self.now >= self.deadline: raise Rejected('serial_deadline')


def check_collector(raw, *, detail_enabled=False):
    from derby_fixed_completion import parse_completion
    parse_completion(raw,detail_enabled=detail_enabled)
    module = load_collector()
    assert (module.MAX_SECONDS, module.MAX_BYTES, module.MAX_LINE) == (200, 65536, 1024)
    lines = raw.splitlines()
    protocol = [line for line in lines if line.startswith(
        (b'APP_ID', b'DERBY READY', b'DERBY_FIXED', b'APP_STOPPED', b'HOME_READY'))]
    assert len(protocol) == 8, protocol
    assert protocol[0] == b'APP_ID local.derby'
    assert re.fullmatch(rb'DERBY READY tones=(?:true|false)', protocol[1])
    assert all(line.startswith(b'DERBY_FIXED v=') for line in protocol[2:5])
    assert protocol[5].startswith(b'DERBY_FIXED_END ')
    assert protocol[6:] == [b'APP_STOPPED', b'HOME_READY']
    # The host checks READY before dispatching the first fixture frame. Thus
    # this assertion covers initialization, and the three replays cannot hide a
    # second startup marker merely by placing it before the first summary.
    ready = protocol[1]+b'\n'
    assert raw.count(ready) == 1
    positives = 0
    for ending in (b'\n', b'\r\n'):
        wire = raw.replace(b'\n', ending)
        for chunk in (1, 17, 4096):
            reader = MemoryReader(wire, chunk)
            trial = module.execute_fixed(reader)
            parse_completion(trial.raw,detail_enabled=detail_enabled)
            assert trial.valid and trial.parsed['work_verified']
            assert trial.raw == wire
            assert trial.receipt['sha256'] == hashlib.sha256(wire).hexdigest()
            assert reader.launch_timeouts == [20]
            positives += 1
    owner=next(line for line in lines if line.startswith(b'DERBY_DETAIL_EXIT '))
    cases = {'missing-owner':raw.replace(owner+b'\n',b''),
             'duplicate-owner':raw.replace(owner,owner+b'\n'+owner),
             'wrong-owner-session':raw.replace(owner,owner.replace(b'session=',b'session=9')),
             'fatal-after-END':raw.replace(b'APP_STOPPED',b'InternalError: interrupted\nAPP_STOPPED'),
             'fatal-after-HOME':raw+b'InternalError: interrupted\n'}
    for name, line in [('identity',protocol[0]), ('ready',protocol[1]),
                       ('summary',protocol[3]), ('end',protocol[5]),
                       ('stop',protocol[6]), ('home',protocol[7])]:
        cases['missing-'+name] = raw.replace(line+b'\n', b'', 1)
    cases['duplicate-ready'] = raw.replace(ready, ready+ready, 1)
    cases['post-home-ready'] = raw+ready
    cases['invalid-end'] = raw.replace(protocol[5], protocol[5].replace(b'valid=1', b'valid=0'))
    cases['bad-end-session'] = raw.replace(protocol[5], protocol[5].replace(b'session=', b'session=99'))
    cases['bad-quality'] = raw.replace(b'quality=0', b'quality=1', 1)
    aborted = b'\n'.join(protocol[:2]+protocol[6:]+[
        re.sub(rb'reps=3 valid=1 quality=0',b'reps=0 valid=0 quality=2',protocol[5])])+b'\n'
    cases['tail-abort-stop-home-before-end'] = aborted
    cases['partial-tail'] = raw+b'partial'
    cases['line-limit'] = raw+b'x'*1024+b'\n'
    cases['capacity'] = raw+b'x'*65536
    negatives = 0
    for name, damaged in cases.items():
        for chunk in (17,4096):
            try:
                trial = module.execute_fixed(MemoryReader(damaged, chunk))
                parse_completion(trial.raw,detail_enabled=detail_enabled)
            except (Rejected, ValueError):
                pass
            else:
                raise AssertionError('collector accepted '+name+': '+str(trial.valid))
            negatives += 1
    # Keep the full capture deadline and collector-owned quality facts strict.
    collector = module.FixedCollector(0)
    collector.feed(raw)
    for line in lines: collector.observe_line(line)
    assert not collector.snapshot(200.001, complete=True).valid
    assert not collector.snapshot(1, complete=True, disconnect=True).valid
    return {'repository':UPSTREAM, 'commit':COMMIT, 'path':SOURCE, 'sha256':SHA256,
            'positive_cases':positives, 'negative_cases':negatives+2,
            'raw_sha256':hashlib.sha256(raw).hexdigest(),
            'owner_contract':'fixed-owner-v1','detail_mode':'ON' if detail_enabled else 'OFF',
            'scope':'pinned collector plus shared owner/fatal gate; memory-only shell/transport; no hardware acceptance'}
