"""Native mesh contracts through the current Shiki frame API, ASan/UBSan."""
import argparse, hashlib, os, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
QJS=ROOT/'components/quickjs-ng/quickjs-ng'
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--opt',choices=['0','2'],default='2');a=p.parse_args()
 out=ROOT/'.cache/mesh-shiki-contract';out.mkdir(parents=True,exist_ok=True)
 flags=['-std=gnu11','-O'+a.opt,'-g','-ffp-contract=off','-fsanitize=address,undefined','-fno-omit-frame-pointer','-DQUICKJS_NG_BUILD','-D_GNU_SOURCE','-DKSN_PROC_HOST_TEST']
 includes=['-I'+str(x) for x in [QJS,ROOT/'main',ROOT/'main/pocket',ROOT/'main/ui/kasane',ROOT/'main/ayatsuri',ROOT/'tools/kasane_contract',ROOT/'tools/hostshim']]
 env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
 def run(cmd): subprocess.run(list(map(str,cmd)),cwd=ROOT,env=env,check=True)
 for f in ['tools/ayatsuri/asset/test_import.cjs','tools/ayatsuri/asset/compact_opaque/test_batch.cjs','tools/ayatsuri/model/test_pack_mesh.cjs','tools/kasane_contract/mesh_depth_fixture.cjs']:run(['node',f])
 run(['node','-e',"require('./tools/ayatsuri/model/sided_fixtures.cjs').write()"])
 objects=[]
 key=hashlib.sha256((' '.join(flags)+subprocess.check_output(['cc','--version'],text=True)+''.join(hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(QJS.glob('*')) if f.suffix in ('.c','.h'))).encode()).hexdigest()
 for name in ['dtoa','libregexp','libunicode','quickjs','quickjs-vm']:
  obj=out/(key+'-'+name+'.o');objects.append(obj)
  if not obj.exists():run(['cc',*flags,*includes,'-w','-c',QJS/(name+'.c'),'-o',obj])
 rom=out/'rom.c';rom.write_text('#include "ksn_shiki.h"\nconst ksn_shiki_rom_entry *ksn_shiki_rom_table(unsigned *n){*n=0;return 0;}\n')
 shared=[ROOT/'main/ui/kasane'/f for f in ['ksn_shiki_frame.c','ksn_core.c','ksn_render.c','ksn_blend_pie.c','ksn_shiki.c','ksn_mesh_depth.c']]+[ROOT/'main/ayatsuri'/f for f in ['ayatsuri_model.c','ayatsuri_visibility.c','compact_opaque.c']]+[ROOT/'main/pocket'/f for f in ['pocket_shiki_frame.c','pocket_shape.c','pocket_shiki_api.c','shiki_heap.c']]+[ROOT/'tools/kasane_contract/proc_native_weak.c',rom]
 core=out/'core';run(['cc',*flags,*includes,'-Wall','-Wextra','-Werror',ROOT/'main/ayatsuri/ayatsuri_model.c',ROOT/'main/ayatsuri/ayatsuri_visibility.c',ROOT/'main/ayatsuri/compact_opaque.c',ROOT/'main/ui/kasane/ksn_shiki_frame.c',ROOT/'main/ui/kasane/ksn_mesh_depth.c',ROOT/'tools/ayatsuri/model/test_sidedness.c','-lm','-o',core]);run([core])
 # Pinned public a426 codec: compare old bytes and old-reader rejection.
 old=ROOT/'tools/kasane_contract/fixtures/compact_opaque_a426f7a.c'
 data=old.read_bytes();assert hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()=='e1c37f77f1dffa3ecdea094897b53169def4a840'
 oldobj=out/'old-codec.o';renames=['-D'+n+'=legacy_'+n for n in ['co_append','co_read','co_vertex_at','co_triangle_at']]
 run(['cc',*flags,*includes,*renames,'-c',old,'-o',oldobj])
 side=out/'side-record';run(['cc',*flags,*includes,ROOT/'main/ayatsuri/compact_opaque.c',ROOT/'main/ui/kasane/ksn_mesh_depth.c',ROOT/'main/ui/kasane/ksn_shiki_frame.c',ROOT/'tools/kasane_contract/test_mesh_side_record.c',oldobj,'-lm','-o',side]);run([side])
 for test in ['test_mesh_depth_qjs','test_gouraud_qjs','test_pocket_proc_qjs']:
  sources=[x for x in shared if test!='test_pocket_proc_qjs' or x.name!='pocket_shape.c']
  binary=out/test;run(['cc',*flags,*includes,*sources,*objects,ROOT/'tools/kasane_contract'/(test+'.c'),'-lm','-o',binary]);run([binary])
 print('SHIKI_NATIVE_MESH_PASS current_frame_api mesh_gouraud managed_both_submit lifecycle existing_frame_suite')
if __name__=='__main__':main()
