"""Compile the actual session dispatch functions with deterministic owner ports."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/app_session.c').read_text()
start = source.index('static void run_pumps(uint32_t buttons) {')
end = source.index('// The half of a turn that is not JavaScript:', start)
with tempfile.TemporaryDirectory(prefix='ksn-session-') as directory:
    path = Path(directory)
    (path / 'session_dispatch_impl.inc').write_text(source[start:end])
    (path/'detail_interrupt_impl.inc').write_text(source[source.index('static int interrupt('):source.index('void app_vm_watchdog(')])
    owner=(root/'main/main.c').read_text()
    (path/'detail_end_run_impl.inc').write_text(owner[owner.index('static void end_run('):owner.index('// Resident suspension')])
    begin=owner.index('static bool end_derby_detail_run(')
    finish=owner.index('// Repaint, and report what one costs.',begin)
    # The leading #ifdef belongs to the helper, and the real closing #endif
    # follows it. Include both with the complete production tick_run body.
    (path/'detail_owner_impl.inc').write_text('#ifdef POCKET_DERBY_DETAIL_MEASURE\n'+owner[begin:finish])
    board = (root / 'main/hal/board.c').read_text()
    capture_start = board.index('static bool capture,')
    capture_end = board.index('static esp_err_t tx(', capture_start)
    (path / 'capture_scope_impl.inc').write_text(board[capture_start:capture_end])
    for flags in (['-O1', '-g', '-fsanitize=address,undefined'], ['-O2', '-fstrict-aliasing']):
        exe = path / 'test'
        subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        *flags, '-I', str(path),
                        str(root / 'tools/test_session_dispatch.c'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        *flags, '-DTEST_DERBY_DETAIL_EXIT', '-I', str(path),
                        str(root/'tools/test_session_dispatch.c'), '-o', str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
