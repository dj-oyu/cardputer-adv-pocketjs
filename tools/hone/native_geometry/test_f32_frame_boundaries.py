"""Host contracts for opt-in frame boundary candidates; no model assets."""
import os
import pathlib
import subprocess
import tempfile
import unittest
HERE = pathlib.Path(__file__).resolve().parent

def build_library(directory):
    path=pathlib.Path(directory)
    source=path/'bridge.c'
    source.write_text(r'''#include "offset_f32_candidate.h"
#include "driver_input_f32_candidate.h"
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
EXPORT int offset_point(const float*p,const float*h,const float*n,float o,float*r){return hone_offset_f32(p,h,n,o,r);}
EXPORT int input_bundle(const double*p,size_t c,double*r){return hone_driver_input_f32(p,c,r);}
''')
    library=path/('boundaries.dll' if os.name=='nt' else 'boundaries.so')
    subprocess.run([os.getenv('CC','cc'),'-std=c11','-O2','-ffp-contract=off','-Wall','-Wextra','-Werror','-shared','-fPIC','-I'+str(HERE),str(source),'-lm','-o',str(library)],check=True)
    return library

class Contracts(unittest.TestCase):
    def test_contracts(self):
        source=r'''#include <assert.h>
#include <stdio.h>
#include "offset_f32_candidate.h"
#include "driver_input_f32_candidate.h"
static void unchanged(const float*a,const float*b){assert(!memcmp(a,b,3*sizeof(float)));}
int main(void){
 float p[3]={-1e-7f,-1e-7f,1e-7f},hit[3]={0,0,0},n[3]={0,0,1},out[3],old[3]={7,8,9};
 assert(hone_offset_f32(p,hit,n,.0025f,out));
 assert(out[0]==-0.0009999999310821295f && out[1]==out[0] && out[2]==0.001767949084751308f);
 float expected[3];memcpy(expected,out,sizeof out);
 assert(hone_offset_f32(p,hit,n,.0025f,p));unchanged(p,expected);
 float epsilon[3]={-nextafterf(FLT_EPSILON,0.f),0,0};
 assert(hone_offset_f32(epsilon,hit,n,.01f,out));unchanged(out,hit);
 epsilon[0]=-FLT_EPSILON;assert(hone_offset_f32(epsilon,hit,n,.01f,out));assert(out[2]>0.f);
 epsilon[0]=-nextafterf(FLT_EPSILON,INFINITY);assert(hone_offset_f32(epsilon,hit,n,.01f,out));assert(out[2]>0.f);
 float far[3]={0,0,1};assert(hone_offset_f32(far,hit,n,-.25f,out));assert(out[2]==-.25f);
 assert(hone_offset_f32(far,hit,n,0.f,out));unchanged(out,hit);
 memcpy(out,old,sizeof out);float invalid[3]={NAN,INFINITY,-INFINITY};
 for(unsigned j=0;j<3;j++){
  for(unsigned k=0;k<3;k++){
   float bad[3]={0,0,1};bad[k]=invalid[j];
   assert(!hone_offset_f32(bad,hit,n,.1f,out));unchanged(out,old);
   assert(!hone_offset_f32(far,bad,n,.1f,out));unchanged(out,old);
   assert(!hone_offset_f32(far,hit,bad,.1f,out));unchanged(out,old);
  }
  assert(!hone_offset_f32(far,hit,n,invalid[j],out));unchanged(out,old);
 }
 float zero[3]={0,0,0},huge[3]={FLT_MAX,0,0};
 assert(!hone_offset_f32(far,hit,zero,.1f,out));unchanged(out,old);
 assert(!hone_offset_f32(huge,hit,n,.1f,out));unchanged(out,old);
 assert(!hone_offset_f32(NULL,hit,n,.1f,out));unchanged(out,old);
 assert(!hone_offset_f32(far,NULL,n,.1f,out));unchanged(out,old);
 assert(!hone_offset_f32(far,hit,NULL,.1f,out));unchanged(out,old);
 assert(!hone_offset_f32(far,hit,n,.1f,NULL));
 double inputs[5]={1.+0x1p-24,1.+3.*0x1p-24,-0.,0x1p-149,FLT_MAX},ready[5];
 assert(hone_driver_input_f32(inputs,5,ready));assert(ready[0]==1.&&ready[1]==1.+0x1p-22);assert(signbit(ready[2]));assert(ready[3]==0x1p-149&&ready[4]==FLT_MAX);
 assert(hone_driver_input_f32(inputs,5,inputs));assert(!memcmp(inputs,ready,sizeof inputs));
 double saved[5];memcpy(saved,ready,sizeof ready);inputs[4]=DBL_MAX;
 assert(!hone_driver_input_f32(inputs,5,ready));assert(!memcmp(saved,ready,sizeof ready));
 inputs[4]=NAN;assert(!hone_driver_input_f32(inputs,5,ready));assert(!memcmp(saved,ready,sizeof ready));
 assert(!hone_driver_input_f32(inputs,4,inputs+1));assert(!hone_driver_input_f32(inputs+1,4,inputs));
 assert(!hone_driver_input_f32(inputs,SIZE_MAX,ready));assert(!hone_driver_input_f32(NULL,1,ready));assert(!hone_driver_input_f32(inputs,1,NULL));assert(hone_driver_input_f32(NULL,0,NULL));
 puts("F32_FRAME_BOUNDARY_CONTRACT PASS");return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path=pathlib.Path(directory);cfile=path/'contract.c';cfile.write_text(source)
            for optimization in ('-O0','-O2'):
                exe=path/('contract.exe' if os.name=='nt' else 'contract')
                cmd=[os.getenv('CC','cc'),'-std=c11',optimization,'-ffp-contract=off','-Wall','-Wextra','-Werror','-I'+str(HERE),str(cfile),'-lm','-o',str(exe)]
                if os.getenv('HONE_SANITIZE')=='1':cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
                subprocess.run(cmd,check=True);subprocess.run([str(exe)],check=True)

if __name__=='__main__':unittest.main()
