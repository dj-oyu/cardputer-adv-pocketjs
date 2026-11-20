import copy,os,pathlib,subprocess,tempfile,unittest
from emit_driver_morph_targets import emit


class TestDriverMorphTargets(unittest.TestCase):
    def fixture(self):
        keys=[dict(name='Basis',mute=False,vertex_group='',value=0),dict(name='Smile',mute=False,vertex_group='',value=0)]
        package=dict(schema='hone.source-preservation.v1',runtime_ready=False,source_sha256='synthetic',
                     meshes=[dict(shape_keys=dict(use_relative=True,blocks=keys)),dict(shape_keys=None)])
        routes=dict(schema='hone.driver-route-plan.v1',runtime_ready=False,source_sha256='synthetic',routes=[
            dict(kind='shape_key',program=0,data_path='key_blocks["Smile"].value',array_index=0,source_meshes=[0]),
            dict(kind='shader_property',program=1)])
        return package,routes
    def test_emit_and_unsupported_inputs(self):
        p,r=self.fixture();header,summary=emit(p,r)
        self.assertIn('hone_morph_count = 2, hone_morph_route_count = 1',header)
        self.assertEqual(summary['unbound_programs'],1)
        for mutation in (lambda x:x['meshes'][0]['shape_keys']['blocks'][1].update(vertex_group='masked'),
                         lambda x:x['meshes'][0]['shape_keys']['blocks'][1].update(mute=True),
                         lambda x:x['meshes'][0]['shape_keys']['blocks'][1].pop('vertex_group')):
            bad=copy.deepcopy(p);mutation(bad)
            with self.assertRaises(ValueError):emit(bad,r)
    def test_c_transaction(self):
        here=pathlib.Path(__file__).resolve().parent;h,_=emit(*self.fixture())
        with tempfile.TemporaryDirectory() as tmp:
            p=pathlib.Path(tmp);(p/'routes.h').write_text(h,encoding='utf8')
            (p/'test.c').write_text('''#include "routes.h"
#include <assert.h>
#include <string.h>
int main(void){double v[2]={.75,0};float out[2],saved[2];
 assert(hone_driver_morph_targets(hone_morph_routes,hone_morph_route_count,v,2,hone_morph_defaults,hone_morph_count,out));
 assert(out[0]==0&&out[1]==.75f&&hone_morph_offsets[1]==2&&hone_morph_offsets[2]==2);
 memcpy(saved,out,sizeof out);v[0]=NAN;
 assert(!hone_driver_morph_targets(hone_morph_routes,hone_morph_route_count,v,2,hone_morph_defaults,hone_morph_count,out));assert(!memcmp(saved,out,sizeof out));
 hone_driver_morph_route dup[2]={{0,1},{1,1}};v[0]=1;
 assert(!hone_driver_morph_targets(dup,2,v,2,hone_morph_defaults,hone_morph_count,out));assert(!memcmp(saved,out,sizeof out));return 0;}
''',encoding='utf8')
            cmd=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(here),str(p/'test.c'),'-lm','-o',str(p/'test')]
            if os.getenv('HONE_SANITIZE')=='1':cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            subprocess.run(cmd,check=True);subprocess.run([str(p/'test')],check=True)


if __name__=='__main__':unittest.main()
