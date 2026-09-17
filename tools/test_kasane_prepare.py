"""Dependency preparation never touches the removed PocketJS checkout or Rust."""
from pathlib import Path
import runpy
import sys
from unittest.mock import patch

root = Path(__file__).resolve().parents[1]
for argv in (['prepare_dependencies.py'], ['prepare_dependencies.py', '--kasane-only']):
    calls = []
    with patch.object(sys, 'argv', argv), \
         patch('subprocess.run', side_effect=lambda args, **kw: calls.append(args)):
        runpy.run_path(str(root / 'tools/prepare_dependencies.py'), run_name='__main__')
    assert calls, 'no dependency preparation'
    # The repository's own path contains "pocketjs", so look only at what
    # follows it: an upstream URL or a .cache/pocketjs checkout.
    words = [str(arg).replace(str(root), '<root>').lower() for call in calls for arg in call]
    assert not any('pocket-stack/pocketjs' in w or '.cache/pocketjs' in w.replace('\\', '/')
                   or 'cargo' in w or 'rust' in w for w in words), calls
    assert any('41129fcfe39c583ee5462d79195741945d51c1fe' in call for call in calls)
    assert any('prepare_minimp3.py' in str(call) for call in calls)
print('KASANE_PREPARE PASS: non-UI dependencies only (subprocess mocked)')
