"""Local 8 MiB backup using ROM reads (avoids USB stub transfer failures)."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys

p = argparse.ArgumentParser()
p.add_argument('--port', required=True)
p.add_argument('--output', type=Path, required=True)
args = p.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
chunks = []
for i in range(8):
    part = args.output.with_suffix(f'.part{i}')
    subprocess.run([sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', args.port,
                    '--no-stub', 'read-flash', '--no-progress', hex(i * 0x100000), '0x100000', str(part)], check=True)
    data = part.read_bytes()
    assert len(data) == 0x100000
    chunks.append(data)
args.output.write_bytes(b''.join(chunks))
print('BACKUP_OK', args.output.stat().st_size, hashlib.sha256(args.output.read_bytes()).hexdigest())
