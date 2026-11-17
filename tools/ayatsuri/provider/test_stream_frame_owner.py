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
#include "ayatsuri/ayatsuri_stream_frame_owner_candidate.h"
#include "ayatsuri/ayatsuri_stream_probe_fixture.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
static const float camera[9]={0,0,400,0,0,0,1,120,67},light[6]={0,0,1,1,1,1};
static ayatsuri_skin_result slot[32];
static unsigned calls,frame;static bool fail;
static bool provide(void*c,unsigned node,unsigned n,ayatsuri_skin_result*out){
 (void)c;calls++;if(fail&&node==2)return false;
 for(unsigned j=0;j<n;j++){float a=j*6.283185307f/n;out[j]=(ayatsuri_skin_result){{8*cosf(a)+(float)frame/8,8*sinf(a),0},{0,0,1}};}
 return true;
}
typedef struct {ayatsuri_stream_frame_owner*f;int*id;bool reject;} check;
static bool preflight(void*ctx,const ksn_mesh_snapshot*s){
 check*c=ctx;
 assert(ayatsuri_stream_frame_produce(c->f,c->id,7,camera,light,provide,NULL,slot)==AYATSURI_MODEL_STATE);
 assert(!ayatsuri_stream_frame_acquire(c->f,c->id,7,preflight,ctx).owner);
 float depth[240];uint16_t pixels[240];
 for(unsigned y=0;y<135;y++){memset(pixels,0,sizeof pixels);assert(ksn_mesh_render(s,pixels,y,1,depth,1));}
 return !c->reject;
}
static uint32_t hash(const ksn_mesh_snapshot*s){
 uint32_t h=2166136261u;float depth[240];uint16_t pixels[240];
 for(unsigned y=0;y<135;y++){memset(pixels,0,sizeof pixels);assert(ksn_mesh_render(s,pixels,y,1,depth,1));for(unsigned x=0;x<240;x++){h^=pixels[x];h*=16777619u;}}
 return h;
}
int main(void){
 ayatsuri_model m;assert(ayatsuri_stream_candidate_open(ayatsuri_stream_probe_blob,sizeof ayatsuri_stream_probe_blob,&m)==AYATSURI_MODEL_OK);
 size_t n=ayatsuri_stream_candidate_bytes(&m);void*arena=malloc(n);assert(arena);
 int id=1;ayatsuri_stream_candidate s={0};assert(ayatsuri_stream_candidate_init(&m,arena,n,&id,7,&s)==AYATSURI_MODEL_OK);
 unsigned char a[16384],b[16384],saved[16384];ayatsuri_stream_frame_owner f={0};
 assert(!ayatsuri_stream_frame_owner_init(&f,&s,&id,7,a,sizeof a,a,sizeof a,1000000));
 assert(!ayatsuri_stream_frame_owner_init(&f,&s,&id,7,arena,n,b,sizeof b,1000000));
 assert(ayatsuri_stream_frame_owner_init(&f,&s,&id,7,a,sizeof a,b,sizeof b,1000000));
 assert(!ayatsuri_stream_frame_owner_init(&f,&s,&id,7,a,sizeof a,b,sizeof b,1000000));
 assert(!ayatsuri_stream_frame_ready(&f,&id,7));check c={&f,&id,false};
 assert(ayatsuri_stream_frame_produce(&f,&id,8,camera,light,provide,NULL,slot)==AYATSURI_MODEL_STATE);
 assert(ayatsuri_stream_frame_produce(&f,&id,7,camera,light,provide,NULL,(void*)a)==AYATSURI_MODEL_INVALID);
 assert(ayatsuri_stream_frame_produce(&f,&id,7,(void*)&f,light,provide,NULL,slot)==AYATSURI_MODEL_INVALID);
 ayatsuri_stream_frame_token stale={0};uint32_t last=0;unsigned rollbacks=0;
 for(frame=0;frame<32;frame++){
  const ksn_mesh_snapshot*old=ayatsuri_stream_frame_ready(&f,&id,7);
  size_t used=old?old->packets.size:0;if(old)memcpy(saved,old->packets.data,used);
  fail=true;assert(ayatsuri_stream_frame_produce(&f,&id,7,camera,light,provide,NULL,slot)!=AYATSURI_MODEL_OK);
  if(old){assert(!memcmp(saved,old->packets.data,used));assert(hash(old)==last);}rollbacks++;
  fail=false;assert(ayatsuri_stream_frame_produce(&f,&id,7,camera,light,provide,NULL,slot)==AYATSURI_MODEL_OK);
  if(old)assert(!memcmp(saved,old->packets.data,used));
  c.reject=true;assert(!ayatsuri_stream_frame_acquire(&f,&id,7,preflight,&c).owner);
  c.reject=false;ayatsuri_stream_frame_token t=ayatsuri_stream_frame_acquire(&f,&id,7,preflight,&c);assert(t.owner);
  const ksn_mesh_snapshot*pending=ayatsuri_stream_frame_borrow(&f,t);assert(pending);uint32_t pending_hash=hash(pending);
  assert(!ayatsuri_stream_frame_complete(&f,stale,true,true));
  ayatsuri_stream_frame_token wrong=t;wrong.generation++;assert(!ayatsuri_stream_frame_complete(&f,wrong,true,true));
  assert(!ayatsuri_stream_frame_complete(&f,t,false,true));
  unsigned before=calls;assert(ayatsuri_stream_frame_produce(&f,&id,7,camera,light,provide,NULL,slot)==AYATSURI_MODEL_STATE);assert(calls==before);
  assert(hash(ayatsuri_stream_frame_borrow(&f,t))==pending_hash);
  assert(ayatsuri_stream_frame_complete(&f,t,true,false));assert(!ayatsuri_stream_frame_borrow(&f,t));
  if(old){assert(ayatsuri_stream_frame_ready(&f,&id,7)==old);assert(hash(old)==last);}rollbacks++;
  assert(ayatsuri_stream_frame_produce(&f,&id,7,camera,light,provide,NULL,slot)==AYATSURI_MODEL_OK);
  t=ayatsuri_stream_frame_acquire(&f,&id,7,preflight,&c);assert(t.owner);
  assert(ayatsuri_stream_frame_complete(&f,t,true,true));assert(!ayatsuri_stream_frame_complete(&f,t,true,true));
  last=hash(ayatsuri_stream_frame_ready(&f,&id,7));assert(last==pending_hash);stale=t;
 }
 f.revision=UINT64_MAX;assert(ayatsuri_stream_frame_produce(&f,&id,7,camera,light,provide,NULL,slot)==AYATSURI_MODEL_STATE);
 assert(ayatsuri_stream_candidate_dispose(&s,&id,7)==AYATSURI_MODEL_OK);free(arena);
 assert(hash(ayatsuri_stream_frame_ready(&f,&id,7))==last);
 printf("PASS frames=32 rollbacks=%u owner_bytes=%zu ready_outlives_stream display_completion=simulated DMA=unverified\n",rollbacks,sizeof f);
 return 0;
}
''')
sources = ['main/ayatsuri/ayatsuri_model_stream_candidate.c',
           'main/ayatsuri/ayatsuri_visibility.c', 'main/ayatsuri/compact_opaque.c',
           'main/ui/kasane/ksn_shiki_frame.c', 'main/ui/kasane/ksn_mesh_depth.c']
for opt in ('0', '2'):
    exe = out / ('owner-' + opt + ('.exe' if os.name == 'nt' else ''))
    cmd = [os.getenv('CC', 'cc'), '-std=c11', '-O' + opt, '-Wall', '-Wextra', '-Werror',
           '-ffp-contract=off', '-fno-fast-math', '-fstack-usage',
           '-I' + str(root/'main'), '-I' + str(root/'tools/hostshim')]
    if a.sanitize:
        cmd += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(cmd + [str(root/s) for s in sources] + [str(c), '-lm', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
