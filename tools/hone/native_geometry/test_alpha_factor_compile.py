import copy,os,pathlib,subprocess,tempfile,unittest
from alpha_factor_compile import emit


def sock(name,index,value):return dict(name=name,index=index,default=dict(status='ok',value=value),is_multi_input=False)
def node(name,kind,inputs,outputs,**kw):return dict(name=name,type=kind,inputs=inputs,outputs=outputs,mute=False,**kw)
def link(a,ai,b,bi):return dict(source_node=a,source_socket=ai,target_node=b,target_socket=bi,muted=False)


class TestAlphaFactor(unittest.TestCase):
    def fixture(self):
        nodes=[node('out','ShaderNodeOutputMaterial',[sock('Surface',0,0),sock('Volume',1,0),sock('Displacement',2,0)],[],active_output=True),
          node('mix','ShaderNodeMixShader',[sock('Fac',0,0),sock('Shader',1,0),sock('Shader',2,0)],[sock('Shader',0,0)]),
          node('clear','ShaderNodeBsdfTransparent',[sock('Color',0,[1,1,1,1])],[sock('BSDF',0,0)]),
          node('body','ShaderNodeBsdfPrincipled',[sock('Alpha',0,1.)],[sock('BSDF',0,0)]),
          node('value','ShaderNodeValue',[],[sock('Value',0,.5)]),
          node('coord','ShaderNodeTexCoord',[],[sock('Object',0,[0,0,0])],object=None,from_instancer=False),
          node('split','ShaderNodeSeparateXYZ',[sock('Vector',0,[0,0,0])],[sock('X',0,0),sock('Y',1,0),sock('Z',2,0)]),
          node('math','ShaderNodeMath',[sock('A',0,0),sock('B',1,0)],[sock('Value',0,0)],operation='MULTIPLY',use_clamp=False)]
        links=[link('mix',0,'out',0),link('clear',0,'mix',1),link('body',0,'mix',2),link('math',0,'mix',0),
               link('split',0,'math',0),link('value',0,'math',1),link('coord',0,'split',0)]
        inventory=dict(schema='hone.material-inventory.v1',runtime_ready=False,source_sha256='synthetic',materials=[dict(name='mat',nodes=nodes,links=links)])
        routes=dict(schema='hone.material-route-plan.v1',runtime_ready=False,source_sha256='synthetic',unsupported={},materials=1,
                    shader_routes=[dict(material='mat',node='value',direction='outputs',socket=0,component=0,program=0)])
        return inventory,routes
    def test_c_graph_and_failure_atomicity(self):
        inv,routes=self.fixture();header,summary=emit(inv,routes)
        self.assertEqual(summary['alpha_driver_routes'],1);self.assertEqual(summary['transparent_mix_materials'],1)
        here=pathlib.Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as tmp:
            p=pathlib.Path(tmp);(p/'alpha.h').write_text(header,encoding='utf8')
            (p/'test.c').write_text('''#include "alpha.h"
#include <assert.h>
int main(void){float xyz[3]={.5f,0,0},work[1024],out=7;double driver[1]={.4};
 assert(hone_alpha_factor(hone_alpha_programs,xyz,driver,1,work,1024,&out)&&fabs(out-.2f)<1e-6);
 driver[0]=4;assert(hone_alpha_factor(hone_alpha_programs,xyz,driver,1,work,1024,&out)&&out==1);
 driver[0]=NAN;assert(!hone_alpha_factor(hone_alpha_programs,xyz,driver,1,work,1024,&out)&&out==1);
 hone_alpha_instruction bad[]={{HONE_ALPHA_CONST,0,0,1},{HONE_ALPHA_CONST,0,0,0},{HONE_ALPHA_DIV,0,1,0}};
 hone_alpha_program p={bad,3,2};assert(!hone_alpha_factor(&p,xyz,driver,1,work,1024,&out)&&out==1);return 0;}
''',encoding='utf8')
            cmd=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(here),str(p/'test.c'),'-lm','-o',str(p/'test')]
            if os.getenv('HONE_SANITIZE')=='1':cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            subprocess.run(cmd,check=True);subprocess.run([str(p/'test')],check=True)
    def test_colored_transparency_and_cycle_reject(self):
        inv,routes=self.fixture();bad=copy.deepcopy(inv);bad['materials'][0]['nodes'][2]['inputs'][0]['default']['value']=[.5,1,1,1]
        with self.assertRaises(ValueError):emit(bad,routes)
        bad=copy.deepcopy(inv);bad['materials'][0]['links'].append(link('math',0,'math',0))
        with self.assertRaises(ValueError):emit(bad,routes)
        bad=copy.deepcopy(inv);next(n for n in bad['materials'][0]['nodes'] if n['name']=='coord')['from_instancer']=True
        with self.assertRaises(ValueError):emit(bad,routes)


if __name__=='__main__':unittest.main()
