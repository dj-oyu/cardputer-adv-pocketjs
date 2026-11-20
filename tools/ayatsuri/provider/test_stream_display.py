"""Linux host: real pocket/Kasane/streaming code, synthetic copied-DMA port."""
import argparse
import os
import pathlib
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--out', type=pathlib.Path, required=True)
p.add_argument('--sanitize', action='store_true')
a = p.parse_args()
root = pathlib.Path(__file__).resolve().parents[3]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
body = (root/'tools/test_pocket_kasane.c').read_text()
fixture = (root/'tools/ayatsuri/provider/stream_display_test.inc').read_text()
fixture += '\n' + (root/'tools/ayatsuri/provider/stream_material_test.inc').read_text()
assert body.count('int main(void) {') == 1
body = body.replace('int main(void) {', fixture + '\nint main(void) {')
assert body.count('    repair_tests();') == 1
body = body.replace('    repair_tests();', '    repair_tests();\n    stream_diag_tests();\n    stream_material_tests();')
source = out/'test.c'
source.write_text(body)
flags = '-O1 -g -DKASANE_DMA_OVERLAP=1 -DAYATSURI_STREAM_DISPLAY_CANDIDATE=1 -ffp-contract=off -fno-fast-math'
flags += ' -I' + str(root)
if a.sanitize:
    flags += ' -fsanitize=address,undefined'
env = dict(os.environ, TEST_SOURCE=str(source), OUT=str(out/'test'), CACHE=str(out/'qjs'),
           CFLAGS=flags, EXTRA_SOURCES='main/pocket/pocket_stream_candidate.c main/ayatsuri/ayatsuri_model_stream_candidate.c')
subprocess.run(['bash', 'tools/build_kasane_test.sh'], cwd=root, env=env, check=True)
subprocess.run([str(out/'test')], check=True)
