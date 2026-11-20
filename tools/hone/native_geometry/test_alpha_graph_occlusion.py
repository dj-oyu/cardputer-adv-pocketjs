"""Unlimited-fragment reference versus the explicit opaque-occlusion candidate."""
import argparse,os,pathlib,subprocess
p=argparse.ArgumentParser();p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[3];out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
c=out/'test.c';c.write_text(r'''
#include "tools/hone/native_geometry/alpha_graph_occlusion_candidate.h"
#include <assert.h>
#include <stdio.h>
static uint32_t state=17;
static unsigned random_u(void){state=state*1664525u+1013904223u;return state;}
static int reference(const hone_graph_snapshot*s,unsigned x,unsigned y,float depth,float*scratch,hone_graph_ordered*out){
 int n=0;for(size_t j=0;j<s->count;j++){
  hone_graph_fragment f;int hit=hone_graph_fragment_at(&s->packets[j].triangle,x,y,depth,s->drivers,s->driver_count,scratch,16,&f);
  if(hit<0)return -1;if(!hit)continue;assert(n<32);int i=n++;
  hone_graph_ordered q={f,s->packets[j].order};
  while(i&&(out[i-1].fragment.z>f.z||(out[i-1].fragment.z==f.z&&out[i-1].order<=q.order))){out[i]=out[i-1];i--;}out[i]=q;
 }return n;
}
static void blend(hone_graph_ordered*f,int n,float rgb[3]){
 rgb[0]=.2f;rgb[1]=.5f;rgb[2]=.7f;
 for(int i=0;i<n;i++)for(unsigned k=0;k<3;k++)rgb[k]=f[i].fragment.rgb[k]*f[i].fragment.alpha+rgb[k]*(1-f[i].fragment.alpha);
}
int main(void){
 hone_graph_packet packets[32];hone_alpha_instruction code[32][4];hone_alpha_program program[32][4];
 hone_graph_snapshot s={.packets=packets,.capacity=32,.ready=1};float scratch[16];unsigned pixels=0,overflows=0;
 for(unsigned trial=0;trial<4000;trial++){
  s.count=1+random_u()%24;
  for(size_t j=0;j<s.count;j++){
   const float alpha[]={0,.2f,.8f,1,nextafterf(1,0)};
   for(unsigned k=0;k<4;k++){code[j][k]=(hone_alpha_instruction){.op=HONE_ALPHA_CONST,.value=k?(random_u()%17)/16.f:alpha[random_u()%5]};program[j][k]=(hone_alpha_program){&code[j][k],1,0};}
   float z=(float)(random_u()%7),shift=(float)(random_u()%3);
   packets[j]=(hone_graph_packet){.triangle={.v={{-1+shift,-1,z,{0,0,0}},{5+shift,-1,z,{1,0,0}},{-1+shift,5,z,{0,1,0}}},.alpha=&program[j][0],.color=&program[j][1]},.order=random_u()%4};
  }
  for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++){
   float depth=trial%3?-INFINITY:2;hone_graph_ordered full[32],kept[4],before[4];memset(kept,0xa5,sizeof kept);memcpy(before,kept,sizeof kept);
   int n=reference(&s,x,y,depth,scratch,full);assert(n>=0);int needed=n;for(int i=0;i<n;i++)if(full[i].fragment.alpha==1)needed=n-i;
   int k=hone_graph_fragments_occlusion(&s,x,y,depth,scratch,16,kept);
   if(needed>4){assert(k==-1&&!memcmp(before,kept,sizeof kept));overflows++;}
   else{assert(k==needed);float a[3],b[3];blend(full,n,a);blend(kept,k,b);assert(!memcmp(a,b,sizeof a));pixels++;}
  }
 }
 /* An invalid fragment behind exact opaque coverage MUST still fail. */
 s.count=2;for(unsigned j=0;j<2;j++){packets[j]=packets[0];for(unsigned k=0;k<3;k++){packets[j].triangle.v[k].x=k==1?5:-1;packets[j].triangle.v[k].y=k==2?5:-1;packets[j].triangle.v[k].z=j?0:3;}packets[j].triangle.alpha=&program[j][0];packets[j].triangle.color=&program[j][1];code[j][0].value=1;}
 code[1][1].value=NAN;hone_graph_ordered q[4],before[4];memset(q,0x55,sizeof q);memcpy(before,q,sizeof q);
 assert(hone_graph_fragments_occlusion(&s,0,0,-INFINITY,scratch,16,q)==-1&&!memcmp(before,q,sizeof q));
 code[1][1].value=.3f;assert(hone_graph_fragments_occlusion(&s,0,0,-INFINITY,scratch,16,q)==1);
 printf("OCCLUSION PASS trials=4000 equal_pixels=%u retained_overflows=%u ties/near_one/late_invalid/rollback\n",pixels,overflows);
}
''')
for opt in ('0','2'):
 exe=out/('test'+opt+('.exe' if os.name=='nt' else ''))
 cmd=[os.getenv('CC','cc'),'-std=c11','-O'+opt,'-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-ffp-contract=off','-fno-fast-math','-I'+str(root),'-I'+str(root/'main')]
 if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
 subprocess.run(cmd+[str(c),'-lm','-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
