#!/usr/bin/env python3
"""Run the pinned pad diagnostic on either reviewed UI owner-loop layout."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess

COMMON_HEAD = '1e68b268a4b91ec54e88f408118de3e39b8b6bc5'
GUARD_HEAD = '146128065e81bb3493985e0b1b7123ff819baaf4'
LEGACY = '    while(1) {\n        /* App teardown'
CONTROL = ('    while(1) {\n        native_observe();\n'
           '        app_control_pump((uint64_t)esp_timer_get_time()/1000);\n'
           '        /* App teardown')
STEP = '        diag_owner_step();\n'


def insert_owner_step(source):
    # Match the teardown boundary, not a generic while loop. Native observation
    # and control delivery keep their production order before the diagnostic.
    counts = [source.count(anchor) for anchor in (LEGACY, CONTROL)]
    assert sum(counts) == 1, f'expected one reviewed owner loop, got {counts}'
    anchor = (LEGACY, CONTROL)[counts.index(1)]
    replacement = anchor.replace('        /* App teardown', STEP + '        /* App teardown')
    return source.replace(anchor, replacement, 1)


def adapt_change(original):
    def change(source, old, new):
        if old == LEGACY:
            assert new == LEGACY.replace('        /* App teardown', STEP + '        /* App teardown')
            return insert_owner_step(source)
        return original(source, old, new)
    return change


def pinned_module(path, head, name):
    path = path.resolve()
    root = path.parents[2]
    assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip() == head
    assert not subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=root, text=True)
    assert path == root / 'tools/games' / path.name
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def self_test(common, guard):
    # Reuse the six completion/deadline cases from the pinned guard unchanged.
    guard.test_owner((common.TEMPLATE / 'owner.inc').read_text())
    change = adapt_change(common.change)
    for anchor in (LEGACY, CONTROL):
        source = '/* prefix */\n' + anchor + ' can time out. */\n/* suffix */'
        expected = source.replace('        /* App teardown', STEP + '        /* App teardown')
        assert change(source, LEGACY, LEGACY.replace('        /* App teardown', STEP + '        /* App teardown')) == expected
    invalid = ['', LEGACY * 2, CONTROL * 2, LEGACY + CONTROL,
               CONTROL.replace('        native_observe();\n', ''),
               CONTROL.replace('native_observe();', 'native_observe_changed();'),
               insert_owner_step(CONTROL)]
    for source in invalid:
        try:
            insert_owner_step(source)
        except AssertionError:
            pass
        else:
            raise AssertionError('unreviewed or ambiguous owner loop accepted')
    # Other common-generator anchors must retain their exact-once rejection.
    assert change('old', 'old', 'new') == 'new'
    for source in ('', 'oldold'):
        try:
            change(source, 'old', 'new')
        except AssertionError:
            pass
        else:
            raise AssertionError('common anchor validation bypassed')
    print(f'DERBY_OWNER_LAYOUT_COMPAT PASS layouts=2 rejected={len(invalid)} common_rejected=2')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--common', type=Path, required=True)
    parser.add_argument('--guard', type=Path, required=True)
    parser.add_argument('--checkout', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    assert args.common.name == 'prepare_derby_app_diag.py'
    assert args.guard.name == 'prepare_derby_pad_bars_diag.py'
    common = pinned_module(args.common, COMMON_HEAD, 'derby_common_diag')
    guard = pinned_module(args.guard, GUARD_HEAD, 'derby_pad_guard')
    assert guard.COMMON_HEAD == COMMON_HEAD
    if args.self_test:
        self_test(common, guard)
        return
    assert args.checkout is not None
    dst = args.checkout.resolve()
    source = (dst / 'main/main.c').read_text()
    insert_owner_step(source)  # Reject unsupported layouts before any writes.
    layout = 'native-control' if CONTROL in source else 'teardown-first'
    common.change = adapt_change(common.change)
    common.prepare(dst, 'pad')
    target = dst / 'main/main.c'
    target.write_text(guard.deadline_guard(target.read_text()))
    patch = subprocess.check_output(['git', 'diff', '--binary'], cwd=dst)
    (dst / 'diagnostic.patch').write_bytes(patch)
    manifest = json.loads((dst / 'diagnostic.json').read_text())
    manifest['common_generator'] = COMMON_HEAD
    manifest['completion_guard'] = 'deadline observed before success; identical for both modes'
    manifest['owner_loop_adapter'] = dict(version=1, sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    manifest['owner_loop_layout'] = layout
    manifest['sha256']['main/main.c'] = hashlib.sha256(target.read_bytes()).hexdigest()
    manifest['patch_sha256'] = hashlib.sha256(patch).hexdigest()
    (dst / 'diagnostic.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
