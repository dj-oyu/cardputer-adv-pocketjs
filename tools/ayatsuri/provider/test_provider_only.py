"""Public synthetic provider-only capability, ownership and rollback regression.
"""
import argparse,os,pathlib,subprocess
p=argparse.ArgumentParser();p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--sanitize',action='store_true');p.add_argument('--opt',choices=['0','2'],default='2');a=p.parse_args()
here=pathlib.Path(__file__).resolve().parent;root=here.parents[2];out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
script=r'''
const fs=require('fs'),assert=require('assert'),{compile,compileProvider}=require(process.argv[2]);
const node={index:0,name:'synthetic',parent:-1,kind:'gouraudMesh',enabled:true,pose:[0,0,0,0,0,0,1,1,1],material:[.25,.75,0,4,0,0,0,0,0,0,0],color:0,thickness:1,opacity:1,edgeSoftness:0,mesh:{vertices:[-4,-4,0,4,-4,0,0,4,0],indices:[0,1,2],normals:[0,0,1,0,0,1,0,0,1],mask:[-1,-1,-1],holes:[],order:'authored',albedoA:0xf800,albedoB:0xf800,thickness:255}};
const design={schema:'ayatsuri-native-model-design-v1',nodes:[node]};
const valid=compile(design);fs.writeFileSync(process.argv[3]+'/valid.aynm',valid);
node.mesh.vertices.fill(0);assert.throws(()=>compile(design),/degenerate mesh triangle/);
const invalid=compileProvider(design);let found=false;
node.mesh.indices=[0,0,2];assert.throws(()=>compileProvider(design),/repeated mesh triangle index/);node.mesh.indices=[0,1,2];
const normals=node.mesh.normals;delete node.mesh.normals;assert.throws(()=>compileProvider(design),/provider cold normals required/);node.mesh.normals=normals;
for(let i=0;i<invalid.readUInt32LE(12);i++){const d=32+20*i;if(invalid.readUInt16LE(d)!==10)continue;const at=invalid.readUInt32LE(d+4),count=invalid.readUInt32LE(d+12);for(let v=0;v<count;v++)for(let k=0;k<3;k++)invalid.writeFloatLE(0,at+28*v+4*k);found=true;}
assert(found);fs.writeFileSync(process.argv[3]+'/collapsed.aynm',invalid);
console.log('PASS default packer rejects collapsed source even with explicit unit normals');
'''
(out/'fixture.cjs').write_text(script)
subprocess.run(['node',str(out/'fixture.cjs'),str(root/'tools/ayatsuri/model/pack_model.cjs'),str(out)],check=True)
(out/'test.c').write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ayatsuri/ayatsuri_model_provider.h"
#include "ui/kasane/ksn_mesh_depth.h"
static const float camera[9]={0,0,400,0,0,0,1,120,67},light[6]={0,0,1,1,1,1};
static unsigned calls;
static bool provide(void*context,unsigned node,unsigned count,ayatsuri_skin_result*slot){
 assert(node==0&&count==3);calls++;unsigned mode=*(unsigned*)context;
 const float xyz[3][3]={{-4,-4,0},{4,-4,0},{0,4,0}};
 for(unsigned v=0;v<3;v++){for(unsigned k=0;k<3;k++)slot[v].position[k]=mode==0?0:xyz[v][k];slot[v].normal[0]=slot[v].normal[1]=0;slot[v].normal[2]=1;}
 if(mode==2)slot[2].normal[2]=NAN;return true;
}
static void*load(const char*path,size_t*n){FILE*f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long len=ftell(f);assert(len>0);rewind(f);*n=(size_t)len;void*p=malloc(*n);assert(p&&fread(p,1,*n,f)==*n);fclose(f);return p;}
static void snapshot(ayatsuri_model_instance*i,const void*owner,ksn_mesh_snapshot*s){
 s->packets.size=s->packets.packets=s->work=0;uint32_t cursor=0;ayatsuri_model_packet packet;ayatsuri_model_status status;
 while((status=ayatsuri_model_instance_next(i,owner,1,&cursor,&packet))==AYATSURI_MODEL_OK){ayatsuri_model_mesh_view view;assert(packet.kind==4);assert(ayatsuri_model_instance_mesh_view(i,owner,1,packet.node_id,&view)==AYATSURI_MODEL_OK);assert(ksn_mesh_append(s,view.patch,view.depth,packet.node_id));}
 assert(status==AYATSURI_MODEL_DONE);
}
int main(int argc,char**argv){
 assert(argc==3);size_t bytes;void*bad=load(argv[2],&bytes);ayatsuri_model rejected;memset(&rejected,0x55,sizeof rejected);ayatsuri_model saved=rejected;
 assert(ayatsuri_model_open(bad,bytes,&rejected)==AYATSURI_MODEL_INVALID&&!memcmp(&saved,&rejected,sizeof saved));
 ayatsuri_model model;assert(ayatsuri_model_open_provider(bad,bytes,&model)==AYATSURI_MODEL_OK&&model.provider_only);
 assert(ayatsuri_model_open_provider(bad,bytes,(ayatsuri_model*)bad)==AYATSURI_MODEL_INVALID);
 void*blob=bad;
 /* Same immutable span and native model outlive every instance. Malformed
  * copies retain all old rejection rules except zero geometric area. */
 unsigned char*mut=malloc(bytes);assert(mut);
 for(unsigned test=0;test<4;test++){
  memcpy(mut,blob,bytes);
  if(test==0){mut[model.section[10].offset+2]=0;mut[model.section[10].offset+3]=0;}
  if(test==1)memset(mut+model.section[9].offset+12,0,12);
  if(test==2){float v=NAN;memcpy(mut+model.section[9].offset,&v,4);}
  if(test==3){float v=4097;memcpy(mut+model.section[9].offset,&v,4);}
  rejected=saved;assert(ayatsuri_model_open_provider(mut,bytes,&rejected)==AYATSURI_MODEL_INVALID&&!memcmp(&saved,&rejected,sizeof saved));
 }
 free(mut);
 size_t ordinary_bytes;void*ordinary_blob=load(argv[1],&ordinary_bytes);ayatsuri_model ordinary;assert(ayatsuri_model_open(ordinary_blob,ordinary_bytes,&ordinary)==AYATSURI_MODEL_OK&&!ordinary.provider_only);free(ordinary_blob);
 int owner=1;size_t need=ayatsuri_model_instance_bytes(&model);void*arena=malloc(need);assert(arena);ayatsuri_model_instance instance;assert(ayatsuri_model_instance_init(&model,arena,need,&owner,1,&instance)==AYATSURI_MODEL_OK);
 unsigned char packets[1024];ksn_mesh_snapshot ready={.packets={packets,sizeof packets,0,0},.work_limit=100000,.enabled=true};ayatsuri_skin_result slot[32];
 unsigned mode=0;
 ayatsuri_model_instance before_entry=instance;
 assert(ayatsuri_model_candidate_evaluate_provider(&instance,&owner,2,camera,light,provide,&mode,slot)==AYATSURI_MODEL_STATE);
 assert(!memcmp(&instance,&before_entry,sizeof instance));
 assert(ayatsuri_model_candidate_evaluate_provider(&instance,&owner,1,camera,light,NULL,&mode,slot)==AYATSURI_MODEL_INVALID);
 assert(ayatsuri_model_candidate_evaluate_provider(&instance,&owner,1,camera,light,provide,&mode,(ayatsuri_skin_result*)arena)==AYATSURI_MODEL_INVALID);
 int wrong_owner=0;
 assert(ayatsuri_model_candidate_evaluate_provider(&instance,&wrong_owner,1,camera,light,provide,&mode,slot)==AYATSURI_MODEL_STATE);
 assert(ayatsuri_model_candidate_evaluate_provider(&instance,&owner,1,camera,light,provide,&mode,(ayatsuri_skin_result*)((char*)slot+1))==AYATSURI_MODEL_INVALID);
 assert(calls==0);
 assert(ayatsuri_model_candidate_evaluate_provider(&instance,&owner,1,camera,light,provide,&mode,slot)==AYATSURI_MODEL_OK);snapshot(&instance,&owner,&ready);assert(!ready.packets.size);
 mode=1;assert(ayatsuri_model_candidate_evaluate_provider(&instance,&owner,1,camera,light,provide,&mode,slot)==AYATSURI_MODEL_OK);snapshot(&instance,&owner,&ready);assert(ready.packets.size>0);
 unsigned char before[1024];memcpy(before,packets,sizeof before);size_t used=ready.packets.size;
 mode=2;assert(ayatsuri_model_candidate_evaluate_provider(&instance,&owner,1,camera,light,provide,&mode,slot)==AYATSURI_MODEL_INVALID);assert(ready.packets.size==used&&!memcmp(before,packets,sizeof before));
 mode=0;assert(ayatsuri_model_candidate_evaluate_provider(&instance,&owner,1,camera,light,provide,&mode,slot)==AYATSURI_MODEL_OK);snapshot(&instance,&owner,&ready);assert(!ready.packets.size);
 /* Both ordinary evaluation entry points reject with no mutation, even
  * when a previous provider result is cached. Re-evaluation requires provider. */
 ayatsuri_model_instance prior=instance;
 void*arena_saved=malloc(need);assert(arena_saved);memcpy(arena_saved,arena,need);
 assert(ayatsuri_model_instance_evaluate(&instance,&owner,1,camera,light)==AYATSURI_MODEL_INVALID);
 assert(ayatsuri_model_instance_evaluate_meshes(&instance,&owner,1,camera,light,NULL,0)==AYATSURI_MODEL_INVALID);
 ayatsuri_model_mesh_geometry all={0,3,slot};
 assert(ayatsuri_model_instance_evaluate_meshes(&instance,&owner,1,camera,light,&all,1)==AYATSURI_MODEL_INVALID);
 assert(!memcmp(&prior,&instance,sizeof prior)&&!memcmp(arena_saved,arena,need));free(arena_saved);
 uint8_t dirty=AYATSURI_TARGET_HIDDEN;float pose[9]={0};uint32_t color=0;double opacity=1;
 ayatsuri_model_targets target={&dirty,pose,&color,&opacity,1};
 assert(ayatsuri_model_instance_targets(&instance,&owner,1,&target)==AYATSURI_MODEL_OK);
 mode=1;assert(ayatsuri_model_candidate_evaluate_provider(&instance,&owner,1,camera,light,provide,&mode,slot)==AYATSURI_MODEL_OK);snapshot(&instance,&owner,&ready);assert(!ready.packets.size&&calls==5);
 assert(ayatsuri_model_candidate_evaluate_provider(&instance,&owner,1,camera,light,provide,&mode,slot)==AYATSURI_MODEL_OK&&calls==6);
 dirty=AYATSURI_TARGET_VISIBLE;assert(ayatsuri_model_instance_targets(&instance,&owner,1,&target)==AYATSURI_MODEL_OK);
 assert(ayatsuri_model_candidate_evaluate_provider(&instance,&owner,1,camera,light,provide,&mode,slot)==AYATSURI_MODEL_OK);snapshot(&instance,&owner,&ready);assert(ready.packets.size>0&&calls==7);
 assert(ayatsuri_model_instance_dispose(&instance,&owner,1)==AYATSURI_MODEL_OK);assert(ayatsuri_model_candidate_evaluate_provider(&instance,&owner,1,camera,light,provide,&mode,slot)==AYATSURI_MODEL_STATE);free(arena);free(blob);
 puts("PASS provider-only open; malformed rejection; closed/open/failure/retry; hidden/cache calls; fallback denied; lifetime/owner/alias; ready preserved");return 0;
}
''')
sources=['main/ayatsuri/ayatsuri_model.c','main/ayatsuri/ayatsuri_visibility.c','main/ayatsuri/compact_opaque.c','main/ui/kasane/ksn_shiki_frame.c','main/ui/kasane/ksn_mesh_depth.c']
binary=out/('test.exe' if os.name=='nt' else 'test')
cmd=[os.environ.get('CC','cc'),'-std=c11','-O'+a.opt,'-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-ffp-contract=off','-fno-fast-math','-DAYATSURI_MODEL_EXPERIMENTAL_PROVIDER','-I'+str(root/'main'),'-I'+str(root/'tools/hostshim'),*[str(root/s) for s in sources],str(out/'test.c'),'-lm','-o',str(binary)]
if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
subprocess.run(cmd,check=True);subprocess.run([str(binary),str(out/'valid.aynm'),str(out/'collapsed.aynm')],check=True)
