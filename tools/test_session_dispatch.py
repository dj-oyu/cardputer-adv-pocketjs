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
    for flags in (['-O1', '-g', '-fsanitize=address,undefined'], ['-O2', '-fstrict-aliasing']):
        for fair in ([], ['-DCONFIG_POCKET_VM_FAIR=1']):
            exe = path / 'test'
            subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            *flags, *fair, '-I', str(path),
                            str(root / 'tools/test_session_dispatch.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
