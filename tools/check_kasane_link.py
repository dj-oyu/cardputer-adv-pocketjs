"""Prove a firmware build has no legacy UI components, sources or symbols.

The legacy UI was removed from the tree; this keeps it from coming back through
a component, an archive or a source row.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--nm', required=True)
args = parser.parse_args()
build = args.build.resolve()
description = json.loads((build / 'project_description.json').read_text())
for component in description['build_components']:
    assert not re.search(r'taffy|pocketjs_(ui_core|ui_qjs|render_rgb565)', component, re.I), component
for filename in ('cardputer_pocketjs.map', 'build.ninja', 'compile_commands.json'):
    data = (build / filename).read_text()
    assert not re.search(r'libpocketjs_idf_|libpocketjs_ui_|libpocketjs_render_|taffy', data, re.I), filename
    assert not re.search(r'(?:jsfont|pet_assets|pocket_ui|render_accel)\.c(?:[.\\/"\s])', data), filename
symbols = subprocess.check_output([args.nm, '-C', str(build / 'cardputer_pocketjs.elf')], text=True)
assert not re.search(r'pocketjs_(?:ui_|rgb565_)|taffy', symbols, re.I), 'legacy symbol in ELF'
for required in ('ksn_core_init', 'pocket_input_install', 'pocketjs_guest_frame'):
    assert required in symbols, 'missing ' + required
print('KASANE_LINK PASS: component graph, link/map, source list and ELF symbols')
