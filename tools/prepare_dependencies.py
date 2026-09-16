"""Fetch pinned upstream for the components this tree still builds out of .cache/.

pocketjs_guest and quickjs-ng are vendored (components/pocketjs_guest,
components/quickjs-ng) and are NOT touched here -- copying over them would
silently discard whatever L1+ work has landed on the vendored copy. The
PocketJS checkout and the Rust UI archives are no longer needed: the legacy
UI core, its binding and the rgb565 renderer were removed from the firmware.
"""
from pathlib import Path
import argparse
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
# Accepted and ignored: every preparation is Kasane-only now.
parser.add_argument('--kasane-only', action='store_true', help=argparse.SUPPRESS)
parser.parse_args()
bmi = ROOT / '.cache/bmi270'
if not bmi.exists():
    subprocess.run(['git','clone','https://github.com/boschsensortec/BMI270_SensorAPI.git',str(bmi)],check=True)
subprocess.run(['git','-C',str(bmi),'checkout','--detach','41129fcfe39c583ee5462d79195741945d51c1fe'],check=True)
# libopus, for pocket.audio.player's Opus decoding. FETCHED AT A TAG, NOT VENDORED:
# components/opus/CMakeLists.txt compiles these sources in place out of .cache/, the
# same arrangement minimp3 uses, so this tree
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
