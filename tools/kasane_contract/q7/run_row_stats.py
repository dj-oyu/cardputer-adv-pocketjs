"""Compare scalar row statistics with its pinned certified predecessor, no hardware."""
import argparse, os, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--sanitize', action='store_true')
a = p.parse_args()
out = ROOT / '.cache/q7-row-stats'
out.mkdir(parents=True, exist_ok=True)
flags = ['-std=gnu11', '-O2', '-g', '-fno-fast-math', '-fno-associative-math',
         '-fno-reciprocal-math', '-ffp-contract=off', '-frounding-math',
         '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'main'),
         '-I'+str(ROOT/'main/ui/kasane')]
if a.sanitize:
    flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
sources = ['main/ui/kasane/ksn_mesh_q7_certified.c', 'main/ui/kasane/ksn_mesh_prepared.c',
           'main/ui/kasane/ksn_mesh_depth.c', 'main/ui/kasane/ksn_shiki_frame.c',
           'main/ayatsuri/compact_opaque.c', 'tools/kasane_contract/q7/row_stats_oracle.c',
           'tools/kasane_contract/q7/test_row_reuse.c']
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
for mode in range(4):
    binary = out / ('mode'+str(mode)+('-sanitized' if a.sanitize else ''))
    subprocess.run(['cc', *flags, '-DQ7_CERT_MODE='+str(mode),
                    *[str(ROOT/s) for s in sources], '-lm', '-o', str(binary)], check=True)
    for band, rows in [(4, 1), (8, 8)]:
        r = subprocess.run([str(binary), str(band), str(rows)], env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (out/(binary.name+f'-{band}-{rows}.log')).write_text(r.stdout)
        print(f'mode={mode} band={band} rows={rows}\n'+r.stdout, end='', flush=True)
        r.check_returncode()
    failure = out / 'failure'
    subprocess.run(['cc', *flags, '-DQ7_CERT_MODE='+str(mode),
                    *[str(ROOT/s) for s in sources[:-1]],
                    str(ROOT/'tools/kasane_contract/q7/test_row_stats_failure.c'),
                    '-lm', '-o', str(failure)], check=True)
    subprocess.run([str(failure)], env=env, check=True)
print('ROW_STATS_ALL_MODES_PASS exact pixels depth owner stats sticky_flags; host timings are not target predictions')
