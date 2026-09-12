"""Fetch pinned upstream for the components this tree still builds out of .cache/.

pocketjs_guest, quickjs-ng and pocketjs_ui_qjs are vendored (components/
pocketjs_guest, components/quickjs-ng, components/pocketjs_ui_qjs) and are
NOT touched here -- copying over them would silently discard whatever L1+
work has landed on the vendored copy. Only pocketjs_ui_core /
pocketjs_render_rgb565, thin C shims over prebuilt Rust archives, still
build in place out of .cache/pocketjs and need this checkout.
"""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
REVISION = '6a0a1b6c91a506c473fc37a0256a47b12eceeca8'
source = ROOT / '.cache/pocketjs'
if not source.exists():
    subprocess.run(['git', 'clone', 'https://github.com/pocket-stack/pocketjs.git', str(source)], check=True)
subprocess.run(['git', '-C', str(source), 'checkout', '--detach', REVISION], check=True)
print('Fetched PocketJS', REVISION)
bmi = ROOT / '.cache/bmi270'
if not bmi.exists():
    subprocess.run(['git','clone','https://github.com/boschsensortec/BMI270_SensorAPI.git',str(bmi)],check=True)
subprocess.run(['git','-C',str(bmi),'checkout','--detach','41129fcfe39c583ee5462d79195741945d51c1fe'],check=True)
# libopus, for pocket.audio.player's Opus decoding. FETCHED AT A TAG, NOT VENDORED:
# components/opus/CMakeLists.txt compiles these sources in place out of .cache/, the
# same arrangement pocketjs_ui_core/render_rgb565 above use, so this tree
# carries the build recipe and licenses/libopus.txt but none of xiph/opus's ~100 C files.
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
