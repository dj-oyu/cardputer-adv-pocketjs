"""Fetch pinned upstream and prepare the local ESP-IDF guest component."""
from pathlib import Path
import shutil
import subprocess
import sys

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
# libopus, for pocket.audio.player's Opus decoding. FETCHED AT A TAG, NOT VENDORED:
# components/opus/CMakeLists.txt compiles these sources in place out of .cache/, the
# same arrangement the PocketJS guest above uses, so this tree carries the build
# recipe and licenses/libopus.txt but none of xiph/opus's ~100 C files.
#
# v1.6.1 rather than the 1.5.2 the study named: the only decode-cost measurement
# this project holds was taken on master a6128f4 (2026-09-06), and v1.6.1
# (2026-01-13) is the release nearest behind it. See components/opus/CMakeLists.txt.
OPUS_TAG = 'v1.6.1'
opus = ROOT / '.cache/codecs/opus-1.6.1'
if not opus.exists():
    subprocess.run(['git', 'clone', '--branch', OPUS_TAG, '--depth', '1',
                    'https://github.com/xiph/opus.git', str(opus)], check=True)
print('Prepared libopus', OPUS_TAG)
subprocess.run([sys.executable, str(ROOT / 'tools/prepare_minimp3.py')], check=True)
