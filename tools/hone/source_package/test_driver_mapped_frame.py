"""Mapped-driver differential/stack experiment. Public synthetic inputs only.

Run with --out <new-build-dir> [--opt 0|2] [--sanitize]. Complete immutable
input/code/slot spans and disjoint ready/scratch are caller preconditions.
No binding resolver, publication owner or firmware caller is added here.

MSYS2 GCC 15.2.0 x64 O0/O2: 15,007 cases pass, including 32/33 stack depth,
uint16 maximum index, duplicate mappings, unused NaN, invalid unused slot,
short-circuit expressions, overflow and malformed instructions. Successful
and failed scratch/ready bytes match the original; late failure keeps ready.

-fstack-usage O0: original frame 624 B vs mapped frame 112 B; both VM calls
368 B, both wrappers 80 B. O2: original frame 896 B and wrapper 8 B; mapped
wrapper 400 B (inlined VM). These are compiler per-function reports, not a
measured stack high-water or a sum across tail calls. Libm stack is excluded.
The 512-byte local input copy is removed; no target stack/FPS or speed claim.
"""
import argparse, os, pathlib, subprocess
from driver_bytecode import compile_expression
p=argparse.ArgumentParser();p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--opt',choices=['0','2'],default='2');p.add_argument('--sanitize',action='store_true');a=p.parse_args()
here=pathlib.Path(__file__).resolve().parent;out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
expressions=['a+b','max(0,min(1,a+b))','1 if a==1 else b','max(a,b)>=0.75','a**2+b**3-a**4','-a if a>=b else +b','(a+b) if (c if a else b) else (a**4 if b else c)','1 if a else b','max(a,min(b,c))']
arrays=[]
for i,e in enumerate(expressions):
 code=compile_expression(e,['a','b','c'])
 rows=','.join('{%d,%d,%s}'%(op,arg,float(v).hex()) for op,arg,v in code)
 arrays.append('static const hone_driver_instruction code%d[]={%s};'%(i,rows))
arrays.append('static const hone_driver_program valid[]={'+','.join('{code%d,sizeof code%d/sizeof code%d[0],64,64}'%(i,i,i) for i in range(len(expressions)))+'};')
source=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "driver_mapped_frame_candidate.h"
#define ARGS const hone_driver_program*p,size_t n,const uint16_t*s,size_t ns,const double*v,size_t nv,double*w,double*r
__attribute__((noinline)) int baseline(ARGS){return hone_driver_frame(p,n,s,ns,v,nv,w,r);}
__attribute__((noinline)) int mapped(ARGS){return hone_driver_mapped_frame(p,n,s,ns,v,nv,w,r);}
'''+ '\n'.join(arrays)+r'''
static uint32_t seed=0x41a769u;
static uint32_t next(void){seed=1664525u*seed+1013904223u;return seed;}
static unsigned successes,failures;
static void compare(hone_driver_program *programs,const uint16_t*slots,size_t ns,const double*inputs,size_t ni){
 double aw[2]={42,43},bw[2]={42,43},ar[2]={44,45},br[2]={44,45};
 int a=baseline(programs,2,slots,ns,inputs,ni,aw,ar),b=mapped(programs,2,slots,ns,inputs,ni,bw,br);
 assert(a==b&&!memcmp(aw,bw,sizeof aw)&&!memcmp(ar,br,sizeof ar));
 if(a)successes++;else{failures++;assert(ar[0]==44&&ar[1]==45);}
}
int main(void){
 static const hone_driver_instruction first[]={{1,0,7},{14,0,0}};
 hone_driver_program pair[2]={{first,2,0,0},{0}};
 uint16_t slots[128];double inputs[8];
 for(unsigned trial=0;trial<12000;trial++){
  for(unsigned j=0;j<128;j++)slots[j]=(uint16_t)(next()%8);
  for(unsigned j=0;j<8;j++)inputs[j]=((int)(next()%4097)-2048)/1024.;
  if(trial%7==0)inputs[next()%8]=NAN;
  if(trial%11==0)inputs[next()%8]=INFINITY;
  if(trial%13==0)inputs[next()%8]=1e308;
  if(trial%17==0)slots[127]=8; /* invalid unused slot must still fail */
  pair[1]=valid[trial%(sizeof valid/sizeof *valid)];compare(pair,slots,128,inputs,8);
 }
 /* Unused NaN is legal; full 64-variable map, final slot and highest input. */
 for(unsigned j=0;j<128;j++)slots[j]=0;
 inputs[0]=NAN;inputs[7]=-0.;slots[127]=7;
 const hone_driver_instruction final_slot[]={{2,63,0},{14,0,0}};
 pair[1]=(hone_driver_program){final_slot,2,64,64};compare(pair,slots,128,inputs,8);
 pair[1].first=UINT32_MAX;compare(pair,slots,128,inputs,8);
 pair[1].first=65;compare(pair,slots,128,inputs,8);
 pair[1].first=64;pair[1].variables=65;compare(pair,slots,128,inputs,8);
 /* Bounded malformed bytecode exercises underflow, stack bound, branches,
  * invalid opcodes and missing END against the original rejecting VM. */
 for(unsigned trial=0;trial<3000;trial++){
  hone_driver_instruction code[64];
  for(unsigned j=0;j<64;j++)code[j]=(hone_driver_instruction){(uint8_t)(next()%17),(uint16_t)(next()%70),(double)(next()%5)};
  pair[1]=(hone_driver_program){code,64,64,64};compare(pair,slots,128,inputs,8);
 }
 /* Exact fixed-stack boundary and one extra push; second-program failure
  * still keeps ready intact after the first program produced scratch. */
 for(unsigned depth=32;depth<=33;depth++){
  hone_driver_instruction code[66];unsigned at=0;
  for(unsigned j=0;j<depth;j++)code[at++]=(hone_driver_instruction){1,0,1};
  for(unsigned j=1;j<depth;j++)code[at++]=(hone_driver_instruction){4,0,0};
  code[at++]=(hone_driver_instruction){14,0,0};
  pair[1]=(hone_driver_program){code,at,0,0};compare(pair,slots,128,inputs,8);
 }
 /* Valid uint16 maximum input index; counts remain size_t. */
 static double many_inputs[65536];many_inputs[65535]=-0.;slots[127]=65535;
 pair[1]=(hone_driver_program){final_slot,2,64,64};compare(pair,slots,128,many_inputs,65536);
 slots[127]=0;
 assert(successes&&failures);
 printf("PASS mapped frame cases=%u successes=%u failures=%u bitwise scratch/ready parity; input copy removed=512 bytes\n",successes+failures,successes,failures);
 return 0;
}
'''
(out/'test.c').write_text(source)
binary=out/('test.exe' if os.name=='nt' else 'test')
cmd=[os.environ.get('CC','cc'),'-std=c11','-O'+a.opt,'-g','-Wall','-Wextra','-Werror','-fstack-usage','-ffp-contract=off','-fno-fast-math','-I'+str(here),str(out/'test.c'),'-lm','-o',str(binary)]
if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
subprocess.run(cmd,check=True);subprocess.run([str(binary)],check=True)
for f in out.glob('*.su'):
 for line in f.read_text().splitlines():
  if any(name in line for name in ('hone_driver_','baseline','mapped')):print(line)
