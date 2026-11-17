import os,pathlib,subprocess,tempfile,unittest


class TestGraphClip(unittest.TestCase):
    def test_attributes_and_rollback(self):
        here=pathlib.Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as tmp:
            p=pathlib.Path(tmp);(p/'test.c').write_text(r'''#include "alpha_graph_clip_candidate.h"
#include <assert.h>
int main(void){
 const hone_alpha_instruction c[]={{HONE_ALPHA_CONST,0,0,1}};
 const hone_alpha_program alpha={c,1,0},color[3]={{c,1,0},{c,1,0},{c,1,0}};
 /* All local components and depth are affine functions of screen x,y. */
 hone_graph_triangle input={{{-2,-1,1,{-2,-1,-3}},{6,-1,9,{6,-1,5}},{-2,7,9,{-2,7,5}}},&alpha,color};
 hone_graph_triangle result[6],saved[6];memset(result,0x5a,sizeof result);memcpy(saved,result,sizeof result);size_t count=777;
 assert(!hone_graph_clip_viewport(&input,0,0,4,4,result,0,&count));
 assert(count==777&&!memcmp(result,saved,sizeof result));
 assert(hone_graph_clip_viewport(&input,0,0,4,4,result,6,&count));assert(count>=1&&count<=5);
 for(size_t i=0;i<count;i++){
  assert(result[i].alpha==&alpha&&result[i].color==color);
  for(unsigned j=0;j<3;j++){
   hone_graph_vertex v=result[i].v[j];assert(v.x>=0&&v.x<=4&&v.y>=0&&v.y<=4);
   assert(fabsf(v.local[0]-v.x)<1e-6f&&fabsf(v.local[1]-v.y)<1e-6f);
   assert(fabsf(v.local[2]-(v.x+v.y))<1e-6f);
   assert(fabsf(v.z-(v.x+v.y+4))<1e-6f);
  }
 }
 float scratch[16];
 for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++){
  hone_graph_fragment expected={0},actual={0};int ref=hone_graph_fragment_at(&input,x,y,-INFINITY,NULL,0,scratch,16,&expected),hits=0;
  assert(ref>=0);
  for(size_t i=0;i<count;i++){
   hone_graph_fragment fragment;int got=hone_graph_fragment_at(&result[i],x,y,-INFINITY,NULL,0,scratch,16,&fragment);
   assert(got>=0);if(got){actual=fragment;hits++;}
  }
  assert(hits==ref);if(hits)assert(fabsf(actual.z-expected.z)<1e-6f&&actual.alpha==expected.alpha);
 }
 /* A fully admitted triangle preserves order, endpoints, and signed zero. */
 const hone_graph_triangle inside={{{-0.f,0,2,{-0.f,1,2}},{4,0,3,{3,4,5}},{0,4,4,{6,7,8}}},&alpha,color};
 memset(result,0x5a,sizeof result);memcpy(saved,result,sizeof result);count=777;
 assert(!hone_graph_clip_viewport(&inside,0,0,4,4,result,0,&count));
 assert(count==777&&!memcmp(result,saved,sizeof result));
 assert(hone_graph_clip_viewport(&inside,0,0,4,4,result,1,&count)&&count==1);
 assert(!memcmp(result[0].v,inside.v,sizeof inside.v));
 assert(result[0].alpha==inside.alpha&&result[0].color==inside.color);
 assert(!memcmp(result+1,saved+1,5*sizeof *result));
 /* All clipped away and late invalid input. */
 assert(hone_graph_clip_viewport(&input,10,10,11,11,result,6,&count)&&count==0);
 memset(result,0x5a,sizeof result);memcpy(saved,result,sizeof result);count=777;input.v[2].local[1]=NAN;
 assert(!hone_graph_clip_viewport(&input,0,0,4,4,result,6,&count));assert(count==777&&!memcmp(result,saved,sizeof result));
 return 0;}
''',encoding='utf8')
            cmd=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(here),str(p/'test.c'),'-lm','-o',str(p/'test')]
            if os.getenv('HONE_SANITIZE')=='1':cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            subprocess.run(cmd,check=True);subprocess.run([str(p/'test')],check=True)


if __name__=='__main__':unittest.main()
