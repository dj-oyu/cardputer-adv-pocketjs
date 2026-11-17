"""Synthetic rigid-coordinate wrap test; no firmware or performance claim."""
import os,pathlib,subprocess,tempfile,unittest


class TestNativeWrap(unittest.TestCase):
    def test_rigid_transform_offset_and_failures(self):
        here=pathlib.Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as tmp:
            p=pathlib.Path(tmp);(p/'test.c').write_text('''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
int hone_native_wrap(const float*,size_t,const float*,size_t,const uint32_t*,size_t,const double*,double,double*,size_t,double*,size_t,double*,size_t,uint32_t*,size_t,void*,size_t,float*,size_t,size_t*);
int main(void){
 float source[3]={.25f,.25f,1},target[9]={0,0,0,1,0,0,0,1,0},out[3]={7,7,7},saved[3];
 uint32_t tri[3]={0,1,2},face[1];double pts[3],verts[9],hits[3],m[12]={1,0,0,0,0,1,0,0,0,0,1,0};
 void *boxes=malloc(24);size_t tests=0;assert(boxes);
 assert(hone_native_wrap(source,1,target,3,tri,1,m,.1,pts,3,verts,9,hits,3,face,1,boxes,1,out,3,&tests));
 assert(fabs(out[0]-.25)<1e-6&&fabs(out[1]-.25)<1e-6&&fabs(out[2]-.1)<1e-6&&tests==1);
 m[11]=1;assert(hone_native_wrap(source,1,target,3,tri,1,m,.1,pts,3,verts,9,hits,3,face,1,boxes,1,out,3,&tests));
 assert(fabs(out[2]+.9)<1e-6);memcpy(saved,out,sizeof saved);
 m[0]=2;assert(!hone_native_wrap(source,1,target,3,tri,1,m,.1,pts,3,verts,9,hits,3,face,1,boxes,1,out,3,&tests));assert(!memcmp(saved,out,sizeof out));
 m[0]=1;assert(!hone_native_wrap(source,1,target,3,tri,1,m,.1,pts,2,verts,9,hits,3,face,1,boxes,1,out,3,&tests));assert(!memcmp(saved,out,sizeof out));
 assert(!hone_native_wrap(source,SIZE_MAX/(3*sizeof(double))+1,target,3,tri,1,m,.1,pts,SIZE_MAX,verts,9,hits,SIZE_MAX,face,SIZE_MAX,boxes,1,out,SIZE_MAX,&tests));assert(!memcmp(saved,out,sizeof out));
 free(boxes);return 0;}
''',encoding='utf8')
            cmd=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                 '-ffp-contract=off','-fno-fast-math',str(here/'native_wrap_candidate.c'),str(p/'test.c'),'-lm','-o',str(p/'test')]
            if os.getenv('HONE_SANITIZE')=='1':cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            subprocess.run(cmd,check=True);subprocess.run([str(p/'test')],check=True)


if __name__=='__main__':unittest.main()
