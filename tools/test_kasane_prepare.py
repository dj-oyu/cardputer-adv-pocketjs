"""Dependency preparation in Kasane-only mode never touches the old checkout."""
from pathlib import Path
import runpy
import sys
from unittest.mock import patch

root = Path(__file__).resolve().parents[1]
calls = []
with patch.object(sys, 'argv', ['prepare_dependencies.py', '--kasane-only']), \
     patch('subprocess.run', side_effect=lambda args, **kw: calls.append(args)):
    runpy.run_path(str(root / 'tools/prepare_dependencies.py'), run_name='__main__')
assert calls, 'no dependency preparation'
assert not any('pocketjs' in str(arg).lower() or 'rust' in str(arg).lower()
               for call in calls for arg in call), calls
assert any('41129fcfe39c583ee5462d79195741945d51c1fe' in call for call in calls)
assert any('prepare_minimp3.py' in str(call) for call in calls)
print('KASANE_PREPARE PASS: non-UI dependencies only (subprocess mocked)')
