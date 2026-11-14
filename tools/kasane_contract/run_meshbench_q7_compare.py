"""Same 150 public frames, Q7 vs original; exports final LE RGB565 reference."""
import hashlib, os, subprocess
from pathlib import Path
from run_meshbench_app import ROOT, build_harness
captures=[]
for name,flags,sources in [
 ('original',[],[]),
 ('q7',['-DKSN_MESH_Q7_BENCH_PREFLIGHT=1','-DKSN_MESH_PREPARED_PROTOTYPE=1','-DKSN_MESH_Q7_CERTIFIED_PROTOTYPE=1','-fno-fast-math','-fno-associative-math','-fno-reciprocal-math','-frounding-math'],['main/ui/kasane/ksn_mesh_prepared.c','main/ui/kasane/ksn_mesh_q7_certified.c'])]:
 binary,out,env=build_harness(cache_name='meshbench-reference-'+name,extra_sources=sources,extra_flags=flags)
 capture=out/'frame149.rgb565le';captures.append(capture)
 env.update(ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',MESHBENCH_REQUEST_CEILING='16384',MESHBENCH_CAPTURE_RGB565=str(capture))
 for rows,image,delayed in [(r,i,d) for r in [4,8] for i in [0,1] for d in [0,1]]:
  result=subprocess.run([str(binary),str(rows),str(image),str(delayed)],cwd=ROOT,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
  (out/f'{rows}-{image}-{delayed}.log').write_text(result.stdout)
  print(result.stdout,end='',flush=True);result.check_returncode()
 print(name,hashlib.sha256(capture.read_bytes()).hexdigest(),flush=True)
a,b=[p.read_bytes() for p in captures]
assert len(a)==64800 and a==b
print('Q7_PUBLIC_SCENE_PASS: 150-frame original oracle equality; final frame149 RGB565LE byte-equal. Not a universal visual bound.')
