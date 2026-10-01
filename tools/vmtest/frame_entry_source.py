#!/usr/bin/env python3
"""Compile the production source-entry functions into the host regression.

This is a focused source slice, not a simulated app_session: assert its wiring,
then copy the actual functions verbatim. The full session/HAL is device-only.
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
source = (root / 'main/app_session.c').read_text(encoding='utf-8')
start = source.index('static void frame_error(')
end = source.index("// The guest's stats", start)
chunk = source[start:end]
assert 'FRAME_WRAP' not in source, 'native apply wrapper returned to production entry'
assert 'pocketjs_guest_set_frame_error_handler(guest,frame_error,NULL);' in source
assert 'if(user_source) err=eval_user_source(source,length,module_entry);' in source
assert 'pocketjs_guest_eval(guest,"0",1,"bind-frame.js")' in chunk
Path(sys.argv[1]).write_text('/* Extracted verbatim from main/app_session.c. */\n' + chunk, encoding='utf-8')
print('FRAME_ENTRY production source wiring PASS')
