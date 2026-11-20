"""Actual streaming API with synthetic provider and simulated display completion."""
import argparse
import os
import pathlib
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--out', type=pathlib.Path, required=True)
p.add_argument('--sanitize', action='store_true')
a = p.parse_args()
root = pathlib.Path(__file__).resolve().parents[3]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
c = out / 'test.c'
c.write_text(r'''
#include "ayatsuri/ayatsuri_stream_probe_fixture.h"
#include "stream_material_payload_candidate.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static const float camera[9]={0,0,400,0,0,0,1,120,67},light[6]={0,0,1,1,1,1};
static unsigned frame;static bool fail;
static bool provide(void*c,unsigned node,unsigned n,ayatsuri_skin_result*out){
 (void)c;if(fail&&node==2)return false;
 for(unsigned j=0;j<n;j++){float a=j*6.283185307f/n;out[j]=(ayatsuri_skin_result){{8*cosf(a)+(float)frame/8,8*sinf(a),0},{0,0,1}};}return true;
}
static bool accept(void*c,const ksn_mesh_snapshot*s){
 ayatsuri_stream_frame_owner*f=c;const ay_material_pending*d=f->payload[1-f->committed];
 uint16_t row[240];float depth[240],scratch[16];hone_graph_band_work w={f->identity,f->generation,row,depth,scratch,16};
 return !s->packets.size&&hone_graph_band_preflight(s,&d->band,&w);
}
static unsigned u16(const uint8_t*p){return p[0]|(unsigned)p[1]<<8;}
int main(void){
 ayatsuri_model m;assert(ayatsuri_stream_candidate_open(ayatsuri_stream_probe_blob,sizeof ayatsuri_stream_probe_blob,&m)==AYATSURI_MODEL_OK);
 int id=1,binding;size_t n=ayatsuri_stream_candidate_bytes(&m);void*arena=malloc(n);ayatsuri_stream_candidate stream={0};
 assert(ayatsuri_stream_candidate_init(&m,arena,n,&id,7,&stream)==AYATSURI_MODEL_OK);
 unsigned char a[4096],b[4096];ayatsuri_stream_frame_owner f={0};assert(ayatsuri_stream_frame_owner_init(&f,&stream,&id,7,a,sizeof a,b,sizeof b,1000000));
 ayatsuri_stream_route routes[256];ay_material_mesh meshes[256];ay_material_face faces[256][64];uint32_t polygons[256*64]={0};size_t nr=0,np=0;
 for(unsigned j=0;j<m.node_count;j++){
  const uint8_t*r=m.data+m.section[0].offset+16*j;if(r[2]!=3)continue;unsigned mid=u16(r+4);const uint8_t*d=m.data+m.section[8].offset+32*mid;
  routes[nr]=(ayatsuri_stream_route){j,mid,u16(r+8),AYATSURI_STREAM_MATERIAL};meshes[nr]=(ay_material_mesh){faces[nr],u16(d+6)};
  for(size_t k=0;k<meshes[nr].count;k++)faces[nr][k]=(ay_material_face){np++,0};nr++;
 }
 hone_alpha_instruction code[]={{.op=HONE_ALPHA_DRIVER,.a=0}};hone_alpha_program alpha={code,1,0},color[3]={{code,1,0},{code,1,0},{code,1,0}};
 ay_material_program materials[]={{&alpha,color}};ay_material_package package={.routes={&m,routes,nr},.meshes=meshes,.polygon_material=polygons,.polygon_count=np,.materials=materials,.material_count=1,.driver_binding=&binding,.driver_count=1,.source_hash={1}};
 hone_graph_packet packets[2][512];double copied[2][1],drivers[]={.4};ay_material_pending pending[2]={ {.packets=packets[0],.capacity=512,.drivers=copied[0],.driver_capacity=1},{.packets=packets[1],.capacity=512,.drivers=copied[1],.driver_capacity=1} };
 ayatsuri_skin_result slot[32];unsigned rollback=0;
 for(frame=0;frame<16;frame++){
  ay_material_pending*d=&pending[1-f.committed];const ay_material_pending*old=f.ready?f.payload[f.committed]:NULL;hone_graph_packet saved[512];size_t count=old?old->band.graph.count:0;if(old)memcpy(saved,old->packets,count*sizeof *saved);
  drivers[0]=.25+.01*frame;
  assert(ay_material_produce(&f,&id,7,camera,light,provide,NULL,slot,&package,package.source_hash,&binding,drivers,1,d)==AYATSURI_MODEL_OK);
  assert(d->band.graph.ready&&d->band.graph.count&&d->drivers[0]==drivers[0]);assert(!f.snapshots[1-f.committed].packets.size);
  drivers[0]=.9;assert(d->drivers[0]!=drivers[0]);
  ayatsuri_stream_frame_token token=ayatsuri_stream_frame_acquire(&f,&id,7,accept,&f);assert(token.owner);
  assert(!ayatsuri_stream_frame_complete(&f,token,false,true));
  assert(ay_material_produce(&f,&id,7,camera,light,provide,NULL,slot,&package,package.source_hash,&binding,drivers,1,d)==AYATSURI_MODEL_STATE);
  assert(ayatsuri_stream_frame_complete(&f,token,true,false));rollback++;
  if(old)assert(f.payload[f.committed]==old&&!memcmp(saved,old->packets,count*sizeof *saved));
  d->capacity=1;assert(ay_material_produce(&f,&id,7,camera,light,provide,NULL,slot,&package,package.source_hash,&binding,drivers,1,d)!=AYATSURI_MODEL_OK);d->capacity=512;assert(!f.produced);rollback++;
  drivers[0]=NAN;assert(ay_material_produce(&f,&id,7,camera,light,provide,NULL,slot,&package,package.source_hash,&binding,drivers,1,d)==AYATSURI_MODEL_INVALID);drivers[0]=.4;
  assert(ay_material_produce(&f,&id,7,camera,light,provide,NULL,slot,&package,package.source_hash,&id,drivers,1,d)==AYATSURI_MODEL_INVALID);
  if(old)assert(ay_material_produce(&f,&id,7,camera,light,provide,NULL,slot,&package,package.source_hash,&binding,drivers,1,(ay_material_pending*)old)==AYATSURI_MODEL_INVALID);
  polygons[0]=1;assert(ay_material_produce(&f,&id,7,camera,light,provide,NULL,slot,&package,package.source_hash,&binding,drivers,1,d)==AYATSURI_MODEL_INVALID);polygons[0]=0;
  uint8_t wrong[32]={2};assert(ay_material_produce(&f,&id,7,camera,light,provide,NULL,slot,&package,wrong,&binding,drivers,1,d)==AYATSURI_MODEL_INVALID);
  fail=true;assert(ay_material_produce(&f,&id,7,camera,light,provide,NULL,slot,&package,package.source_hash,&binding,drivers,1,d)!=AYATSURI_MODEL_OK);fail=false;rollback++;
  if(old)assert(!memcmp(saved,old->packets,count*sizeof *saved));
  assert(ay_material_produce(&f,&id,7,camera,light,provide,NULL,slot,&package,package.source_hash,&binding,drivers,1,d)==AYATSURI_MODEL_OK);
  token=ayatsuri_stream_frame_acquire(&f,&id,7,accept,&f);assert(token.owner&&ayatsuri_stream_frame_complete(&f,token,true,true));assert(f.payload[f.committed]==d);
 }
 uint8_t dirty[256]={0};float poses[256*9]={0};uint32_t colors[256]={0};double opacity[256]={0};dirty[0]=AYATSURI_TARGET_HIDDEN;
 ayatsuri_model_targets targets={dirty,poses,colors,opacity,m.node_count};assert(ayatsuri_stream_candidate_targets(&stream,&id,7,&targets)==AYATSURI_MODEL_OK);
 ay_material_pending*hidden=&pending[1-f.committed];assert(ay_material_produce(&f,&id,7,camera,light,provide,NULL,slot,&package,package.source_hash,&binding,drivers,1,hidden)==AYATSURI_MODEL_OK);assert(hidden->band.graph.ready&&!hidden->band.graph.count);
 assert(ayatsuri_stream_candidate_dispose(&stream,&id,7)==AYATSURI_MODEL_OK);free(arena);
 printf("MATERIAL_PAYLOAD PASS frames=16 rollback=%u copied_drivers/clip/routes/freeze ACK=simulated lighting=absent\n",rollback);return 0;
}
''')
sources = ['main/ayatsuri/ayatsuri_model_stream_candidate.c',
           'main/ayatsuri/ayatsuri_visibility.c', 'main/ayatsuri/compact_opaque.c',
           'main/ui/kasane/ksn_shiki_frame.c', 'main/ui/kasane/ksn_mesh_depth.c']
for opt in ('0', '2'):
    exe = out / ('owner-' + opt + ('.exe' if os.name == 'nt' else ''))
    cmd = [os.getenv('CC', 'cc'), '-std=c11', '-O' + opt, '-Wall', '-Wextra', '-Werror',
           '-ffp-contract=off', '-fno-fast-math', '-fstack-usage',
           '-Wno-misleading-indentation', '-I' + str(root/'tools/ayatsuri/provider'), '-I' + str(root/'main'), '-I' + str(root/'tools/hostshim')]
    if a.sanitize:
        cmd += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(cmd + [str(root/s) for s in sources] + [str(c), '-lm', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
