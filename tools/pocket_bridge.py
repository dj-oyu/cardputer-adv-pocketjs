"""PC side of pocket.bridge -- docs/common-api.md section 13, USB transport.

    python tools/pocket_bridge.py selftest                 # no board, no port
    python tools/pocket_bridge.py serve --port COM3        # answer a running app
    python tools/pocket_bridge.py demo  --port COM3        # type the app in, then serve

The wire format is main/pocket_bridge.h. A frame is

    0x1d 'B' <hex of body and CRC32> '\\n'

and this reader scans the RAW byte stream for 0x1d rather than reading lines,
because a frame and an ESP_LOGI line share one USB stream: the firmware writes a
frame in a single driver call so a log line cannot be spliced into the middle of
one, but a log LINE can perfectly well be cut in half by a frame arriving. Log
text is therefore reassembled here around the frames rather than trusted to
readline().

This adapter answers host.* and agent.* itself. It deliberately does NOT run a
model, a shell command or anything else the device asks for by name: section 13
says the PC's shell and its API keys are not to be reachable from the device,
and a dispatch table with no execution in it is the only way to mean that.

Python stdlib except pyserial. Run it in the ESP-IDF Python environment.
"""
import argparse
import binascii
import json
import platform
import struct
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEMO = ROOT / 'apps/bridge/bridge.js'

START = 0x1d
TYPE = ord('B')
VERSION = 1
HEADER = 12
MAX_FRAME = 496                       # header + payload + CRC; see pocket_bridge.h
MAX_PAYLOAD = MAX_FRAME - HEADER - 4  # 480

HELLO, WELCOME, REQUEST, RESPONSE, ERROR, EVENT, BYE = 1, 2, 3, 4, 5, 6, 7
KIND_NAME = {HELLO: 'HELLO', WELCOME: 'WELCOME', REQUEST: 'REQUEST',
             RESPONSE: 'RESPONSE', ERROR: 'ERROR', EVENT: 'EVENT', BYE: 'BYE'}


def encode(kind, session, request, payload=b''):
    """One wire frame. `payload` is already the bytes the kind defines."""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f'payload {len(payload)} over maxPayloadBytes {MAX_PAYLOAD}')
    body = struct.pack('<BBHII', VERSION, kind, len(payload), session, request) + payload
    body += struct.pack('<I', zlib.crc32(body))
    return bytes([START, TYPE]) + binascii.hexlify(body) + b'\n'


class Reader:
    """A byte-stream splitter with the same rules the firmware's parser uses.

    feed() returns a list of ('frame', (kind, session, request, payload)) and
    ('log', text) items, in the order they were seen. A frame that fails any
    check is reported as ('bad', reason) rather than dropped in silence -- the
    device counts its own losses in the log, and this side should be able to say
    the same thing.
    """

    def __init__(self):
        self.state = 'text'
        self.hex = bytearray()
        self.text = bytearray()

    def _flush_text(self, out):
        while b'\n' in self.text:
            line, _, rest = self.text.partition(b'\n')
            self.text = bytearray(rest)
            line = line.rstrip(b'\r').decode('utf-8', 'replace')
            if line:
                out.append(('log', line))

    def _finish(self, out):
        raw = bytes(self.hex)
        self.hex = bytearray()
        self.state = 'text'
        if len(raw) % 2:
            return out.append(('bad', 'odd hex run'))
        try:
            body = binascii.unhexlify(raw)
        except binascii.Error:
            return out.append(('bad', 'not hex'))
        if len(body) < HEADER + 4:
            return out.append(('bad', f'{len(body)} bytes is shorter than a header'))
        if body[0] != VERSION:
            return out.append(('bad', f'version {body[0]}'))
        if zlib.crc32(body[:-4]) != struct.unpack('<I', body[-4:])[0]:
            return out.append(('bad', 'CRC mismatch'))
        _, kind, length, session, request = struct.unpack('<BBHII', body[:HEADER])
        if HEADER + length + 4 != len(body):
            return out.append(('bad', f'length {length} does not match {len(body)}'))
        out.append(('frame', (kind, session, request, body[HEADER:-4])))

    def feed(self, data):
        out = []
        for c in data:
            if self.state == 'text':
                if c == START:
                    self._flush_text(out)
                    self.state = 'type'
                else:
                    self.text.append(c)
                continue
            if self.state == 'type':
                if c == START:
                    continue
                self.state = 'hex' if c == TYPE else 'skip'
                self.hex = bytearray()
                continue
            if self.state == 'hex':
                if c == 0x0a:
                    self._finish(out)
                elif c == START:
                    out.append(('bad', 'restarted mid-frame'))
                    self.state = 'type'
                elif chr(c) in '0123456789abcdef':
                    self.hex.append(c)
                else:
                    out.append(('bad', 'non-hex byte in a frame'))
                    self.state = 'skip'
                continue
            # skip: a frame we could not use, or a 0x1d that was not ours
            if c == START:
                self.state = 'type'
            elif c == 0x0a:
                self.state = 'text'
        self._flush_text(out)
        return out


def split(payload, limit):
    """The "name '\\0' rest" shape the device uses for method, topic and code."""
    at = payload.find(b'\0', 0, limit)
    if at < 0:
        return None, b''
    return payload[:at].decode('ascii', 'replace'), payload[at + 1:]


# ---------------------------------------------------------------- the adapter


class Adapter:
    """host.* and agent.* over one session. No model, no shell, no filesystem."""

    def __init__(self, peer):
        self.peer = peer
        self.session = 0
        self.sequence = 0
        self.jobs = {}
        self.serial = 0
        self.pending = []          # (due, topic, payload) events not yet sent

    # -- requests ----------------------------------------------------------
    def hello(self, payload):
        name, _ = split(payload, 32)
        if self.peer and name != self.peer:
            return ERROR, b'NOT_FOUND\0this adapter is ' + self.peer.encode()
        return WELCOME, (name or '').encode()

    def request(self, method, argument):
        handler = getattr(self, 'do_' + method.replace('.', '_'), None)
        if handler is None:
            return ERROR, b'UNSUPPORTED\0' + f'this adapter has no {method}'.encode()
        try:
            return RESPONSE, json.dumps(handler(argument), separators=(',', ':')).encode()
        except KeyError as e:
            return ERROR, b'NOT_FOUND\0' + str(e).strip("'").encode()
        except ValueError as e:
            return ERROR, b'INVALID_ARGUMENT\0' + str(e).encode()

    def do_host_info(self, _):
        return {'host': platform.node()[:40], 'system': platform.system(),
                'adapter': 'pocket_bridge.py'}

    def do_host_time(self, _):
        return {'unixMs': int(time.time() * 1000),
                'iso': time.strftime('%Y-%m-%dT%H:%M:%S')}

    def do_agent_submit(self, argument):
        prompt = (argument or {}).get('prompt')
        if not isinstance(prompt, str) or not prompt:
            raise ValueError('prompt must be a non-empty string')
        self.serial += 1
        job = f'job-{self.serial}'
        # Accepted, then finished a moment later, so the event path is exercised
        # with something honest: the adapter is not generating anything, and the
        # result says so rather than pretending.
        self.jobs[job] = {'state': 'accepted', 'result': None}
        now = time.monotonic()
        self.pending.append((now + 0.4, job, 'running'))
        self.pending.append((now + 1.2, job, 'done'))
        return {'jobId': job, 'state': 'accepted'}

    def do_agent_status(self, argument):
        job = (argument or {}).get('jobId')
        if job not in self.jobs:
            raise KeyError(str(job))
        return dict(self.jobs[job], jobId=job)

    def do_agent_cancel(self, argument):
        job = (argument or {}).get('jobId')
        if job not in self.jobs:
            raise KeyError(str(job))
        self.pending = [p for p in self.pending if p[1] != job]
        self.jobs[job]['state'] = 'cancelled'
        return {'jobId': job, 'state': 'cancelled'}

    # -- events ------------------------------------------------------------
    def due(self):
        """The event frames whose time has come, oldest first."""
        now = time.monotonic()
        ready = [p for p in self.pending if p[0] <= now]
        self.pending = [p for p in self.pending if p[0] > now]
        for _, job, state in ready:
            self.jobs[job]['state'] = state
            if state == 'done':
                self.jobs[job]['result'] = 'this adapter does not run a model'
            self.sequence += 1
            body = json.dumps({'jobId': job, 'state': state,
                               'result': self.jobs[job]['result']},
                              separators=(',', ':')).encode()
            yield self.sequence, b'agent.job\0' + body


# ------------------------------------------------------------------- serving


def serve(port, adapter, quiet=False, until=None):
    reader = Reader()
    while until is None or time.monotonic() < until:
        for kind, item in reader.feed(port.read(256) or b''):
            if kind == 'log':
                if not quiet:
                    print(item, flush=True)
                continue
            if kind == 'bad':
                print(f'[bridge] dropped a frame: {item}', file=sys.stderr, flush=True)
                continue
            k, session, request, payload = item
            if k == HELLO:
                adapter.session = session
                reply, body = adapter.hello(payload)
                port.write(encode(reply, session, request, body))
                print(f'[bridge] session {session} '
                      f'{"open" if reply == WELCOME else "refused"}', flush=True)
                continue
            if session != adapter.session:
                continue                       # a frame from a run that ended
            if k == BYE:
                print(f'[bridge] session {session} closed by the device', flush=True)
                adapter.session = 0
                adapter.pending.clear()
                continue
            if k != REQUEST:
                continue
            method, argument = split(payload, 33)
            try:
                value = json.loads(argument or b'null')
            except ValueError:
                port.write(encode(ERROR, session, request,
                                  b'CORRUPT_DATA\0the payload was not JSON'))
                continue
            reply, body = adapter.request(method or '', value)
            print(f'[bridge] {method} -> {KIND_NAME[reply]}', flush=True)
            port.write(encode(reply, session, request, body))
        if adapter.session:
            for sequence, body in adapter.due():
                port.write(encode(EVENT, adapter.session, sequence, body))
        port.flush()


# ------------------------------------------------------------------ the demo
#
# The only way onto the device with nothing but the cable: type the source into
# the Playground and press C-r. main.c's input task reads ONE byte per 5 ms poll
# and the UI task takes ONE keystroke per 16 ms frame, so the typing has to be
# paced to the slower of those or characters are lost with no complaint. 50
# bytes a second is comfortably under both.

TYPE_RATE = 50.0


def expect(port, marker, seconds, echo=True):
    end = time.monotonic() + seconds
    seen = []
    while time.monotonic() < end:
        line = port.readline().decode('utf-8', 'replace').strip()
        if line:
            seen.append(line)
            if echo:
                print(line, flush=True)
        if marker in line:
            return line
    raise RuntimeError(f'waited {seconds}s for {marker}; last saw {seen[-5:]}')


def type_source(port, source):
    sent = 0
    for i in range(0, len(source), 8):
        port.write(source[i:i + 8])
        port.flush()
        sent += len(source[i:i + 8])
        time.sleep(len(source[i:i + 8]) / TYPE_RATE)
        if sent % 200 < 8:
            print(f'[bridge] typed {sent}/{len(source)} bytes', flush=True)


def demo(port, adapter, source, skip_typing):
    # b then a: both always log a CATEGORY line, changed or not, so this is a
    # sync point rather than a guess about where the menu already was.
    port.write(b'q')
    time.sleep(0.5)
    port.reset_input_buffer()
    port.write(b'b')
    expect(port, 'CATEGORY 1', 5)
    port.write(b'a')
    expect(port, 'CATEGORY 0', 5)
    # APP is logged on every press, so five ups reach 0 from anywhere.
    for _ in range(5):
        port.write(b'u')
        time.sleep(0.1)
    expect(port, 'APP 0', 5)
    port.write(b'd')
    time.sleep(0.1)
    port.write(b'd')
    expect(port, 'APP 2', 5)               # PLAYGROUND, main/shell.c apps[]
    port.write(b'e')
    expect(port, 'CODE_READY', 5)
    if not skip_typing:
        port.write(b'\x0e')                # C-n: empty the document
        time.sleep(0.3)
        type_source(port, source)
        time.sleep(0.5)
    port.write(b'\x12')                    # C-r: save and run
    print('[bridge] running; serving until Ctrl-C', flush=True)
    serve(port, adapter)


# ---------------------------------------------------------------- self test


def selftest():
    checks = 0

    def ok(condition, what):
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError(what)

    reader = Reader()
    wire = encode(REQUEST, 0x11223344, 0x55667788, b'host.info\0null')
    ok(len(wire) == 2 + 2 * (HEADER + 14 + 4) + 1, 'wire length')
    items = reader.feed(wire)
    ok(items == [('frame', (REQUEST, 0x11223344, 0x55667788, b'host.info\0null'))],
       f'round trip: {items}')

    # A log line cut in half by a frame: the two halves must join back up and
    # the frame must come out whole. This is the case the format exists for.
    reader = Reader()
    items = reader.feed(b'I (123) app: HELLO' + wire + b'_FRAME\n')
    ok(('frame', (REQUEST, 0x11223344, 0x55667788, b'host.info\0null')) in items,
       'frame inside a log line')
    ok(('log', 'I (123) app: HELLO_FRAME') in items, f'log rejoined: {items}')

    # A corrupted frame is reported, and the reader is usable straight after.
    broken = bytearray(wire)
    broken[10] = ord('0') if broken[10] != ord('0') else ord('1')
    reader = Reader()
    items = reader.feed(bytes(broken) + wire)
    ok([k for k, _ in items] == ['bad', 'frame'], f'corrupt then good: {items}')

    reader = Reader()
    ok([k for k, _ in reader.feed(b'\x1dP' + b'00' * 48 + b'\n' + wire)] == ['frame'],
       "pet_hub's own 0x1e frames are not the only thing skipped")

    ok(split(b'agent.job\0{"a":1}', 24) == ('agent.job', b'{"a":1}'), 'split')
    ok(split(b'nonul', 24) == (None, b''), 'split without a NUL')

    a = Adapter('demo')
    ok(a.hello(b'demo\x000.1.0')[0] == WELCOME, 'hello accepts its peer')
    ok(a.hello(b'other\x000.1.0')[0] == ERROR, 'hello refuses another peer')
    a.session = 7
    kind, body = a.request('agent.submit', {'prompt': 'hi'})
    ok(kind == RESPONSE and json.loads(body)['jobId'] == 'job-1', 'submit')
    ok(a.request('agent.nope', None)[1].startswith(b'UNSUPPORTED\0'), 'unknown method')
    ok(a.request('agent.status', {'jobId': 'no'})[1].startswith(b'NOT_FOUND\0'), 'no job')
    time.sleep(1.3)
    events = list(a.due())
    ok([json.loads(split(b, 24)[1])['state'] for _, b in events] == ['running', 'done'],
       f'events: {events}')

    try:
        encode(REQUEST, 1, 1, b'x' * (MAX_PAYLOAD + 1))
        ok(False, 'oversized payload should be refused')
    except ValueError:
        checks += 1
    print(f'pocket_bridge selftest: {checks} checks passed')
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='command', required=True)
    sub.add_parser('selftest', help='codec and adapter checks; no board needed')
    for name, help_text in (('serve', 'answer an app that is already running'),
                            ('demo', 'type apps/bridge/bridge.js in, run it, then serve')):
        c = sub.add_parser(name, help=help_text)
        c.add_argument('--port', required=True)
        c.add_argument('--peer', default='pocket-bridge-demo',
                       help='the peerId this adapter answers to')
        if name == 'demo':
            c.add_argument('--source', type=Path, default=DEMO)
            c.add_argument('--no-type', action='store_true',
                           help='run what the Playground already holds')
    a = p.parse_args()
    if a.command == 'selftest':
        return selftest()

    import serial
    adapter = Adapter(a.peer)
    with serial.Serial(a.port, 115200, timeout=0.05, write_timeout=5) as port:
        if a.command == 'serve':
            print('[bridge] serving; Ctrl-C to stop', flush=True)
            serve(port, adapter)
        else:
            source = a.source.read_bytes().replace(b'\r\n', b'\n')
            print(f'[bridge] {a.source} is {len(source)} bytes, '
                  f'about {len(source) / TYPE_RATE:.0f}s to type', flush=True)
            demo(port, adapter, source, a.no_type)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
