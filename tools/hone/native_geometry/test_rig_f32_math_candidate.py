"""Host-only numerical/ownership contracts for opt-in rig arithmetic."""
import os
import pathlib
import subprocess
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent


def build_library(directory):
    directory = pathlib.Path(directory)
    source = directory / "rig_bridge.c"
    source.write_text(r'''#include "rig_f32_math_candidate.h"
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
EXPORT int euler(const float*a,float*out){return hone_rig_f32_euler_xyz(a,out);}
EXPORT int quaternion(const float*m,float*out){return hone_rig_f32_matrix_quat_positive_trace(m,out);}
EXPORT int position(const float*p,const float*j,size_t n,const uint32_t*i,const float*w,size_t k,float*out){return hone_dq_f32_position(p,j,n,i,w,k,out);}
''', encoding="utf8")
    library = directory / ("rig.dll" if os.name == "nt" else "rig.so")
    subprocess.run([os.getenv("CC", "cc"), "-std=c11", "-O2", "-ffp-contract=off", "-shared", "-fPIC",
                    "-Wall", "-Wextra", "-Werror", "-I"+str(HERE), str(source), "-lm", "-o", str(library)], check=True)
    return library


class RigMathContract(unittest.TestCase):
    def test_numerical_envelope_atomic_failure_and_alias(self):
        source = r'''#include "rig_f32_math_candidate.h"
#include <assert.h>
#include <stdio.h>
int rigid_dq_position(const double*,const double*,size_t,const unsigned*,const double*,size_t,double*);
static uint32_t state=917;
static float random_unit(void){state=state*1664525u+1013904223u;return (float)(state>>8)/16777216.0f;}
static void same(const float*a,const float*b,size_t n){assert(!memcmp(a,b,n*sizeof(float)));}
int main(void){
 const float identity[9]={1,0,0,0,1,0,0,0,1};
 float matrix[9],saved_matrix[9];for(unsigned k=0;k<9;k++)matrix[k]=saved_matrix[k]=20+(float)k;
 const float zero[3]={0,0,0};assert(hone_rig_f32_euler_xyz(zero,matrix));for(unsigned k=0;k<9;k++)assert(matrix[k]==identity[k]);
 float q[4]={11,22,33,44},saved_q[4]={11,22,33,44};
 assert(hone_rig_f32_matrix_quat_positive_trace(identity,q));assert(q[0]==1&&q[1]==0&&q[2]==0&&q[3]==0);
 float alias[9]={.1f,-.2f,.3f};float expected[9];assert(hone_rig_f32_euler_xyz(alias,expected));
 assert(hone_rig_f32_euler_xyz(alias,alias));same(alias,expected,9);
 float expected_q[4];assert(hone_rig_f32_matrix_quat_positive_trace(alias,expected_q));
 assert(hone_rig_f32_matrix_quat_positive_trace(alias,alias));same(alias,expected_q,4);
 const float invalid[3]={NAN,INFINITY,-INFINITY};
 for(unsigned v=0;v<3;v++)for(unsigned k=0;k<3;k++){
  float angles[3]={0,0,0};angles[k]=invalid[v];memcpy(matrix,saved_matrix,sizeof matrix);
  assert(!hone_rig_f32_euler_xyz(angles,matrix));same(matrix,saved_matrix,9);
 }
 for(unsigned v=0;v<3;v++)for(unsigned k=0;k<9;k++){
  float bad[9];memcpy(bad,identity,sizeof bad);bad[k]=invalid[v];memcpy(q,saved_q,sizeof q);
  assert(!hone_rig_f32_matrix_quat_positive_trace(bad,q));same(q,saved_q,4);
 }
 const float half_turn[9]={-1,0,0,0,-1,0,0,0,1};
 const float trace_zero[9]={0,0,1,1,0,0,0,1,0};
 assert(!hone_rig_f32_matrix_quat_positive_trace(half_turn,q));same(q,saved_q,4);
 assert(!hone_rig_f32_matrix_quat_positive_trace(trace_zero,q));same(q,saved_q,4);
 const float near_boundary[3]={0,0,1.5f};assert(hone_rig_f32_euler_xyz(near_boundary,matrix));
 assert(hone_rig_f32_matrix_quat_positive_trace(matrix,q));
 memcpy(matrix,identity,sizeof matrix);matrix[0]=2;memcpy(q,saved_q,sizeof q);
 assert(!hone_rig_f32_matrix_quat_positive_trace(matrix,q));same(q,saved_q,4);
 memcpy(matrix,identity,sizeof matrix);matrix[0]=-1;
 assert(!hone_rig_f32_matrix_quat_positive_trace(matrix,q));same(q,saved_q,4);
 assert(!hone_rig_f32_euler_xyz(NULL,matrix));assert(!hone_rig_f32_euler_xyz(zero,NULL));
 assert(!hone_rig_f32_matrix_quat_positive_trace(NULL,q));assert(!hone_rig_f32_matrix_quat_positive_trace(identity,NULL));
 const float point[3]={1,2,3};float joints[14]={1,0,0,0,1,2,3,-1,0,0,0,1,2,3};
 uint32_t indices[4]={0,1,0,1};float weights[4]={.25f,.75f,0,0};float result[3],old[3]={21,22,23};
 assert(hone_dq_f32_position(point,joints,2,indices,weights,2,result));assert(result[0]==2&&result[1]==4&&result[2]==6);
 memcpy(result,point,sizeof result);assert(hone_dq_f32_position(result,joints,2,indices,weights,2,result));assert(result[2]==6);
 float joint_alias[14];memcpy(joint_alias,joints,sizeof joints);assert(hone_dq_f32_position(point,joint_alias,2,indices,weights,2,joint_alias));assert(joint_alias[2]==6);
 assert(hone_dq_f32_position(point,joints,2,indices,weights,0,result));same(point,result,3);
 weights[0]=weights[1]=0;assert(hone_dq_f32_position(point,joints,2,indices,weights,2,result));same(point,result,3);
 memcpy(result,old,sizeof result);weights[0]=FLT_MIN;
 assert(!hone_dq_f32_position(point,joints,2,indices,weights,1,result));same(result,old,3);
 weights[0]=FLT_MAX;assert(!hone_dq_f32_position(point,joints,2,indices,weights,1,result));same(result,old,3);
 weights[0]=-1;assert(!hone_dq_f32_position(point,joints,2,indices,weights,1,result));same(result,old,3);
 weights[0]=1;indices[0]=2;assert(!hone_dq_f32_position(point,joints,2,indices,weights,1,result));same(result,old,3);indices[0]=0;
 for(unsigned v=0;v<3;v++){
  weights[0]=invalid[v];assert(!hone_dq_f32_position(point,joints,2,indices,weights,1,result));same(result,old,3);weights[0]=1;
  for(unsigned k=0;k<7;k++){float bad[14];memcpy(bad,joints,sizeof bad);bad[k]=invalid[v];assert(!hone_dq_f32_position(point,bad,2,indices,weights,1,result));same(result,old,3);}
  for(unsigned k=0;k<3;k++){float bad[3]={0,0,0};bad[k]=invalid[v];assert(!hone_dq_f32_position(bad,joints,2,indices,weights,1,result));same(result,old,3);}
 }
 joints[0]=0;assert(!hone_dq_f32_position(point,joints,2,indices,weights,1,result));same(result,old,3);joints[0]=1;
 assert(!hone_dq_f32_position(point,joints,2,indices,weights,5,result));same(result,old,3);
 assert(!hone_dq_f32_position(point,joints,65537,indices,weights,1,result));same(result,old,3);
 assert(!hone_dq_f32_position(NULL,joints,2,indices,weights,1,result));same(result,old,3);
 assert(!hone_dq_f32_position(point,NULL,2,indices,weights,1,result));same(result,old,3);
 assert(!hone_dq_f32_position(point,joints,2,NULL,weights,1,result));same(result,old,3);
 assert(!hone_dq_f32_position(point,joints,2,indices,NULL,1,result));same(result,old,3);
 assert(!hone_dq_f32_position(point,joints,2,indices,weights,1,NULL));
 /* Independent existing double consumer supplies a numerical envelope, not
  * a substitute for the Blender full-frame position/normal admission gate. */
 double maximum=0;
 for(unsigned trial=0;trial<4000;trial++){
  float p[3],palette[14],w[2];double dp[3],dj[14],dw[2],reference[3];unsigned ix[2]={0,1};
  for(unsigned k=0;k<3;k++)dp[k]=p[k]=6*random_unit()-3;
  for(unsigned bone=0;bone<2;bone++){
   double a[4],norm=0;for(unsigned k=0;k<4;k++){a[k]=2*random_unit()-1;norm+=a[k]*a[k];}norm=sqrt(norm);
   for(unsigned k=0;k<4;k++)dj[7*bone+k]=palette[7*bone+k]=(float)(a[k]/norm);
   for(unsigned k=4;k<7;k++)dj[7*bone+k]=palette[7*bone+k]=2*random_unit()-1;
  }
  for(unsigned k=0;k<2;k++)dw[k]=w[k]=.01f+random_unit();
  assert(hone_dq_f32_position(p,palette,2,indices,w,2,result));
  assert(rigid_dq_position(dp,dj,2,ix,dw,2,reference));
  for(unsigned k=0;k<3;k++){double error=fabs(result[k]-reference[k]);if(error>maximum)maximum=error;assert(error<4e-6);}
 }
 printf("RIG_F32_CONTRACT PASS random=4000 max_component_error=%.9g\n",maximum);return 0;
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
                           str(HERE.parent/"source_package"/"rigid_dq_candidate.c"), "-lm", "-o", str(executable)]
                if os.getenv("HONE_SANITIZE") == "1":
                    command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                subprocess.run(command, check=True)
                subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
