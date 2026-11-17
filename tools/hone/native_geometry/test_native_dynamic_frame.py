"""Lazy per-mesh normals versus retained normals; explicit frame invalidation."""
import argparse,os,pathlib,subprocess
from native_geometry import emit
from test_consumer_plan import fixture
p=argparse.ArgumentParser();p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
here=pathlib.Path(__file__).resolve().parent;root=here.parents[2];out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
(out/'geometry.h').write_text(emit(fixture()))
(out/'test.c').write_text(r'''
#include "geometry.h"
#include "native_gather.h"
#include "ayatsuri/ayatsuri_model_provider.h"
#include "native_dynamic_frame_candidate.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int polygon_normals(const float*,size_t,const uint32_t*,const uint32_t*,const uint8_t*,size_t,size_t,double*,float*);
static unsigned calls;static bool fail;static double work[12];
static bool compute(void*c,const hone_input_mesh*m,const float*positions,float*out){
 (void)c;calls++;if(fail){out[0]=123;return false;}
 return polygon_normals(positions,m->source_vertex_count,m->polygon_offsets,m->polygon_vertices,m->polygon_smooth,m->polygon_count,m->polygon_corner_count,work,out)!=0;
}

static bool dynamic_compute(void*c,const hone_input_mesh*m,const float*p,double*w,size_t count,float*n){
 (void)c;assert(count>=3*(size_t)m->source_vertex_count);
 return polygon_normals(p,m->source_vertex_count,m->polygon_offsets,m->polygon_vertices,m->polygon_smooth,m->polygon_count,m->polygon_corner_count,w,n)!=0;
}
int main(void){
 const hone_input_mesh*m=hone_input_meshes;float positions[12],normal[18],cache[18];uint32_t p0[]={0,1,2},p1[]={3,4,5};
 hone_provider_mesh meshes[2]={{m,positions,normal},{m,positions,normal}};
 hone_provider_part parts[]={{0,3,p0},{0,3,p1},{1,3,p0},{1,3,p1}};hone_provider_context ctx={meshes,2,parts,4};
 hone_normal_stream stream={.source=&ctx,.normals=cache,.normal_floats=18,.compute=compute};ayatsuri_skin_result expected[3],actual[3],saved[3];
 assert(!hone_normal_stream_provider(&stream,0,3,actual));
 for(unsigned frame=1;frame<=24;frame++){
  memcpy(positions,m->source_position,sizeof positions);positions[11]=.5f+frame*.01f;
  assert(compute(NULL,m,positions,normal));assert(hone_normal_stream_begin(&stream,frame));assert(!hone_normal_stream_begin(&stream,frame));unsigned before=calls;
  for(unsigned node=0;node<4;node++){
   assert(hone_native_provider(&ctx,node,3,expected));assert(hone_normal_stream_provider(&stream,node,3,actual));assert(!memcmp(expected,actual,sizeof actual));
  }
  assert(calls==before+2);
  memcpy(saved,actual,sizeof saved);stream.valid=false;fail=true;
  assert(!hone_normal_stream_provider(&stream,0,3,actual)&&!stream.valid&&!memcmp(saved,actual,sizeof saved));fail=false;
  assert(hone_normal_stream_provider(&stream,0,3,actual));memcpy(saved,actual,sizeof saved);
  p0[2]=m->vertices;assert(!hone_normal_stream_provider(&stream,0,3,actual)&&!memcmp(saved,actual,sizeof saved));p0[2]=2;
  stream.normal_floats=17;assert(!hone_normal_stream_provider(&stream,0,3,actual));stream.normal_floats=18;
  assert(!hone_normal_stream_provider(&stream,0,3,(void*)cache));
 }

 hone_dynamic_frame dynamic={0};int owner;size_t bytes=hone_dynamic_frame_bytes(m,1);void*arena=malloc(bytes);assert(arena);hone_provider_part dp[]={{0,3,p0},{0,3,p1}};
 assert(!hone_dynamic_frame_init(&dynamic,&owner,1,m,1,dp,2,arena,bytes-1,dynamic_compute,NULL));assert(!dynamic.alive);
 assert(hone_dynamic_frame_init(&dynamic,&owner,1,m,1,dp,2,arena,bytes,dynamic_compute,NULL));
 assert(!hone_dynamic_frame_provider(&dynamic,0,3,actual));
 for(unsigned frame=1;frame<=24;frame++){
  assert(!hone_dynamic_frame_begin(&dynamic,&owner,2,frame));assert(hone_dynamic_frame_begin(&dynamic,&owner,1,frame));assert(!hone_dynamic_frame_begin(&dynamic,&owner,1,frame));
  float*p=hone_dynamic_frame_positions(&dynamic,&owner,1,0);assert(p&&!memcmp(p,m->source_position,12*sizeof *p));
  p[11]=.5f+.01f*frame;memcpy(positions,p,sizeof positions);assert(compute(NULL,m,positions,normal));
  p[11]=NAN;assert(!hone_dynamic_frame_seal(&dynamic,&owner,1)&&!hone_dynamic_frame_provider(&dynamic,0,3,actual));p[11]=positions[11];
  assert(hone_dynamic_frame_seal(&dynamic,&owner,1));assert(!hone_dynamic_frame_positions(&dynamic,&owner,1,0));
  for(unsigned node=0;node<2;node++){assert(hone_native_provider(&ctx,node,3,expected));assert(hone_dynamic_frame_provider(&dynamic,node,3,actual));assert(!memcmp(expected,actual,sizeof actual));}
  assert(!hone_dynamic_frame_provider(&dynamic,0,3,arena));
 }
 assert(!hone_dynamic_frame_begin_retained(&dynamic,&owner,2,25));
 assert(hone_dynamic_frame_begin_retained(&dynamic,&owner,1,25));
 assert(!hone_dynamic_frame_begin_retained(&dynamic,&owner,1,25));
 float *retained=hone_dynamic_frame_positions(&dynamic,&owner,1,0);
 assert(retained&&!memcmp(retained,positions,sizeof positions));
 assert(!hone_dynamic_frame_provider(&dynamic,0,3,actual));
 retained[11]=.125f;memcpy(positions,retained,sizeof positions);
 assert(compute(NULL,m,positions,normal));assert(hone_dynamic_frame_seal(&dynamic,&owner,1));
 for(unsigned node=0;node<2;node++){assert(hone_native_provider(&ctx,node,3,expected));assert(hone_dynamic_frame_provider(&dynamic,node,3,actual));assert(!memcmp(expected,actual,sizeof actual));}
 assert(!hone_dynamic_frame_dispose(&dynamic,&owner,2));assert(hone_dynamic_frame_dispose(&dynamic,&owner,1));assert(!hone_dynamic_frame_provider(&dynamic,0,3,actual));free(arena);
 printf("DYNAMIC_FRAME PASS frames=25 arena_bytes=%zu init_capacity/owner/revision/retain/seal_finite/slot_alias/dispose\n",bytes);
 stream.busy=true;assert(!hone_normal_stream_begin(&stream,25));assert(!hone_normal_stream_provider(&stream,0,3,actual));stream.busy=false;
 puts("NORMAL_STREAM PASS frames=24 exact_provider_values frame_invalidation/cache_reuse/failure_slot_rollback/capacity/alias");
}
''')
for opt in ('0','2'):
 exe=out/('test'+opt+('.exe' if os.name=='nt' else ''))
 cmd=[os.getenv('CC','cc'),'-std=c11','-O'+opt,'-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-ffp-contract=off','-fno-fast-math','-I'+str(out),'-I'+str(here),'-I'+str(root/'main')]
 if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
 subprocess.run(cmd+[str(out/'test.c'),str(root/'tools/hone/source_package/native_polygon_normals.c'),'-lm','-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
