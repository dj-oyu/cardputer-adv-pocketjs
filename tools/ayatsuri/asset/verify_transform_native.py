"""Actual Blender TRS -> packed model -> native single-sheet coverage oracle.

Closed-form source geometry and independent barycentric solve; no renderer in
reference. Ambient white isolates visibility/coverage, not shading normal quality.
Boundary pixels within .001px are explicitly unaccepted (F32 projection margin).
"""
import argparse,json,math,os,struct,subprocess,hashlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];ASSET=ROOT/'tools/ayatsuri/asset'
p=argparse.ArgumentParser();p.add_argument('--exports',type=Path,required=True);p.add_argument('--output-dir',type=Path,required=True);p.add_argument('--asan',action='store_true');a=p.parse_args();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=False)
env=dict(os.environ)
if a.asan:env.update(ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
def run(cmd,label):
 r=subprocess.run(list(map(str,cmd)),cwd=ROOT,env=env,capture_output=True,text=True,timeout=120);(out/(label+'.log')).write_text(r.stdout+r.stderr);r.check_returncode()
run(['node',ASSET/'pack_transform_probe.cjs',a.exports.resolve(),out],'pack')
sources=[ASSET/'render_transform_probe.c']+[ROOT/'main'/s for s in ['ayatsuri/ayatsuri_model.c','ayatsuri/ayatsuri_visibility.c','ayatsuri/compact_opaque.c','ui/kasane/ksn_shiki_frame.c','ui/kasane/ksn_mesh_depth.c']]
flags=['-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-ffp-contract=off']
if a.asan:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
run(['cc',*flags,'-I',ROOT/'main',*sources,'-lm','-o',out/'render'],'compile')
def expected(m,sign):
 pts=[]
 for p in m['vertices']:
  x,y,z=[v*s for v,s in zip(p,m['scale'])];t=m['rotationZ'];X=x*math.cos(t)-y*math.sin(t)+m['translation'][0];Y=x*math.sin(t)+y*math.cos(t)+m['translation'][1];Z=z+m['translation'][2]
  pts.append((120+sign*12*X,67-12*Z))
 face=[0,2,1] if math.prod(m['scale'])>0 else [0,1,2]
 A,B,C=[pts[i] for i in face];u=(B[0]-A[0],B[1]-A[1]);v=(C[0]-A[0],C[1]-A[1]);det=u[0]*v[1]-u[1]*v[0]
 # Solve two-coordinate barycentric system rather than copy production edges.
 values=[];accepted=[]
 for y in range(135):
  for x in range(240):
   dx,dy=x-A[0],y-A[1];b=(dx*v[1]-dy*v[0])/det;c=(u[0]*dy-u[1]*dx)/det;w=[1-b-c,b,c]
   distances=[abs(w[0]*det)/math.dist(B,C),abs(w[1]*det)/math.dist(C,A),abs(w[2]*det)/math.dist(A,B)]
   accepted.append(min(distances)>.001)
   values.append(65535 if det>0 and min(w)>0 else 0)
 return values,accepted,det

def verify(raw,values,accepted):
 assert len(raw)==64800
 pixels=struct.unpack('<32400H',raw)
 assert all(not ok or actual==want for actual,want,ok in zip(pixels,values,accepted)),'INDEPENDENT_COVERAGE_COLOR_MISMATCH'
 return sum(v!=0 for v in pixels)
results=[]
for name in ['nonuniform','mirrored']:
 m=json.loads((a.exports/(name+'.json')).read_text())
 for sign in [1,-1]:
  label=f'{name}-{sign}';target=out/(label+'.u16le');run([out/'render',out/(name+'.aynm'),target,sign],label)
  values,accepted,det=expected(m,sign);raw=target.read_bytes();coverage=verify(raw,values,accepted)
  assert (coverage>0)==(det>0)
  wrong=out/(label+'-wrong.u16le');run([out/'render',out/(name+'-wrong-winding.aynm'),wrong,sign],label+'-wrong')
  try:verify(wrong.read_bytes(),values,accepted)
  except AssertionError:pass
  else:raise AssertionError('actual wrong winding render accepted')
  assert sum(accepted)>=32000
  corrupt=bytearray(raw);index=next(i for i,ok in enumerate(accepted) if ok and (values[i]!=0 if det>0 else True));corrupt[index*2]^=1
  try:verify(corrupt,values,accepted)
  except AssertionError:pass
  else:raise AssertionError('pixel corruption accepted')
  results.append(dict(case=name,cameraSign=sign,coverage=coverage,acceptedPixels=sum(accepted),excludedBoundaryPixels=len(accepted)-sum(accepted),screenDeterminant=det,imageSha256=hashlib.sha256(raw).hexdigest()))
report=dict(passed=True,sourceSha256={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources+[Path(__file__),ASSET/'pack_transform_probe.cjs']},inputSha256={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in a.exports.glob('*') if p.suffix in ('.glb','.json')},modelSha256={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.glob('*.aynm')},cases=results,negativeRenders=4,negativePixelMutations=4,sanitizers='ASan/UBSan; LSan disabled' if a.asan else 'none',limitations=['Only two actual Blender object-TRS tilted single triangles','Ambient white coverage/winding oracle; no shading-normal/pixel lighting claim','Boundary distance <= .001px excluded','No overlapping/depth-tie surface or arbitrary scene/device acceptance'])
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
