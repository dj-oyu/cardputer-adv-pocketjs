"""Read-only Codex/Claude usage adapters and acknowledged USB telemetry.

No prompts, answers, API keys or credentials enter snapshots or USB frames.
Use --help and docs/pet-companion.md. Python stdlib except pyserial for send.
"""
import argparse
from contextlib import closing
import hashlib
import json
import math
import os
from pathlib import Path
import queue
import shutil
import sqlite3
import struct
import subprocess
import sys
import threading
import time
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
DEFAULT = ROOT / '.cache/pet-companion'
MAX_TOKENS = 2**53-1


def numeric(value, limit):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and 0 <= value <= limit


def window(data, used, reset):
    if not isinstance(data, dict) or not numeric(data.get(used), 100) or not numeric(data.get(reset), 2**32-1):
        return None
    return {'usedPercent': data[used], 'resetsAt': int(data[reset])}


def normalize_codex(limits, usage, bucket='codex'):
    buckets = limits.get('rateLimitsByLimitId')
    # Never silently substitute another model family's quota bucket.
    item = buckets.get(bucket) if isinstance(buckets, dict) else limits.get('rateLimits')
    if not isinstance(item, dict) or item.get('limitId', bucket) != bucket:
        item = {}
    total = (usage.get('summary') or {}).get('lifetimeTokens')
    return {'provider': 0, 'tokens': int(total) if numeric(total, MAX_TOKENS) else None,
            'windows': [window(item.get(k), 'usedPercent', 'resetsAt') for k in ('primary', 'secondary')]}


def normalize_claude(status, tokens):
    rates = status.get('rate_limits') or {}
    if not isinstance(rates, dict):
        rates = {}
    return {'provider': 1, 'tokens': tokens,
            'windows': [window(rates.get(k), 'used_percentage', 'resets_at') for k in ('five_hour', 'seven_day')]}


def database(directory):
    directory.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(directory / 'metrics.sqlite3', timeout=1)
    db.execute('create table if not exists meta (key text primary key, value text)')
    db.execute('create table if not exists messages (id text primary key, tokens integer)')
    db.execute('create table if not exists cursors (id text primary key, inode text, offset integer)')
    db.execute('create table if not exists snapshots (provider integer primary key, value text)')
    return db


def atomic_json(path, value):
    tmp = path.with_name(path.name+'.'+uuid.uuid4().hex+'.tmp')
    try:
        tmp.write_text(json.dumps(value, separators=(',', ':')), encoding='utf-8')
        os.replace(tmp, path)
    finally:
        if tmp.exists():
            tmp.unlink()


def publish(directory, sample, db=None, identity='default'):
    if db is None:
        with closing(database(directory)) as owned:
            owned.execute('begin immediate')
            return publish(directory,sample,owned,identity)
    key = f'stream:{sample["provider"]}:{identity}'
    row = db.execute('select value from meta where key=?', (key,)).fetchone()
    stream = int(row[0]) if row else (uuid.uuid4().int & 0xffffffff) or 1
    db.execute('insert or replace into meta values (?,?)', (key, str(stream)))
    key = f'seq:{sample["provider"]}'
    row = db.execute('select value from meta where key=?', (key,)).fetchone()
    seq = int(row[0])+1 if row else 1
    if seq >= 2**32:
        raise ValueError('sequence exhausted; use a new state directory')
    db.execute('insert or replace into meta values (?,?)', (key, str(seq)))
    now = int(time.time())
    from datetime import datetime
    offset = int(datetime.now().astimezone().utcoffset().total_seconds())
    sample = dict(sample, version=1, stream=stream, sequence=seq, observed=now, utcOffset=offset)
    # Commit sequence before making the immutable snapshot visible. Gaps are OK.
    db.execute('insert or replace into snapshots values (?,?)', (sample['provider'], json.dumps(sample)))
    db.commit()
    atomic_json(directory / ('codex.json' if sample['provider'] == 0 else 'claude.json'), sample)
    return sample


def transcript_tokens(db, transcript, session):
    """Compatibility reader: unique assistant message IDs, never context size.

    Store numeric metadata only. Bound work per invocation and retry partial
    final lines on the next call. Repeated streaming records take a high-water
    value instead of counting the same response multiple times.
    """
    path = Path(transcript)
    if path.suffix != '.jsonl' or not path.is_file():
        return None
    key = hashlib.sha256(str(path.resolve()).encode()).hexdigest()
    stat = path.stat()
    inode = str(stat.st_ino)
    row = db.execute('select inode,offset from cursors where id=?', (key,)).fetchone()
    offset = row[1] if row and row[0] == inode and stat.st_size >= row[1] else 0
    processed = 0
    with path.open('rb') as f:
        f.seek(offset)
        while processed < 4*1024*1024:
            start = f.tell()
            line = f.readline(2*1024*1024+1)
            if not line or not line.endswith(b'\n'):
                break
            processed += len(line)
            offset = f.tell()
            try:
                record = json.loads(line)
            except (ValueError, UnicodeDecodeError):
                continue
            if not isinstance(record, dict) or record.get('type') != 'assistant':
                continue
            msg = record.get('message') or {}
            if not isinstance(msg, dict):
                continue
            usage = msg.get('usage') or {}
            mid = msg.get('id')
            if not isinstance(mid, str) or not isinstance(usage, dict):
                continue
            fields = ('input_tokens', 'output_tokens', 'cache_creation_input_tokens', 'cache_read_input_tokens')
            if not any(k in usage for k in fields) or not all(numeric(usage.get(k, 0), MAX_TOKENS) for k in fields):
                continue
            total = sum(int(usage.get(k, 0)) for k in fields)
            digest = hashlib.sha256((str(session)+':'+mid).encode()).hexdigest()
            db.execute('insert into messages values (?,?) on conflict(id) do update set tokens=max(tokens,excluded.tokens)', (digest, total))
    db.execute('insert or replace into cursors values (?,?,?)', (key, inode, offset))
    return db.execute('select sum(tokens) from messages').fetchone()[0]


def claude_statusline(directory, status):
    with closing(database(directory)) as db:
        db.execute('begin immediate')
        transcript = status.get('transcript_path')
        tokens = transcript_tokens(db, transcript, status.get('session_id', '')) if isinstance(transcript, str) else None
        sample = normalize_claude(status, tokens)
        publish(directory, sample, db)
    value = sample['windows'][0]
    print('Pet linked | 5h '+(str(round(value['usedPercent']))+'%' if value else '--'))


class CodexReader:
    def __init__(self, executable):
        self.process = subprocess.Popen([executable, 'app-server', '--stdio'], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, encoding='utf-8',
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        self.lines = queue.Queue()
        self.serial = 0
        def read():
            for line in self.process.stdout:
                try:
                    self.lines.put(json.loads(line))
                except ValueError:
                    pass
            self.lines.put(None)
        threading.Thread(target=read, daemon=True).start()
        try:
            self.request('initialize', {'clientInfo': {'name': 'pocket_pet', 'version': '0.1.0'},
                                       'capabilities': {'experimentalApi': True}})
            self.process.stdin.write('{"method":"initialized"}\n'); self.process.stdin.flush()
        except Exception:
            self.close()
            raise

    def request(self, method, params=None):
        self.serial += 1
        msg = {'id': self.serial, 'method': method}
        if params is not None:
            msg['params'] = params
        self.process.stdin.write(json.dumps(msg)+'\n'); self.process.stdin.flush()
        end = time.monotonic()+20
        while time.monotonic() < end:
            try:
                response = self.lines.get(timeout=max(.01, end-time.monotonic()))
            except queue.Empty:
                break
            if response is None:
                raise RuntimeError('Codex app-server exited')
            if response.get('id') != self.serial:
                continue
            if 'error' in response:
                raise RuntimeError(f'{method}: unavailable (code {response["error"].get("code")})')
            return response.get('result') or {}
        raise RuntimeError(f'{method}: timed out')

    def close(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill(); self.process.wait()


def pack(sample):
    if sample.get('version') != 1 or sample.get('provider') not in (0, 1):
        raise ValueError('invalid snapshot provider/version')
    tokens = sample.get('tokens')
    if tokens is not None and (not numeric(tokens, MAX_TOKENS) or int(tokens) != tokens):
        raise ValueError('invalid tokens')
    flags = 1 if tokens is not None else 0
    resets, used = [], []
    for i, value in enumerate(sample['windows']):
        if value is not None:
            if window(value, 'usedPercent', 'resetsAt') is None:
                raise ValueError('invalid quota window')
            flags |= 2 << i
        resets.append(value['resetsAt'] if value else 0)
        used.append(round(value['usedPercent']*100) if value else 0)
    if len(resets) != 2:
        raise ValueError('two windows required')
    data = struct.pack('<BBBBIIIQIHHIiI', 1, 1, sample['provider'], flags,
        sample['sequence'], sample['stream'], sample['observed'], tokens or 0,
        resets[0], used[0], used[1], resets[1], sample['utcOffset'], int(time.time()))
    data += struct.pack('<I', zlib.crc32(data))
    assert len(data) == 48
    return b'\x1eP'+data.hex().encode()+b'\n'


def send_snapshot(port, sample):
    packet = pack(sample)
    marker = f'PET_ACK {sample["provider"]} {sample["sequence"]}'.encode()
    for attempt in range(3):
        port.write(packet);port.flush()
        until = time.monotonic()+3
        while time.monotonic() < until:
            if port.readline().rstrip().endswith(marker):
                return
    raise RuntimeError('device did not acknowledge telemetry; check firmware/port')


def send_clock(port):
    from datetime import datetime
    now = int(time.time())
    offset = int(datetime.now().astimezone().utcoffset().total_seconds())
    data = bytearray(44)
    data[0:2] = bytes([1, 2])
    struct.pack_into('<I', data, 4, now)
    struct.pack_into('<iI', data, 36, offset, now)
    data.extend(struct.pack('<I', zlib.crc32(data)))
    port.write(b'\x1eP'+data.hex().encode()+b'\n');port.flush()
    until = time.monotonic()+3
    while time.monotonic()<until:
        if port.readline().rstrip().endswith(f'PET_ACK 2 {now}'.encode()):
            return
    raise RuntimeError('clock sync not acknowledged')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--state-dir', type=Path, default=DEFAULT)
    sub = p.add_subparsers(dest='command', required=True)
    c = sub.add_parser('codex', help='read account usage and quota; no model generation')
    c.add_argument('--executable', default=shutil.which('codex.exe') or shutil.which('codex'))
    c.add_argument('--bucket', default='codex')
    c.add_argument('--once', action='store_true')
    c.add_argument('--interval', type=int, default=60)
    sub.add_parser('claude-statusline', help='read Claude status JSON from stdin; optional transcript metadata')
    s = sub.add_parser('send', help='send available sanitized snapshots over USB')
    s.add_argument('--port', required=True)
    s.add_argument('--once', action='store_true')
    s.add_argument('--clock-only', action='store_true', help='sync alarm clock without any AI account')
    a = p.parse_args()
    if a.command == 'claude-statusline':
        try:
            claude_statusline(a.state_dir, json.loads(sys.stdin.read(65537)))
        except (OSError, ValueError, sqlite3.Error):
            print('Pet link unavailable')
        return
    if a.command == 'codex':
        if not a.executable:
            p.error('Codex CLI not found; specify --executable')
        while True:
            reader = None
            try:
                reader = CodexReader(a.executable)
                try:
                    account = reader.request('account/read').get('account') or {}
                except RuntimeError:
                    account = {}
                identity = hashlib.sha256(str(account.get('email') or account.get('id') or account.get('type') or 'default').encode()).hexdigest()
                limits = reader.request('account/rateLimits/read')
                try:
                    usage = reader.request('account/usage/read')
                except RuntimeError:
                    usage = {}  # Unknown remains null; quota % is not token usage.
                sample = publish(a.state_dir, normalize_codex(limits, usage, a.bucket), identity=identity)
                print('Codex snapshot saved; tokens '+('available' if sample['tokens'] is not None else 'unavailable'), flush=True)
            except (OSError, RuntimeError) as e:
                print(str(e), file=sys.stderr)
                if a.once:
                    return 1
            finally:
                if reader:
                    reader.close()
            if a.once:
                return 0
            time.sleep(max(30, a.interval))
    else:
        import serial
        while True:
            try:
                with serial.Serial(a.port, 115200, timeout=.1, write_timeout=2) as port:
                    while True:
                        send_clock(port)
                        sent = a.clock_only
                        if not a.clock_only and (a.state_dir / 'metrics.sqlite3').exists():
                            with closing(database(a.state_dir)) as db:
                                samples = db.execute('select value from snapshots order by provider').fetchall()
                            for (value,) in samples:
                                sample = json.loads(value)
                                if time.time()-sample['observed'] <= 180:
                                    send_snapshot(port, sample);sent = True
                        if a.once:
                            if not sent:
                                raise RuntimeError('no fresh snapshots; run a collector first')
                            return 0
                        time.sleep(5)
            except (OSError, RuntimeError, ValueError, serial.SerialException) as e:
                print(str(e), file=sys.stderr)
                if a.once:
                    return 1
                time.sleep(5)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
