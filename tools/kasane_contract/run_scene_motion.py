"""Public rigid-joint scene/motion vertical slice on real QuickJS and Kasane."""
import hashlib
import json
import subprocess
from run_meshbench_app import ROOT, build_harness


def main():
    subprocess.run(['node', 'tools/ayatsuri/model/scene_motion_fixture.cjs'], cwd=ROOT, check=True)
    out = ROOT/'.cache/scene-motion-host'
    out.mkdir(parents=True, exist_ok=True)
    chunk_list = ROOT/'tools/ayatsuri/model/scene_motion_chunks.txt'
    subprocess.run(['python3', 'tools/make_app_chunks.py', str(out/'generated'), str(chunk_list)], cwd=ROOT, check=True)
    binary, out, env = build_harness('tools/kasane_contract/test_scene_motion_host.c', 'scene-motion-host',
                                   ['main/pocket/pocket_app_load.c', 'tools/hostshim/app_chunks_host.c'])
    inputs = ['apps/lib/ayatsuri_model.js', 'apps/lib/ayatsuri_motion.js',
              'tools/kasane_contract/scene_motion_scenario.js', '.cache/ayatsuri-native/scene-motion.aynm',
              '.cache/ayatsuri-native/scene-motion-double.aynm']
    (out/'runtime-inputs.json').write_text(json.dumps({p: hashlib.sha256((ROOT/p).read_bytes()).hexdigest()
                                                   for p in inputs}, indent=2)+'\n')
    for rows, direct, sided in [(r, d, s) for r in [4, 8] for d in [0, 1] for s in [0, 1]]:
        result = subprocess.run([str(binary), str(rows), str(direct), str(sided)], cwd=ROOT, env=env,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out/f'rows{rows}-direct{direct}-double{sided}.log').write_text(result.stdout)
        print(result.stdout, end='')
        result.check_returncode()
    print('SCENE_MOTION_ALL_ARMS_PASS public synthetic only; no character rig or device claim')


if __name__ == '__main__':
    main()
