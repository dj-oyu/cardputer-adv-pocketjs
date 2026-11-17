"""Synthetic contracts for the opt-in affine arithmetic candidate."""
import os
import pathlib
import subprocess
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent


def build_library(directory):
    directory = pathlib.Path(directory)
    source = directory / "affine_bridge.c"
    source.write_text(r'''#include "affine_f32_candidate.h"
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
EXPORT int relative(const float*t,const float*s,float*out){return hone_affine_f32_relative(t,s,out);}
EXPORT int point(const float*m,const float*p,float*out){return hone_affine_f32_point(m,p,out);}
''', encoding="utf8")
    library = directory / ("affine.dll" if os.name == "nt" else "affine.so")
    subprocess.run([os.getenv("CC", "cc"), "-std=c11", "-O2", "-ffp-contract=off",
                    "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-I"+str(HERE),
                    str(source), "-lm", "-o", str(library)], check=True)
    return library


class AffineContract(unittest.TestCase):
    def test_atomic_failure_alias_and_admission(self):
        source = r'''#include "affine_f32_candidate.h"
#include <assert.h>
static const float identity[12]={1,0,0,0,0,1,0,0,0,0,1,0};
static void unchanged(const float *a,const float *b,size_t n){assert(!memcmp(a,b,n*sizeof(float)));}
int main(void){
 float out[24],saved[24],target[12],source[12];
 for(unsigned i=0;i<24;i++)out[i]=saved[i]=100+(float)i;
 assert(hone_affine_f32_relative(identity,identity,out));
 for(unsigned i=0;i<24;i++)assert(out[i]==identity[i%12]);
 const float turn[12]={0,-1,0,4,1,0,0,-2,0,0,1,3};
 assert(hone_affine_f32_relative(identity,turn,out));
 const float point[3]={1,2,3};float moved[3],back[3];
 assert(hone_affine_f32_point(out,point,moved));assert(moved[0]==2&&moved[1]==-1&&moved[2]==6);
 assert(hone_affine_f32_point(out+12,moved,back));unchanged(point,back,3);
 /* Exact and partial overlap are supported without reinterpreting array types. */
 float alias[40];memcpy(alias,turn,sizeof turn);
 assert(hone_affine_f32_relative(identity,alias,alias));unchanged(alias,out,24);
 memcpy(alias+4,turn,sizeof turn);assert(hone_affine_f32_relative(identity,alias+4,alias));unchanged(alias,out,24);
 memcpy(moved,point,sizeof point);assert(hone_affine_f32_point(turn,moved,moved));
 assert(moved[0]==2&&moved[1]==-1&&moved[2]==6);
 memcpy(saved,out,sizeof out);
 const float invalid[3]={NAN,INFINITY,-INFINITY};
 for(unsigned v=0;v<3;v++)for(unsigned k=0;k<12;k++){
  memcpy(source,identity,sizeof source);source[k]=invalid[v];
  assert(!hone_affine_f32_relative(identity,source,out));unchanged(out,saved,24);
  assert(!hone_affine_f32_relative(source,identity,out));unchanged(out,saved,24);
 }
 for(unsigned kind=0;kind<5;kind++){
  memcpy(source,identity,sizeof source);
  if(kind==0)source[0]=2;       /* scale */
  if(kind==1)source[1]=.1f;     /* shear */
  if(kind==2)source[0]=-1;      /* reflection */
  if(kind==3)source[0]=0;       /* singular */
  if(kind==4){source[0]=2;source[5]=2;source[10]=2;}
  assert(!hone_affine_f32_relative(source,source,out));unchanged(out,saved,24);
 }
 assert(!hone_affine_f32_relative(NULL,identity,out));unchanged(out,saved,24);
 assert(!hone_affine_f32_relative(identity,NULL,out));unchanged(out,saved,24);
 assert(!hone_affine_f32_relative(identity,identity,NULL));
 memcpy(target,identity,sizeof target);memcpy(source,identity,sizeof source);
 target[3]=-FLT_MAX;source[3]=FLT_MAX;
 assert(!hone_affine_f32_relative(target,source,out));unchanged(out,saved,24);
 /* Reverse inversion overflows even though the supplied rigid matrix and
  * its forward translation are finite. No half-ready forward result leaks. */
 const float diagonal[12]={.70710677f,-.70710677f,0,FLT_MAX,
                          .70710677f,.70710677f,0,FLT_MAX,0,0,1,0};
 assert(hone_affine_f32_rigid(diagonal));
 assert(!hone_affine_f32_relative(identity,diagonal,out));unchanged(out,saved,24);
 const float half_turn[12]={-1,0,0,0,0,-1,0,0,0,0,1,0};
 assert(hone_affine_f32_relative(identity,half_turn,out));
 memcpy(out,saved,sizeof out);
 const float maximum[3]={FLT_MAX,-FLT_MAX,FLT_MIN};float maximum_out[3];
 assert(hone_affine_f32_point(identity,maximum,maximum_out));unchanged(maximum,maximum_out,3);
 float dst[3]={11,22,33},old[3]={11,22,33},bad[3]={NAN,0,0};
 for(unsigned v=0;v<3;v++)for(unsigned k=0;k<3;k++){
  float bad_point[3]={0,0,0};bad_point[k]=invalid[v];
  assert(!hone_affine_f32_point(identity,bad_point,dst));unchanged(dst,old,3);
 }
 assert(!hone_affine_f32_point(identity,bad,dst));unchanged(dst,old,3);
 memcpy(source,identity,sizeof source);source[3]=FLT_MAX;bad[0]=FLT_MAX;
 assert(!hone_affine_f32_point(source,bad,dst));unchanged(dst,old,3);
 assert(!hone_affine_f32_point(NULL,point,dst));unchanged(dst,old,3);
 assert(!hone_affine_f32_point(identity,NULL,dst));unchanged(dst,old,3);
 assert(!hone_affine_f32_point(identity,point,NULL));
 /* Same non-rigid parent cancellation is deliberately NOT an admitted case. */
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            cfile = path / "contract.c"
            cfile.write_text(source, encoding="utf8")
            for optimization in ("-O0", "-O2"):
                executable = path / ("contract.exe" if os.name == "nt" else "contract")
                command = [os.getenv("CC", "cc"), "-std=c11", optimization, "-ffp-contract=off",
                           "-Wall", "-Wextra", "-Werror", "-I"+str(HERE), str(cfile),
                           "-lm", "-o", str(executable)]
                if os.getenv("HONE_SANITIZE") == "1":
                    command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                subprocess.run(command, check=True)
                subprocess.run([str(executable)], check=True)
            rejected = subprocess.run([os.getenv("CC", "cc"), "-std=c11", "-ffast-math",
                                       "-I"+str(HERE), "-c", str(cfile), "-o", str(path/"bad.o")],
                                      capture_output=True, text=True)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("requires finite checks", rejected.stderr)


if __name__ == "__main__":
    unittest.main()
