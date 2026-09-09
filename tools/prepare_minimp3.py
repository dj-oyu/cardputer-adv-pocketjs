"""Fetch the reviewed scalar MP3 decoder at an immutable revision."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REVISION = 'ea99364f61c14656440e8d77e9c233ccf3124633'
source = ROOT / '.cache/codecs/minimp3'
if not source.exists():
    subprocess.run(['git', 'clone', 'https://github.com/lieff/minimp3.git', str(source)], check=True)
subprocess.run(['git', '-C', str(source), 'checkout', '--detach', REVISION], check=True)
print('Prepared minimp3', REVISION)
