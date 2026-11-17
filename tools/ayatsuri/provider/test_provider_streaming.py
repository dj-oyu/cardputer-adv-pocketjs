"""Isolated host-only streaming experiment; never compiled into firmware.

Builds a guarded transformed copy of current model.c under separate symbols.
Only provider-only group/mesh models are accepted. Each node retains 84 bytes
of transform/pose; a single max-sized mesh work slot is copied to pending
snapshot immediately and never exposed by next/view. No production ABI change.
"""
import argparse, os, pathlib, re, subprocess
p=argparse.ArgumentParser();p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--sanitize',action='store_true');p.add_argument('--opt',choices=['0','2'],default='2');a=p.parse_args()
here=pathlib.Path(__file__).resolve().parent;root=here.parents[2];out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
s=(root/'main/ayatsuri/ayatsuri_model.c').read_text()
def replace(old,new):
 global s
 if s.count(old)!=1:raise RuntimeError('streaming prototype anchor changed: '+old[:70])
 s=s.replace(old,new)
exports=re.findall(r'^(?:ayatsuri_model_status|size_t) (ayatsuri_model_\w+)\(',s,re.M)
macros='\n'.join('#define '+n+' stream_'+n for n in exports)+'\n'
anchor='size_t ayatsuri_model_instance_bytes(const ayatsuri_model *m){'
replace(anchor,'''#include "ui/kasane/ksn_mesh_depth.h"
typedef struct {ayatsuri_model_geometry_provider provider;void *user;ksn_mesh_snapshot *pending;} stream_request;
static bool stream_dispatch(void*opaque,unsigned node,unsigned count,ayatsuri_skin_result*slot){stream_request*r=opaque;return r->provider(r->user,node,count,slot);}
static size_t stream_shared_offset(const ayatsuri_model*m){return ((sizeof(model_node_state)*m->node_count+7u)&~(size_t)7u)+88u*m->node_count;}
'''+anchor)
replace('    return m&&m->data?((sizeof(model_node_state)*m->node_count+7u)&~(size_t)7u)+m->slot_bytes:0;', '''    if(!m||!m->data||!m->provider_only)return 0;
    size_t maximum=0;
    for(unsigned j=0;j<m->node_count;j++){
        const uint8_t *node=row(m,1,j,16);if(node[2]!=0&&node[2]!=3)return 0;
        if(node[2]==3){size_t bytes=node_slot_bytes(m,j,node);if(bytes>maximum)maximum=bytes;}
    }
    return stream_shared_offset(m)+maximum;''')
replace('        offset+=node_slot_bytes(m,j,node);','        offset+=88;')
anchor='        if((node[2]==1&&!ellipse_packet(i,j,right,down))'
replace(anchor,'''        uint32_t old_offset=states(i)[j].offset;
        if(node[2]==3){
            size_t shared=stream_shared_offset(i->model);
            memcpy((uint8_t*)i->arena+shared,work(i,j),84);
            states(i)[j].offset=(uint32_t)shared;
        }
'''+anchor)
replace('            return leave(i,AYATSURI_MODEL_RANGE);\n        if(node[2]){add_count', '''        {states(i)[j].offset=old_offset;return leave(i,AYATSURI_MODEL_RANGE);}
        if(node[2]==3&&states(i)[j].world_enabled){
            const ksn_proc_gouraud_patch *patch=(const void*)((const uint8_t*)work(i,j)+84);
            const float *xy=(const void*)((const uint8_t*)patch+sizeof *patch);
            stream_request *request=context;
            if(patch->header.triangles&&!ksn_mesh_append(request->pending,patch,xy+2*patch->header.vertices,j)){
                states(i)[j].offset=old_offset;return leave(i,AYATSURI_MODEL_RANGE);
            }
        }
        states(i)[j].offset=old_offset;
        if(node[2]){add_count''')
s+='''\n/* Fixture-only entry. Caller owns disjoint pending snapshot storage and invokes
 * no retained-view or next API on a streaming instance. No publication here. */
ayatsuri_model_status stream_candidate_frame(ayatsuri_model_instance*i,const void*owner,uint32_t generation,const float*camera,const float*light,ayatsuri_model_geometry_provider provider,void*user,ayatsuri_skin_result*slot,ksn_mesh_snapshot*pending){
 if(!pending||!provider)return AYATSURI_MODEL_INVALID;
 stream_request request={provider,user,pending};
 pending->packets.size=0;pending->packets.packets=0;pending->work=0;
 return ayatsuri_model_candidate_evaluate_provider(i,owner,generation,camera,light,stream_dispatch,&request,slot);
}
'''
# Streaming packet storage is ephemeral: do not let the copied model API
# expose borrowed packet pointers after its shared slot has been overwritten.
for name,unused in [('ayatsuri_model_instance_next','(void)cursor;(void)out;'),('ayatsuri_model_instance_mesh_view','(void)id;(void)out;'),('ayatsuri_model_instance_sided_mesh_view','(void)id;(void)out;')]:
    start=s.index('ayatsuri_model_status '+name+'(');brace=s.index('{',start);end=s.index('\n}',brace)+2
    s=s[:brace]+('{'+unused+'ayatsuri_model_status status=enter(i,owner,generation);if(status)return status;return leave(i,AYATSURI_MODEL_INVALID);}')+s[end:]
(out/'stream_model.c').write_text(macros+s)
subprocess.run(['node',str(here/'fixture.cjs'),str(out/'synthetic.aynm')],check=True)
(out/'test.c').write_text(r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ayatsuri/ayatsuri_model_provider.h"
#include "ui/kasane/ksn_mesh_depth.h"
size_t stream_ayatsuri_model_instance_bytes(const ayatsuri_model*);
ayatsuri_model_status stream_ayatsuri_model_instance_next(ayatsuri_model_instance*,const void*,uint32_t,uint32_t*,ayatsuri_model_packet*);
ayatsuri_model_status stream_ayatsuri_model_instance_init(const ayatsuri_model*,void*,size_t,const void*,uint32_t,ayatsuri_model_instance*);
ayatsuri_model_status stream_ayatsuri_model_instance_dispose(ayatsuri_model_instance*,const void*,uint32_t);
ayatsuri_model_status stream_ayatsuri_model_instance_targets(ayatsuri_model_instance*,const void*,uint32_t,const ayatsuri_model_targets*);
ayatsuri_model_status stream_candidate_frame(ayatsuri_model_instance*,const void*,uint32_t,const float*,const float*,ayatsuri_model_geometry_provider,void*,ayatsuri_skin_result*,ksn_mesh_snapshot*);
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
 ayatsuri_model m;assert(ayatsuri_model_open_provider(blob,bytes,&m)==AYATSURI_MODEL_OK);
 int owner=1;size_t regular_bytes=ayatsuri_model_instance_bytes(&m),stream_bytes=stream_ayatsuri_model_instance_bytes(&m);assert(stream_bytes<regular_bytes);
 void*regular_arena=malloc(regular_bytes),*stream_arena=malloc(stream_bytes);assert(regular_arena&&stream_arena);
 ayatsuri_model_instance regular,stream;assert(ayatsuri_model_instance_init(&m,regular_arena,regular_bytes,&owner,1,&regular)==AYATSURI_MODEL_OK);assert(stream_ayatsuri_model_instance_init(&m,stream_arena,stream_bytes,&owner,1,&stream)==AYATSURI_MODEL_OK);
 unsigned char rb[16384],pb[16384],ob[16384],saved[16384];
 ksn_mesh_snapshot ready={.packets={rb,sizeof rb,0,0},.work_limit=1000000,.enabled=true},pending={.packets={pb,sizeof pb,0,0},.work_limit=1000000,.enabled=true},oracle={.packets={ob,sizeof ob,0,0},.work_limit=1000000,.enabled=true};
 ayatsuri_skin_result slot[32];unsigned failures=0;
 for(unsigned frame=0;frame<24;frame++){
  uint8_t dirty[24]={0};float poses[24*9]={0};uint32_t colors[24]={0};double opacity[24]={0};
  dirty[0]=frame%7==0?AYATSURI_TARGET_HIDDEN:AYATSURI_TARGET_VISIBLE;
  ayatsuri_model_targets targets={dirty,poses,colors,opacity,24};
  assert(ayatsuri_model_instance_targets(&regular,&owner,1,&targets)==AYATSURI_MODEL_OK);
  assert(stream_ayatsuri_model_instance_targets(&stream,&owner,1,&targets)==AYATSURI_MODEL_OK);
  struct Producer p={frame,0,0};assert(ayatsuri_model_candidate_evaluate_provider(&regular,&owner,1,camera,light,provide,&p,slot)==AYATSURI_MODEL_OK&&p.calls==23);collect(&regular,&owner,&oracle);
  p.calls=0;assert(stream_candidate_frame(&stream,&owner,1,camera,light,provide,&p,slot,&pending)==AYATSURI_MODEL_OK&&p.calls==23);
  uint32_t cursor=0;ayatsuri_model_packet forbidden;memset(&forbidden,0x55,sizeof forbidden);ayatsuri_model_packet old_forbidden=forbidden;
  assert(stream_ayatsuri_model_instance_next(&stream,&owner,1,&cursor,&forbidden)==AYATSURI_MODEL_INVALID&&cursor==0&&!memcmp(&forbidden,&old_forbidden,sizeof forbidden));
  assert(pending.packets.size==oracle.packets.size&&!memcmp(pending.packets.data,oracle.packets.data,oracle.packets.size));
  ksn_mesh_snapshot swap=ready;ready=pending;pending=swap;
  for(unsigned rows=1;rows<=8;rows*=8){render(&ready,pixels,rows);render(&oracle,expected,rows);assert(!memcmp(pixels,expected,sizeof pixels));}
  size_t used=ready.packets.size;memcpy(saved,ready.packets.data,used);ksn_mesh_snapshot ready_before=ready;
  for(int node=1;node<=23;node++)for(int sign=-1;sign<=1;sign+=2){p.fail=sign*node;p.calls=0;assert(stream_candidate_frame(&stream,&owner,1,camera,light,provide,&p,slot,&pending)!=AYATSURI_MODEL_OK);assert(!memcmp(&ready,&ready_before,sizeof ready)&&!memcmp(saved,ready.packets.data,used));failures++;}
  p.fail=0;pending.packets.capacity=1;ayatsuri_model_status limited=stream_candidate_frame(&stream,&owner,1,camera,light,provide,&p,slot,&pending);assert((limited!=AYATSURI_MODEL_OK)==(used!=0));assert(!memcmp(&ready,&ready_before,sizeof ready)&&!memcmp(saved,ready.packets.data,used));if(used)failures++;pending.packets.capacity=sizeof pb;
 }
 assert(stream_ayatsuri_model_instance_dispose(&stream,&owner,1)==AYATSURI_MODEL_OK);assert(ayatsuri_model_instance_dispose(&regular,&owner,1)==AYATSURI_MODEL_OK);free(stream_arena);free(regular_arena);free(blob);
 render(&ready,pixels,8);assert(!memcmp(pixels,expected,sizeof pixels));
 printf("PASS streaming frames=24 bitwise_packets/raster rollback=%u arena=%zu->%zu saved=%zu ready_outlives_model\n",failures,regular_bytes,stream_bytes,regular_bytes-stream_bytes);return 0;
}
''')
sources=['main/ayatsuri/ayatsuri_model.c','main/ayatsuri/ayatsuri_visibility.c','main/ayatsuri/compact_opaque.c','main/ui/kasane/ksn_shiki_frame.c','main/ui/kasane/ksn_mesh_depth.c']
binary=out/('test.exe' if os.name=='nt' else 'test')
cmd=[os.environ.get('CC','cc'),'-std=c11','-O'+a.opt,'-g','-Wall','-Wextra','-Werror','-ffp-contract=off','-fno-fast-math','-DAYATSURI_MODEL_EXPERIMENTAL_PROVIDER','-I'+str(root/'main'),'-I'+str(root/'main/ayatsuri'),'-I'+str(root/'tools/hostshim'),*[str(root/s) for s in sources],str(out/'stream_model.c'),str(out/'test.c'),'-lm','-o',str(binary)]
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
