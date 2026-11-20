import os,pathlib,subprocess,tempfile,unittest


class TestRootPalette(unittest.TestCase):
    def test_root_conjugation_and_atomic_failure(self):
        here=pathlib.Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as tmp:
            p=pathlib.Path(tmp);(p/'test.c').write_text('''#include "rig_root_palette_candidate.h"
#include <assert.h>
int main(void){
 hone_root_bone bones[2]={0};for(unsigned j=0;j<2;j++)bones[j].rest_q[0]=1;
 bones[0].rest_t[0]=1;bones[0].rotation[2]=1.5707963267948966;
 bones[1].mode=HONE_BONE_QUATERNION;bones[1].rotation[0]=1;
 double scratch[14],ready[14],saved[14];
 assert(hone_root_palette(bones,2,scratch,ready));
 assert(fabs(ready[0]-.7071067811865476)<1e-12&&fabs(ready[3]-.7071067811865476)<1e-12);
 assert(fabs(ready[4]-1)<1e-12&&fabs(ready[5]+1)<1e-12&&fabs(ready[6])<1e-12);
 assert(ready[7]==1);memcpy(saved,ready,sizeof ready);bones[1].rotation[0]=0;
 assert(!hone_root_palette(bones,2,scratch,ready));assert(!memcmp(saved,ready,sizeof ready));
 bones[1].rotation[0]=1;bones[0].location[0]=NAN;
 assert(!hone_root_palette(bones,2,scratch,ready));assert(!memcmp(saved,ready,sizeof ready));
 bones[0].location[0]=0;hone_root_bone work[2];hone_root_driver_route routes[1]={{0,0,2}};double values[1]={0};
 assert(hone_root_driver_palette(bones,2,routes,1,values,1,work,scratch,ready));assert(fabs(ready[4])<1e-12&&fabs(ready[5])<1e-12);
 memcpy(saved,ready,sizeof ready);values[0]=NAN;
 assert(!hone_root_driver_palette(bones,2,routes,1,values,1,work,scratch,ready));assert(!memcmp(saved,ready,sizeof ready));return 0;}
''',encoding='utf8')
            cmd=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(here),str(p/'test.c'),'-lm','-o',str(p/'test')]
            if os.getenv('HONE_SANITIZE')=='1':cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            subprocess.run(cmd,check=True);subprocess.run([str(p/'test')],check=True)


if __name__=='__main__':unittest.main()
