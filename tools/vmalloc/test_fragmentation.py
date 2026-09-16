"""Negative/positive controls for physical free-extent fragmentation metrics."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
binary = root / '.cache/vmalloc' / ('vmalloc_replay-' + (sys.argv[1] if len(sys.argv) > 1 else 'o2'))
for allocator in ('tlsf', 'estalloc', 'naive', 'segment', 'slab'):
    for case in ('contiguous', 'fragmented', 'teardown_only'):
        trace = root / 'tools/vmalloc/tests' / (case + '.trace')
        output = subprocess.check_output([
            str(binary), '--allocator', allocator, '--pool', '65536',
            '--sample-every', '1', '--verify', str(trace)], text=True)
        fields = dict(part.split('=', 1) for part in output.split() if '=' in part)
        assert fields['result'] == 'OK' and fields['verify'] == 'OK', output
        frag = int(fields['app_ext_frag'])
        if case != 'fragmented':
            assert frag == 0, output
        else:
            assert frag >= 4096, output
        assert int(fields['app_min_pool_largest']) < 65536, output
        if case == 'teardown_only':
            assert int(fields['peak_external_frag']) >= 4096, output
        print(allocator, case, 'external_bytes=' + str(frag), 'OK')
