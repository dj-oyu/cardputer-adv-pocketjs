"""Public synthetic reproduction: cold admission versus live provider geometry.
No parser/packer behavior is changed. A future provider-only admission API must
prevent ordinary cold fallback; placeholder cold geometry is NOT a solution.
"""
import argparse,os,pathlib,subprocess
p=argparse.ArgumentParser();p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
here=pathlib.Path(__file__).resolve().parent;root=here.parents[2];out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
script=r'''
const fs=require('fs'),assert=require('assert'),{compile}=require(process.argv[2]);
const node={index:0,name:'synthetic',parent:-1,kind:'gouraudMesh',enabled:true,pose:[0,0,0,0,0,0,1,1,1],material:[.25,.75,0,4,0,0,0,0,0,0,0],color:0,thickness:1,opacity:1,edgeSoftness:0,mesh:{vertices:[-4,-4,0,4,-4,0,0,4,0],indices:[0,1,2],normals:[0,0,1,0,0,1,0,0,1],mask:[-1,-1,-1],holes:[],order:'authored',albedoA:0xf800,albedoB:0xf800,thickness:255}};
const design={schema:'ayatsuri-native-model-design-v1',nodes:[node]};
const valid=compile(design);fs.writeFileSync(process.argv[3]+'/valid.aynm',valid);
node.mesh.vertices.fill(0);assert.throws(()=>compile(design),/degenerate mesh triangle/);
const invalid=Buffer.from(valid);let found=false;
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
 assert(ayatsuri_model_open(bad,bytes,&rejected)==AYATSURI_MODEL_INVALID&&!memcmp(&saved,&rejected,sizeof saved));free(bad);
 void*blob=load(argv[1],&bytes);ayatsuri_model model;assert(ayatsuri_model_open(blob,bytes,&model)==AYATSURI_MODEL_OK);
 int owner=1;size_t need=ayatsuri_model_instance_bytes(&model);void*arena=malloc(need);assert(arena);ayatsuri_model_instance instance;assert(ayatsuri_model_instance_init(&model,arena,need,&owner,1,&instance)==AYATSURI_MODEL_OK);
 unsigned char packets[1024];ksn_mesh_snapshot ready={.packets={packets,sizeof packets,0,0},.work_limit=100000,.enabled=true};ayatsuri_skin_result slot[32];
 unsigned mode=0;assert(ayatsuri_model_candidate_evaluate_provider(&instance,&owner,1,camera,light,provide,&mode,slot)==AYATSURI_MODEL_OK);snapshot(&instance,&owner,&ready);assert(!ready.packets.size);
 mode=1;assert(ayatsuri_model_candidate_evaluate_provider(&instance,&owner,1,camera,light,provide,&mode,slot)==AYATSURI_MODEL_OK);snapshot(&instance,&owner,&ready);assert(ready.packets.size>0);
 unsigned char before[1024];memcpy(before,packets,sizeof before);size_t used=ready.packets.size;
 mode=2;assert(ayatsuri_model_candidate_evaluate_provider(&instance,&owner,1,camera,light,provide,&mode,slot)==AYATSURI_MODEL_INVALID);assert(ready.packets.size==used&&!memcmp(before,packets,sizeof before));
 mode=0;assert(ayatsuri_model_candidate_evaluate_provider(&instance,&owner,1,camera,light,provide,&mode,slot)==AYATSURI_MODEL_OK);snapshot(&instance,&owner,&ready);assert(!ready.packets.size);
 /* Ordinary evaluation restores cold geometry. This is why simply relaxing
  * meshes_valid would NOT create a safe provider-only admission boundary. */
 assert(ayatsuri_model_instance_evaluate(&instance,&owner,1,camera,light)==AYATSURI_MODEL_OK);snapshot(&instance,&owner,&ready);assert(ready.packets.size>0&&calls==4);
 assert(ayatsuri_model_instance_dispose(&instance,&owner,1)==AYATSURI_MODEL_OK);free(arena);free(blob);
 puts("PASS default open rejects collapsed cold; provider closed/open/fail/closed; ready preserved; ordinary cold fallback reproduced");return 0;
}
''')
sources=['main/ayatsuri/ayatsuri_model.c','main/ayatsuri/ayatsuri_visibility.c','main/ayatsuri/compact_opaque.c','main/ui/kasane/ksn_shiki_frame.c','main/ui/kasane/ksn_mesh_depth.c']
binary=out/('test.exe' if os.name=='nt' else 'test')
cmd=[os.environ.get('CC','cc'),'-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-ffp-contract=off','-fno-fast-math','-DAYATSURI_MODEL_EXPERIMENTAL_PROVIDER','-I'+str(root/'main'),'-I'+str(root/'tools/hostshim'),*[str(root/s) for s in sources],str(out/'test.c'),'-lm','-o',str(binary)]
if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
subprocess.run(cmd,check=True);subprocess.run([str(binary),str(out/'valid.aynm'),str(out/'collapsed.aynm')],check=True)
