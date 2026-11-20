import os,pathlib,subprocess,tempfile,unittest


class TestAlphaGraphFragment(unittest.TestCase):
    def test_orthographic_local_coordinate_and_atomic_failure(self):
        here=pathlib.Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as tmp:
            p=pathlib.Path(tmp);(p/'test.c').write_text('''#include "alpha_graph_fragment_candidate.h"
#include <assert.h>
int main(void){
 const hone_alpha_instruction alpha_code[]={{HONE_ALPHA_X,0,0,0}};
 const hone_alpha_instruction red[]={{HONE_ALPHA_Z,0,0,0}};
 const hone_alpha_instruction green[]={{HONE_ALPHA_CONST,0,0,.25f}};
 const hone_alpha_instruction blue[]={{HONE_ALPHA_DRIVER,0,0,0}};
 const hone_alpha_program alpha={alpha_code,1,0},colors[3]={{red,1,0},{green,1,0},{blue,1,0}};
 hone_graph_triangle t={{{0,0,1,{0,0,0}},{4,0,1,{1,0,0}},{0,4,1,{0,0,1}}},&alpha,colors};
 float scratch[8];double driver[1]={.75};hone_graph_fragment out={9,9,{9,9,9}},saved;
 assert(hone_graph_fragment_at(&t,1,1,0,driver,1,scratch,8,&out)==1);
 assert(fabs(out.alpha-.25f)<1e-6&&fabs(out.rgb[0]-.25f)<1e-6&&out.rgb[1]==.25f&&out.rgb[2]==.75f);
 saved=out;assert(hone_graph_fragment_at(&t,9,9,0,driver,1,scratch,8,&out)==0&&!memcmp(&out,&saved,sizeof out));
 assert(hone_graph_fragment_at(&t,1,1,2,driver,1,scratch,8,&out)==0&&!memcmp(&out,&saved,sizeof out));
 driver[0]=NAN;assert(hone_graph_fragment_at(&t,1,1,0,driver,1,scratch,8,&out)==-1&&!memcmp(&out,&saved,sizeof out));return 0;}
''',encoding='utf8')
            cmd=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(here),str(p/'test.c'),'-lm','-o',str(p/'test')]
            if os.getenv('HONE_SANITIZE')=='1':cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            subprocess.run(cmd,check=True);subprocess.run([str(p/'test')],check=True)


if __name__=='__main__':unittest.main()
