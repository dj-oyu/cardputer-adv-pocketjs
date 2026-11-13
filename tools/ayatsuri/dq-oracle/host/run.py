"""Isolated experimental header tests; no production method/API changes."""
import pathlib,subprocess,os,sys,json,hashlib
here=pathlib.Path(__file__).resolve().parent;root=here.parents[3];out=pathlib.Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=False)
subprocess.run(['node',str(root/'tools/ayatsuri/skin/render_fixture.cjs'),str(out/'fixture')],check=True)
inputs=list(here.rglob('*.h'))+list(here.glob('*.c'))+[here/'run.py']+list((root/'main/ayatsuri').glob('*.h'))+[root/'tools/ayatsuri/skin/test_render.c']
sources=[root/p for p in ['main/ayatsuri/ayatsuri_model.c','main/ayatsuri/ayatsuri_visibility.c','main/ayatsuri/compact_opaque.c','main/ui/kasane/ksn_shiki_frame.c','main/ui/kasane/ksn_mesh_depth.c']]
inputs+=sources+list((root/'main/ui/kasane').glob('*.h'))+list((root/'tools/hostshim').glob('*.h'))+[root/'tools/ayatsuri/skin/render_fixture.cjs',root/'tools/ayatsuri/model/pack_model.cjs']
fixture_hash={str(p.name):hashlib.sha256(p.read_bytes()).hexdigest() for p in (out/'fixture').glob('*.aynm')}
def hashes():return {str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
before=hashes();head=subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip();commands=[];logs={}
for mode,flags in [('o2',['-O2']),('san',['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
 for test in ['core','admission','native']:
  exe=out/(test+'-'+mode)
  cmd=['cc','-std=gnu11','-Wall','-Wextra','-Werror','-ffp-contract=off','-fno-fast-math',*flags,*['-I'+str(p) for p in [here/'support',here/'support/ayatsuri',root/'main',root/'main/ayatsuri',root/'tools/hostshim',root/'tools/ayatsuri/skin']],str(here/('test_'+test+'.c'))]
  if test=='native':cmd+=list(map(str,sources))+['-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free']
  cmd+=['-lm','-o',str(exe)];commands.append(cmd);subprocess.run(cmd,check=True)
  args=[str(exe)]+([str(out/'fixture')] if test=='native' else []);log=subprocess.check_output(args,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'));print(log,end='');logs[exe.name]=log;(out/(exe.name+'.log')).write_text(log)
assert hashes()==before
(out/'report.json').write_text(json.dumps({'baseHead':head,'sourceSha256':before,'fixtureSha256':fixture_hash,'commands':commands,'logs':logs,'scope':'experimental core/native mesh only; shared renderer; no production binding, JS/OOM/DMA/device; LSan disabled'},indent=2)+'\n')
