import copy,os,pathlib,subprocess,tempfile,unittest
from base_color_compile import emit
from test_alpha_factor_compile import TestAlphaFactor,node,sock,link


class TestBaseColorCompiler(unittest.TestCase):
    def fixture(self):
        inv,routes=TestAlphaFactor().fixture();mat=inv['materials'][0]
        body=next(n for n in mat['nodes'] if n['name']=='body');body['inputs'].append(sock('Base Color',1,[.1,.2,.3,1]))
        rgb=node('rgb','ShaderNodeMixRGB',[sock('Fac',0,0),sock('Color1',1,[1,0,0,1]),sock('Color2',2,[0,0,1,1])],
                 [sock('Color',0,[0,0,0,1])],blend_type='MIX',use_clamp=False,use_alpha=False)
        ramp=node('ramp','ShaderNodeValToRGB',[sock('Fac',0,0)],[sock('Color',0,[0,0,0,1])],
            color_ramp=dict(interpolation='LINEAR',color_mode='RGB',elements=[
                dict(position=0.,color=[0,0,1,1]),dict(position=.5,color=[0,1,0,1]),dict(position=1.,color=[1,1,1,1])]))
        mat['nodes'] += [rgb,ramp];mat['links'] += [link('rgb',0,'body',1),link('value',0,'rgb',0),link('value',0,'ramp',0),link('ramp',0,'rgb',2)]
        return inv,routes
    def test_rgb_graph_and_atomic_failure(self):
        inv,routes=self.fixture();header,summary=emit(inv,routes);self.assertEqual(summary['color_driver_routes'],1)
        here=pathlib.Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as tmp:
            p=pathlib.Path(tmp);(p/'color.h').write_text(header,encoding='utf8')
            (p/'test.c').write_text('''#include "color.h"
#include <assert.h>
int main(void){float xyz[3]={0,0,0},scratch[1024],out[3]={7,7,7};double values[1]={.5};
 assert(hone_base_color(hone_base_color_programs,xyz,values,1,scratch,1024,out));
 assert(fabs(out[0]-.5)<1e-6&&fabs(out[1]-.5)<1e-6&&fabs(out[2])<1e-6);
 values[0]=NAN;assert(!hone_base_color(hone_base_color_programs,xyz,values,1,scratch,1024,out));
 assert(fabs(out[0]-.5)<1e-6&&fabs(out[1]-.5)<1e-6&&fabs(out[2])<1e-6);return 0;}
''',encoding='utf8')
            cmd=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(here),str(p/'test.c'),'-lm','-o',str(p/'test')]
            if os.getenv('HONE_SANITIZE')=='1':cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            subprocess.run(cmd,check=True);subprocess.run([str(p/'test')],check=True)
    def test_duplicate_ramp_stop_rejects(self):
        inv,routes=self.fixture();bad=copy.deepcopy(inv)
        ramp=next(n for n in bad['materials'][0]['nodes'] if n['name']=='ramp');ramp['color_ramp']['elements'][1]['position']=0
        with self.assertRaises(ValueError):emit(bad,routes)
        bad=copy.deepcopy(inv);next(n for n in bad['materials'][0]['nodes'] if n['name']=='rgb')['use_alpha']=True
        with self.assertRaises(ValueError):emit(bad,routes)


if __name__=='__main__':unittest.main()
