#ifndef HONE_PROJECTED_SNAPSHOT_CANDIDATE_H
#define HONE_PROJECTED_SNAPSHOT_CANDIDATE_H
#include "alpha_graph_clip_candidate.h"
#include "../source_package/alpha_packet_candidate.h"
#include <stdbool.h>
typedef struct {float xyz[9];uint32_t material,order;} hone_projected_packet;
typedef struct {float rgb[3];uint32_t admitted;} hone_projected_material;
_Static_assert(sizeof(hone_projected_packet)==44,"projected packet ABI");
_Static_assert(sizeof(hone_projected_material)==16,"projected material ABI");
typedef struct {
 hone_projected_packet*packets;size_t count,capacity;
 const hone_projected_material*materials;size_t material_count;
 const void*owner;uint32_t generation;uint64_t revision;
 bool initialized,building,ready,pinned,failed;
} hone_projected_snapshot;
static inline bool hone_projected_material_admit(const hone_alpha_program*a,const hone_alpha_program*c,hone_projected_material*out){
 if(!a||!c||!out||!a->code||a->count!=1||a->result||a->code[0].op!=HONE_ALPHA_CONST||a->code[0].value!=1)return false;
 if(!hone_alpha_disjoint(out,sizeof *out,a,sizeof *a)||!hone_alpha_disjoint(out,sizeof *out,c,3*sizeof *c)||!hone_alpha_disjoint(out,sizeof *out,a->code,sizeof *a->code))return false;
 hone_projected_material result={.admitted=1};
 for(unsigned k=0;k<3;k++){
  if(!c[k].code||c[k].count!=1||c[k].result||c[k].code[0].op!=HONE_ALPHA_CONST||!isfinite(c[k].code[0].value)||c[k].code[0].value<0||c[k].code[0].value>1)return false;
  if(!hone_alpha_disjoint(out,sizeof *out,c[k].code,sizeof *c[k].code))return false;
  result.rgb[k]=c[k].code[0].value;
 }
 *out=result;return true;
}
/* Native caller owns exclusive arrays; material table stays immutable. init is
 * not source-package authentication. Never reinitialize a live snapshot. */
static inline bool hone_projected_snapshot_init(hone_projected_snapshot*s,const void*owner,uint32_t generation,
 hone_projected_packet*packets,size_t capacity,const hone_projected_material*materials,size_t material_count){
 if(!s||s->initialized||!owner||!generation||!packets||!capacity||capacity>65535||!materials||!material_count||material_count>65535||
  !hone_alpha_disjoint(s,sizeof *s,packets,capacity*sizeof *packets)||!hone_alpha_disjoint(s,sizeof *s,materials,material_count*sizeof *materials)||
  !hone_alpha_disjoint(packets,capacity*sizeof *packets,materials,material_count*sizeof *materials))return false;
 *s=(hone_projected_snapshot){.packets=packets,.capacity=capacity,.materials=materials,.material_count=material_count,.owner=owner,.generation=generation,.initialized=true};return true;
}
static inline bool hone_projected_begin(hone_projected_snapshot*s,const void*owner,uint32_t generation,uint64_t revision){
 if(!s||!s->initialized||s->pinned||s->owner!=owner||s->generation!=generation||!revision||revision<=s->revision)return false;
 s->revision=revision;s->count=0;s->building=true;s->ready=false;s->failed=false;return true;
}
/* Validate original local attributes before omitting them. Programs must prove
 * exact opaque constant RGB matching this admitted material. General shaders
 * must use a separate full-attribute route, or reject the diagnostic frame.
 * Partial frame failure poisons seal; already appended prefix remains unchanged. */
static inline bool hone_projected_append(hone_projected_snapshot*s,const hone_graph_triangle*t,uint32_t material,uint32_t first_order){
 if(!s||!s->initialized||!s->building||s->ready||s->pinned||s->failed)return false;
 hone_projected_material check;
 if(!t||material>=s->material_count||s->materials[material].admitted!=1||
   !hone_projected_material_admit(t->alpha,t->color,&check)||memcmp(check.rgb,s->materials[material].rgb,sizeof check.rgb)||
   !hone_alpha_disjoint(t,sizeof *t,s->packets,s->capacity*sizeof *s->packets)||!hone_alpha_disjoint(t,sizeof *t,s,sizeof *s)){s->failed=true;return false;}
 for(unsigned v=0;v<3;v++)for(unsigned k=0;k<3;k++)if(!isfinite(t->v[v].local[k])||fabsf(t->v[v].local[k])>1e20f){s->failed=true;return false;}
 const hone_alpha_program*programs[4]={t->alpha,t->color,t->color+1,t->color+2};
 for(unsigned k=0;k<4;k++)if(!hone_alpha_disjoint(s->packets,s->capacity*sizeof *s->packets,programs[k],sizeof *programs[k])||!hone_alpha_disjoint(s->packets,s->capacity*sizeof *s->packets,programs[k]->code,sizeof *programs[k]->code)){s->failed=true;return false;}
 hone_graph_triangle clipped[6];size_t count=0;
 if(!hone_graph_clip_viewport(t,0,0,240,135,clipped,6,&count)||count>s->capacity-s->count||(count&&first_order>UINT32_MAX-(count-1))){s->failed=true;return false;}
 for(size_t j=0;j<count;j++)for(unsigned v=0;v<3;v++)for(unsigned k=0;k<3;k++)if(fabsf(clipped[j].v[v].local[k])>1e20f){s->failed=true;return false;}
 hone_projected_packet staged[6];
 for(size_t j=0;j<count;j++){
  staged[j]=(hone_projected_packet){.material=material,.order=first_order+(uint32_t)j};
  for(unsigned k=0;k<3;k++){staged[j].xyz[3*k]=clipped[j].v[k].x;staged[j].xyz[3*k+1]=clipped[j].v[k].y;staged[j].xyz[3*k+2]=clipped[j].v[k].z;}
 }
 memcpy(s->packets+s->count,staged,count*sizeof *staged);s->count+=count;return true;
}
static inline bool hone_projected_seal(hone_projected_snapshot*s){
 if(!s||!s->initialized||!s->building||s->pinned||s->failed)return false;s->building=false;s->ready=true;return true;
}
static inline bool hone_projected_pin(hone_projected_snapshot*s,const void*owner,uint32_t generation,uint64_t revision){
 if(!s||!s->ready||s->building||s->pinned||s->failed||s->owner!=owner||s->generation!=generation||s->revision!=revision)return false;s->pinned=true;return true;
}
/* drained must come from authoritative finish_display, including error drain.
 * false retains the pin. This one-buffer API does not retain an older READY. */
static inline bool hone_projected_finish(hone_projected_snapshot*s,uint64_t revision,bool drained){
 if(!s||!s->pinned||s->revision!=revision||!drained)return false;s->pinned=false;return true;
}
/* Read-only callback-duration borrow; stable while ready/pinned, expires at the
 * next successful begin. Caller checks owner/generation/revision before replay. */
static inline const hone_projected_packet*hone_projected_at(const hone_projected_snapshot*s,size_t index){
 return s&&s->ready&&!s->failed&&index<s->count?s->packets+index:NULL;
}
static inline const hone_projected_material*hone_projected_color(const hone_projected_snapshot*s,const hone_projected_packet*p){
 return s&&s->ready&&p&&p->material<s->material_count&&s->materials[p->material].admitted==1?s->materials+p->material:NULL;
}
#endif
