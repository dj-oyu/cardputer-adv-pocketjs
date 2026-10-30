#!/usr/bin/env python3
"""Run a pinned prepare_derby_app_diag.py on either reviewed UI owner-loop layout.

The pinned generators (5bf3323, 1e68b26) anchor diag_owner_step() on
`while(1) {` followed directly by the app-teardown comment. main.c has since
put native_observe() and app_control_pump() first, so the pinned anchor no
longer matches and every workflow that prepares the oval/straight diagnostic
on a current checkout failed at its first step. This runner keeps the pinned
generator unchanged and swaps only that one anchor, with the adapter that
prepare_derby_pad_diag_compat.py already uses for the pad profile.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess

# The generator revisions the workflows pin; another one is refused.
HEADS = ('5bf332368d6e793cd40a8a18a48b4da4ac270622', '1e68b268a4b91ec54e88f408118de3e39b8b6bc5')
HERE = Path(__file__).resolve().parent


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def pinned_common(path):
    path = path.resolve()
    root = path.parents[2]
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    assert head in HEADS, f'unreviewed generator revision {head}'
    assert not subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=root, text=True)
    assert path == root / 'tools/games/prepare_derby_app_diag.py'
    return load(path, 'derby_common_diag'), head


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--common', type=Path, required=True)
    parser.add_argument('--checkout', type=Path, required=True)
    parser.add_argument('--profile', choices=['oval', 'straight'], required=True)
    args = parser.parse_args()
    # The pad runner's adapter: one reviewed owner loop, exact-once anchors.
    adapter = load(HERE / 'prepare_derby_pad_diag_compat.py', 'derby_owner_adapter')
    common, head = pinned_common(args.common)
    dst = args.checkout.resolve()
    source = (dst / 'main/main.c').read_text()
    adapter.insert_owner_step(source)  # Reject unsupported layouts before any writes.
    layout = 'native-control' if adapter.CONTROL in source else 'teardown-first'
    common.change = adapter.adapt_change(common.change)
    common.prepare(dst, args.profile)
    manifest = json.loads((dst / 'diagnostic.json').read_text())
    manifest['common_generator'] = head
    manifest['owner_loop_adapter'] = dict(version=1, sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    manifest['owner_loop_layout'] = layout
    (dst / 'diagnostic.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
