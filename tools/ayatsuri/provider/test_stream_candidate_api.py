"""Test the separately compiled streaming API, not a transformed model copy.
Reuses the bounded synthetic geometry and rollback assertions from the earlier
experiment; no private assets or firmware build are involved.
"""
import argparse,os,pathlib,subprocess
p=argparse.ArgumentParser();p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--sanitize',action='store_true');p.add_argument('--opt',choices=['0','2'],default='2');a=p.parse_args()
here=pathlib.Path(__file__).resolve().parent;root=here.parents[2];out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
subprocess.run(['node',str(here/'fixture.cjs'),str(out/'synthetic.aynm')],check=True)
(out/'test.c').write_text(r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ayatsuri/ayatsuri_model_stream_candidate.h"
ayatsuri_model_status stream_internal_ayatsuri_model_instance_next(ayatsuri_model_instance*,const void*,uint32_t,uint32_t*,ayatsuri_model_packet*);
#include "ui/kasane/ksn_mesh_depth.h"
static const float camera[9]={0,0,400,0,0,0,1,120,67},light[6]={0,0,1,1,1,1};
static uint16_t pixels[240*135],expected[240*135];static float depth[240*8];
struct Producer{unsigned frame,calls;int fail;};
static bool provide(void*ctx,unsigned node,unsigned count,ayatsuri_skin_result*slot){
 struct Producer*p=ctx;p->calls++;assert(node>=1&&node<=23&&count>=3&&count<=32);
 if((int)node==p->fail)return false;
 for(unsigned v=0;v<count;v++){
  double a=v*2*3.141592653589793/count;
  slot[v]=(ayatsuri_skin_result){{(float)(8*cos(a)+3*sin(p->frame*.2)),(float)(8*sin(a)),(float)(2*sin(p->frame*.1+node))},{0,0,1}};
 }
 if(-(int)node==p->fail)slot[count-1].normal[2]=0;
 return true;
}
static void collect(ayatsuri_model_instance*i,void*owner,ksn_mesh_snapshot*s){
 s->packets.size=0;s->packets.packets=0;s->work=0;uint32_t cursor=0;ayatsuri_model_packet packet;ayatsuri_model_status status;
 while((status=ayatsuri_model_instance_next(i,owner,1,&cursor,&packet))==AYATSURI_MODEL_OK){ayatsuri_model_mesh_view v;assert(ayatsuri_model_instance_mesh_view(i,owner,1,packet.node_id,&v)==AYATSURI_MODEL_OK);assert(ksn_mesh_append(s,v.patch,v.depth,packet.node_id));}
 assert(status==AYATSURI_MODEL_DONE);
}
static void render(ksn_mesh_snapshot*s,uint16_t*p,unsigned rows){for(unsigned j=0;j<240*135;j++)p[j]=17;for(unsigned y=0;y<135;y+=8)assert(ksn_mesh_render(s,p+y*240,y,135-y<8?135-y:8,depth,rows));}
int main(int argc,char**argv){
 assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);size_t bytes=(size_t)ftell(f);rewind(f);void*blob=malloc(bytes);assert(blob&&fread(blob,1,bytes,f)==bytes);fclose(f);
 ayatsuri_model m;assert(ayatsuri_stream_candidate_open(blob,bytes,&m)==AYATSURI_MODEL_OK);
 ayatsuri_model ordinary;assert(ayatsuri_model_open(blob,bytes,&ordinary)==AYATSURI_MODEL_OK);assert(ayatsuri_stream_candidate_bytes(&ordinary)==0);
 int owner=1;size_t regular_bytes=ayatsuri_model_instance_bytes(&m),stream_bytes=ayatsuri_stream_candidate_bytes(&m);assert(stream_bytes<regular_bytes);
 void*regular_arena=malloc(regular_bytes),*stream_arena=malloc(stream_bytes);assert(regular_arena&&stream_arena);
 ayatsuri_model_instance regular;ayatsuri_stream_candidate stream={0};assert(ayatsuri_model_instance_init(&m,regular_arena,regular_bytes,&owner,1,&regular)==AYATSURI_MODEL_OK);assert(ayatsuri_stream_candidate_init(&m,stream_arena,stream_bytes,&owner,1,&stream)==AYATSURI_MODEL_OK);
 unsigned char rb[16384],pb[16384],ob[16384],saved[16384];
 ksn_mesh_snapshot ready={.packets={rb,sizeof rb,0,0},.work_limit=1000000,.enabled=true},pending={.packets={pb,sizeof pb,0,0},.work_limit=1000000,.enabled=true},oracle={.packets={ob,sizeof ob,0,0},.work_limit=1000000,.enabled=true};
 ayatsuri_skin_result slot[32];unsigned failures=0;
 assert(ayatsuri_stream_candidate_init(&m,stream_arena,stream_bytes,&owner,1,&stream)==AYATSURI_MODEL_INVALID);
 struct Producer preflight={0};ksn_mesh_snapshot before_pending=pending;
 assert(ayatsuri_stream_candidate_frame(&stream,&owner,2,camera,light,provide,&preflight,slot,&pending)==AYATSURI_MODEL_STATE);
 assert(!memcmp(&before_pending,&pending,sizeof pending));
 assert(ayatsuri_stream_candidate_frame(&stream,&owner,1,camera,light,provide,&preflight,(void*)stream_arena,&pending)==AYATSURI_MODEL_INVALID);
 assert(!memcmp(&before_pending,&pending,sizeof pending));
 ksn_mesh_snapshot alias=pending;alias.packets.data=stream_arena;alias.packets.capacity=stream_bytes;
 assert(ayatsuri_stream_candidate_frame(&stream,&owner,1,camera,light,provide,&preflight,slot,&alias)==AYATSURI_MODEL_INVALID);
 stream.core.busy=true;assert(ayatsuri_stream_candidate_frame(&stream,&owner,1,camera,light,provide,&preflight,slot,&pending)==AYATSURI_MODEL_BUSY);stream.core.busy=false;
 assert(!memcmp(&before_pending,&pending,sizeof pending));

 for(unsigned frame=0;frame<24;frame++){
  uint8_t dirty[24]={0};float poses[24*9]={0};uint32_t colors[24]={0};double opacity[24]={0};
  dirty[0]=frame%7==0?AYATSURI_TARGET_HIDDEN:AYATSURI_TARGET_VISIBLE;
  ayatsuri_model_targets targets={dirty,poses,colors,opacity,24};
  assert(ayatsuri_model_instance_targets(&regular,&owner,1,&targets)==AYATSURI_MODEL_OK);
  assert(ayatsuri_stream_candidate_targets(&stream,&owner,1,&targets)==AYATSURI_MODEL_OK);
  struct Producer p={frame,0,0};assert(ayatsuri_model_candidate_evaluate_provider(&regular,&owner,1,camera,light,provide,&p,slot)==AYATSURI_MODEL_OK&&p.calls==23);collect(&regular,&owner,&oracle);
  p.calls=0;assert(ayatsuri_stream_candidate_frame(&stream,&owner,1,camera,light,provide,&p,slot,&pending)==AYATSURI_MODEL_OK&&p.calls==23);
  uint32_t cursor=0;ayatsuri_model_packet forbidden;memset(&forbidden,0x55,sizeof forbidden);ayatsuri_model_packet old_forbidden=forbidden;
  assert(stream_internal_ayatsuri_model_instance_next(&stream.core,&owner,1,&cursor,&forbidden)==AYATSURI_MODEL_INVALID&&cursor==0&&!memcmp(&forbidden,&old_forbidden,sizeof forbidden));
  assert(pending.packets.size==oracle.packets.size&&!memcmp(pending.packets.data,oracle.packets.data,oracle.packets.size));
  ksn_mesh_snapshot swap=ready;ready=pending;pending=swap;
  for(unsigned rows=1;rows<=8;rows*=8){render(&ready,pixels,rows);render(&oracle,expected,rows);assert(!memcmp(pixels,expected,sizeof pixels));}
  size_t used=ready.packets.size;memcpy(saved,ready.packets.data,used);ksn_mesh_snapshot ready_before=ready;
  for(int node=1;node<=23;node++)for(int sign=-1;sign<=1;sign+=2){p.fail=sign*node;p.calls=0;assert(ayatsuri_stream_candidate_frame(&stream,&owner,1,camera,light,provide,&p,slot,&pending)!=AYATSURI_MODEL_OK);assert(!memcmp(&ready,&ready_before,sizeof ready)&&!memcmp(saved,ready.packets.data,used));failures++;}
  p.fail=0;pending.packets.capacity=1;ayatsuri_model_status limited=ayatsuri_stream_candidate_frame(&stream,&owner,1,camera,light,provide,&p,slot,&pending);assert((limited!=AYATSURI_MODEL_OK)==(used!=0));assert(!memcmp(&ready,&ready_before,sizeof ready)&&!memcmp(saved,ready.packets.data,used));if(used)failures++;pending.packets.capacity=sizeof pb;
 }
 assert(ayatsuri_stream_candidate_dispose(&stream,&owner,1)==AYATSURI_MODEL_OK);assert(ayatsuri_model_instance_dispose(&regular,&owner,1)==AYATSURI_MODEL_OK);free(stream_arena);free(regular_arena);free(blob);
 render(&ready,pixels,8);assert(!memcmp(pixels,expected,sizeof pixels));
 printf("PASS streaming frames=24 bitwise_packets/raster rollback=%u arena=%zu->%zu saved=%zu ready_outlives_model\n",failures,regular_bytes,stream_bytes,regular_bytes-stream_bytes);return 0;
}
''')
sources=['main/ayatsuri/ayatsuri_model.c','main/ayatsuri/ayatsuri_visibility.c','main/ayatsuri/compact_opaque.c','main/ui/kasane/ksn_shiki_frame.c','main/ui/kasane/ksn_mesh_depth.c']
binary=out/('test.exe' if os.name=='nt' else 'test')
cmd=[os.environ.get('CC','cc'),'-std=c11','-O'+a.opt,'-g','-Wall','-Wextra','-Werror','-ffp-contract=off','-fno-fast-math','-DAYATSURI_MODEL_EXPERIMENTAL_PROVIDER','-I'+str(root/'main'),'-I'+str(root/'main/ayatsuri'),'-I'+str(root/'tools/hostshim'),*[str(root/s) for s in sources],str(root/'main/ayatsuri/ayatsuri_model_stream_candidate.c'),str(out/'test.c'),'-lm','-o',str(binary)]
if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
subprocess.run(cmd,check=True);subprocess.run([str(binary),str(out/'synthetic.aynm')],check=True)
# Mesh-parent chain catches incorrect restoration of per-node world transforms.
fixture=(here/'fixture.cjs').read_text()
if fixture.count('parent:0,kind:')!=1:raise RuntimeError('hierarchy fixture anchor changed')
fixture=fixture.replace("require('../model/pack_model.cjs')",'require(process.argv[3])')
fixture=fixture.replace('parent:0,kind:','parent:i===1?0:i-1,kind:')
(out/'chain.cjs').write_text(fixture)
subprocess.run(['node',str(out/'chain.cjs'),str(out/'chain.aynm'),str(root/'tools/ayatsuri/model/pack_model.cjs')],check=True)
subprocess.run([str(binary),str(out/'chain.aynm')],check=True)

# Mixed 3/32 vertex mesh chain exercises variable-size reuse of the max slot.
for anchor in ('const v=[],idx=[];', 'j<32;', 'j<31;', 'Array(32)', 'Math.PI/32'):
    if fixture.count(anchor)!=(2 if anchor=='Math.PI/32' else 1):raise RuntimeError('mixed fixture anchor changed: '+anchor)
fixture=fixture.replace('const v=[],idx=[];','const v=[],idx=[],nv=i%2?3:32;').replace('j<32;','j<nv;').replace('j<31;','j<nv-1;').replace('Array(32)','Array(nv)').replace('Math.PI/32','Math.PI/nv')
(out/'mixed.cjs').write_text(fixture)
subprocess.run(['node',str(out/'mixed.cjs'),str(out/'mixed.aynm'),str(root/'tools/ayatsuri/model/pack_model.cjs')],check=True)
subprocess.run([str(binary),str(out/'mixed.aynm')],check=True)
