"""Strict-compile the real host-query headers under all display guard combinations.

Only two SDK type declarations are supplied; no query implementation is mocked.
This checks header availability and signatures, not target linking/concurrency.
"""
from itertools import product
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'main/app_session.c').read_text()
after_display = source[source.index('#include "disp.h"'):]
match = re.search(r'(#if[^\n]*(?:\\\n[^\n]*)*\n(?:(?!#endif).)*?'
                  r'#include "sound.h"\n#endif)', after_display, re.S)
assert match, 'host-query header block not found'
block = match.group(1)
for header in ('pocket_pixel.h', 'pocket_grid.h', 'pocket_video.h', 'sound.h'):
    assert f'#include "{header}"' in block, header
# Restore the original narrow guard as a negative control. The exact P2a-only
# configuration must diagnose unavailable declarations instead of passing vacuously.
body = block[block.index('/*'):]
narrow = '#ifdef CONFIG_POCKET_FRAME_BREAKDOWN\n' + body
cc = shlex.split(os.environ.get('CC', 'cc'))
with tempfile.TemporaryDirectory(prefix='session-query-headers-') as directory:
    temp = Path(directory)
    (temp / 'driver').mkdir()
    (temp / 'esp_err.h').write_text('typedef int esp_err_t;\n')
    (temp / 'driver/i2c_master.h').write_text(
        'typedef void *i2c_master_bus_handle_t;\n')
    def compile_block(text, flags):
        frame, defer, dma = flags
        active = frame or (defer and dma)
        calls = ('''bool query_types(void) {
    bool (*sound)(void) = sound_capture_active;
    bool (*pixel)(void) = pocket_pixel_pending;
    bool (*video)(void) = pocket_video_pending;
    bool (*grid)(void) = pocket_grid_pending;
    return sound() || pixel() || video() || grid();
}
''' if active else 'int no_queries;\n')
        test = temp / 'test.c'
        test.write_text('#include <stdbool.h>\n' + text + '\n' + calls)
        command = cc + ['-std=c11', '-O2', '-Werror=implicit-function-declaration',
                        '-Werror=incompatible-pointer-types', '-DQUICKJS_NG_BUILD']
        for include in (temp, ROOT / 'main', ROOT / 'main/pocket', ROOT / 'main/hal',
                        ROOT / 'components/quickjs-ng/quickjs-ng'):
            command += ['-I', str(include)]
        for enabled, name in zip(flags, ('CONFIG_POCKET_FRAME_BREAKDOWN',
                                        'CONFIG_POCKET_DISP_DEFER', 'KASANE_DMA_OVERLAP')):
            if enabled:
                command.append('-D' + name + '=1')
        return subprocess.run(command + ['-c', str(test), '-o', str(temp / 'test.o')],
                              capture_output=True, text=True)
    for flags in product((False, True), repeat=3):
        result = compile_block(block, flags)
        assert result.returncode == 0, (flags, result.stderr)
    result = compile_block(narrow, (False, True, True))
    assert result.returncode != 0, 'narrow-guard negative control unexpectedly passed'
    assert 'sound_capture_active' in result.stderr, result.stderr
print('session query headers: 8 configurations PASS; narrow-guard regression detected')
