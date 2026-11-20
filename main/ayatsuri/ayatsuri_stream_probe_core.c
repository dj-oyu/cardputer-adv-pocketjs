#include "ayatsuri_stream_probe_core.h"
#include "ayatsuri_stream_probe_fixture.h"
#include <math.h>
#include <string.h>
#include <limits.h>
typedef struct {unsigned frame,calls;bool fail;} producer;
static bool provide(void*opaque,unsigned node,unsigned count,ayatsuri_skin_result*slot){
 producer*p=opaque;p->calls++;
 if(node<1||node>3||count!=32||(p->fail&&node==2))return false;
 for(unsigned v=0;v<count;v++){
  float a=(float)v*(6.283185307179586f/32.f);
  slot[v]=(ayatsuri_skin_result){{8.f*cosf(a)+3.f*sinf(p->frame*.2f),8.f*sinf(a),2.f*sinf(p->frame*.1f+node)},{0,0,1}};
 }
 return true;
}
static bool render(const ksn_mesh_snapshot*s,ayatsuri_stream_probe_work*w,uint32_t*out){
 uint32_t hash=2166136261u;
 for(unsigned y=0;y<135;y+=8){unsigned rows=135-y<8?135-y:8;
  for(unsigned j=0;j<240*rows;j++)w->band[j]=17;
  if(!ksn_mesh_render(s,w->band,y,rows,w->depth,AY_STREAM_PROBE_DEPTH_ROWS))return false;
  for(unsigned j=0;j<240*rows;j++){hash^=w->band[j];hash*=16777619u;}
 }
 *out=hash;return true;
}
size_t ayatsuri_stream_probe_arena_bytes(void){
 ayatsuri_model m;if(ayatsuri_stream_candidate_open(ayatsuri_stream_probe_blob,sizeof ayatsuri_stream_probe_blob,&m)!=AYATSURI_MODEL_OK)return 0;
 return ayatsuri_stream_candidate_bytes(&m);
}
static bool disjoint(const void*a,size_t an,const void*b,size_t bn){
 uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
 return an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);
}
bool ayatsuri_stream_probe_run(void*arena,size_t bytes,ayatsuri_stream_probe_work*w,ayatsuri_stream_probe_stats*out,
 ayatsuri_stream_probe_clock clock,ayatsuri_stream_probe_yield yield,void*context){
 if(!arena||!w||!out||!clock||!disjoint(arena,bytes,w,sizeof *w)||!disjoint(arena,bytes,out,sizeof *out)||!disjoint(w,sizeof *w,out,sizeof *out))return false;
 *out=(ayatsuri_stream_probe_stats){0};
 ayatsuri_model m;if(ayatsuri_stream_candidate_open(ayatsuri_stream_probe_blob,sizeof ayatsuri_stream_probe_blob,&m)!=AYATSURI_MODEL_OK)return false;
 ayatsuri_stream_candidate candidate={0};int owner=0;
 if(ayatsuri_stream_candidate_init(&m,arena,bytes,&owner,1,&candidate)!=AYATSURI_MODEL_OK)return false;
 out->arena_bytes=ayatsuri_stream_candidate_bytes(&m);out->workspace_bytes=sizeof *w;out->geometry_min_us=UINT64_MAX;
 ksn_mesh_snapshot ready={.packets={w->ready_bytes,sizeof w->ready_bytes,0,0},.work_limit=1000000,.enabled=true};
 ksn_mesh_snapshot pending={.packets={w->pending_bytes,sizeof w->pending_bytes,0,0},.work_limit=1000000,.enabled=true};
 const float camera[9]={0,0,400,0,0,0,1,120,67},light[6]={0,0,1,1,1,1};producer p={0};bool ok=false;
 for(unsigned frame=0;frame<AY_STREAM_PROBE_FRAMES;frame++){
  p.frame=frame;p.fail=false;uint64_t started=clock(context);
  if(ayatsuri_stream_candidate_frame(&candidate,&owner,1,camera,light,provide,&p,w->slot,&pending)!=AYATSURI_MODEL_OK)goto done;
  uint64_t finished=clock(context);if(finished<started)goto done;uint64_t elapsed=finished-started;
  out->geometry_us+=elapsed;if(elapsed<out->geometry_min_us)out->geometry_min_us=elapsed;if(elapsed>out->geometry_max_us)out->geometry_max_us=elapsed;
  ksn_mesh_snapshot swap=ready;ready=pending;pending=swap;
  if(ready.packets.size>sizeof w->saved)goto done;
  if(ready.packets.size>out->snapshot_peak)out->snapshot_peak=ready.packets.size;
  started=clock(context);if(!render(&ready,w,&out->final_pixel_hash))goto done;finished=clock(context);if(finished<started)goto done;out->raster_us+=finished-started;
  ksn_mesh_snapshot old;memcpy(&old,&ready,sizeof old);memcpy(w->saved,ready.packets.data,ready.packets.size);
  p.fail=true;
  if(ayatsuri_stream_candidate_frame(&candidate,&owner,1,camera,light,provide,&p,w->slot,&pending)==AYATSURI_MODEL_OK)goto done;
  if(memcmp(&old,&ready,sizeof old)||memcmp(w->saved,ready.packets.data,ready.packets.size))goto done;
  out->rollbacks++;out->frames++;if(yield)yield(context,frame);
 }
 ok=true;
done:
 out->provider_calls=p.calls;
 if(ayatsuri_stream_candidate_dispose(&candidate,&owner,1)!=AYATSURI_MODEL_OK)ok=false;
 if(ok){
  memset(arena,0xa5,out->arena_bytes);uint32_t after=0;
  out->ready_after_arena_wipe=render(&ready,w,&after)&&after==out->final_pixel_hash;
  ok=out->ready_after_arena_wipe&&out->frames==AY_STREAM_PROBE_FRAMES&&out->provider_calls==AY_STREAM_PROBE_FRAMES*5u&&out->snapshot_peak>0;
 }
 return ok;
}
