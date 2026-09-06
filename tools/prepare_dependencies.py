"""Fetch pinned upstream and prepare the local ESP-IDF guest component."""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REVISION = '6a0a1b6c91a506c473fc37a0256a47b12eceeca8'
source = ROOT / '.cache/pocketjs'
if not source.exists():
    subprocess.run(['git', 'clone', 'https://github.com/pocket-stack/pocketjs.git', str(source)], check=True)
subprocess.run(['git', '-C', str(source), 'checkout', '--detach', REVISION], check=True)
guest = ROOT / '.cache/components/pocketjs_guest'
shutil.copytree(source / 'hosts/esp-idf/components/pocketjs_guest', guest, dirs_exist_ok=True)
preparer = guest / 'tools/prepare_quickjs.py'
text = preparer.read_text()
# Reviewed Espressif Registry 0.14.0 source: identical reverse/species write
# paths, four-space formatting. Keep the fail-closed check and immutable guards.
old = '8779a5050c2a78905b9f5fa671f8e33920d4ec9d82cc99157cf12bd5339e4a48'
new = '36128da188cb236ffd029dd3c672ff8f85e5a196a9211e267a515c8efc1ab52c'
assert text.count(old) == 1
preparer.write_text(text.replace(old, new), encoding='utf-8')
print('Prepared PocketJS', REVISION)
bmi = ROOT / '.cache/bmi270'
if not bmi.exists():
    subprocess.run(['git','clone','https://github.com/boschsensortec/BMI270_SensorAPI.git',str(bmi)],check=True)
subprocess.run(['git','-C',str(bmi),'checkout','--detach','41129fcfe39c583ee5462d79195741945d51c1fe'],check=True)
