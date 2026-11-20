#ifndef HONE_ALPHA_PACKET_CANDIDATE_H
#define HONE_ALPHA_PACKET_CANDIDATE_H
#ifdef __FAST_MATH__
#error "Alpha preflight requires finite-value and ordered floating-point semantics"
#endif
/* Optional native host packet lane. Existing opaque packet ABI is untouched.
 * No renderer admission: continuous alpha/mask semantics must be consumed by
 * an explicit alpha raster;
  never submit these packets to opaque depth writes. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef struct {float x,y,z,mask;
 } hone_alpha_vertex;

typedef struct {hone_alpha_vertex v[3];
 float rgb[3],alpha,mask_low,mask_high;
 uint32_t order,node;
 unsigned mask_enabled;
 } hone_alpha_packet;

typedef struct {hone_alpha_packet *packets;
 size_t count,capacity;
 const void *owner;
 uint32_t generation;
 int pinned,ready;
 uint64_t revision;
 } hone_alpha_snapshot;

static inline int hone_alpha_disjoint(const void*a,size_t an,const void*b,size_t bn){
 uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
 return an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);

}
/* Closed interval, at most five vertices;
  candidate-local bounded work. */
static inline int hone_alpha_clip(const hone_alpha_packet *p,float near,float far,hone_alpha_packet result[3]){
 if(!p||!result||!isfinite(near)||!isfinite(far)||near>=far||!isfinite(p->alpha)||p->alpha<0||p->alpha>1)return -1;

 if(p->mask_enabled>1||(p->mask_enabled&&(!isfinite(p->mask_low)||!isfinite(p->mask_high)||p->mask_low>p->mask_high)))return -1;

 for(unsigned k=0;k<3;k++)if(!isfinite(p->rgb[k])||p->rgb[k]<0||p->rgb[k]>1)return -1;

 hone_alpha_vertex a[6],b[6];
 unsigned n=3;
 memcpy(a,p->v,sizeof p->v);

 for(unsigned i=0;i<n;i++)if(!isfinite(a[i].x)||!isfinite(a[i].y)||!isfinite(a[i].z)||!isfinite(a[i].mask))return -1;

 for(unsigned pass=0;pass<2&&n;pass++){
  float plane=pass?far:near;
 unsigned out=0;

 hone_alpha_vertex prev=a[n-1];
 int pi=pass?prev.z<=plane:prev.z>=plane;

  for(unsigned i=0;i<n;i++){
   hone_alpha_vertex cur=a[i];
 int ci=pass?cur.z<=plane:cur.z>=plane;

   if(ci!=pi){double t=((double)plane-prev.z)/((double)cur.z-prev.z);

 hone_alpha_vertex q={(float)(prev.x+t*((double)cur.x-prev.x)),(float)(prev.y+t*((double)cur.y-prev.y)),plane,(float)(prev.mask+t*((double)cur.mask-prev.mask))};

 if(out>=6||!isfinite(q.x)||!isfinite(q.y)||!isfinite(q.mask))return -1;
 b[out++]=q;
 }
   if(ci){if(out>=6)return -1;
 b[out++]=cur;
 }prev=cur;
 pi=ci;

  }
  n=0;
 for(unsigned i=0;i<out;i++)if(!n||memcmp(&b[i],&a[n-1],sizeof b[i]))a[n++]=b[i];

  if(n>1&&!memcmp(a,a+n-1,sizeof *a))n--;

 if(n>5)return -1;

 }
 if(n<3)return 0;

 for(unsigned i=1;i+1<n;i++){result[i-1]=*p;
 result[i-1].v[0]=a[0];
 result[i-1].v[1]=a[i];
 result[i-1].v[2]=a[i+1];
 }
 return (int)n-2;

}
/* Append to private candidate only. Full preflight before any packet mutation.
 * The owner holds immutable disjoint source and candidate for both passes. */
static inline int hone_alpha_append(hone_alpha_snapshot*s,const void*owner,uint32_t generation,const hone_alpha_packet*input,size_t count,float near,float far){
 if(!s||!s->packets||!owner||owner!=s->owner||!generation||generation!=s->generation||s->ready||s->pinned||s->count>s->capacity||!input||!count||count>SIZE_MAX/sizeof *input||s->capacity>SIZE_MAX/sizeof *input)return 0;

 if(!hone_alpha_disjoint(s,sizeof *s,s->packets,s->capacity*sizeof *input)||!hone_alpha_disjoint(input,count*sizeof *input,s,sizeof *s)||!hone_alpha_disjoint(input,count*sizeof *input,s->packets,s->capacity*sizeof *input))return 0;

 size_t needed=0;

 hone_alpha_packet temp[3];

 for(size_t i=0;i<count;i++){int n=hone_alpha_clip(input+i,near,far,temp);

 if(n<0||(size_t)n>s->capacity-s->count-needed)return 0;
 needed+=(size_t)n;
 }
 size_t at=s->count;
 for(size_t i=0;i<count;i++){int n=hone_alpha_clip(input+i,near,far,temp);

 if(n<0)return 0;
 memcpy(s->packets+at,temp,(size_t)n*sizeof *temp);
 at+=(size_t)n;
 }s->count=at;
 return 1;

}
/* Ready snapshots are immutable and pinned snapshots cannot be replaced.
 * A failure never changes the existing visible snapshot. Both buffers disjoint. */
static inline int hone_alpha_publish(hone_alpha_snapshot*ready,hone_alpha_snapshot*candidate,const void*owner,uint32_t generation){
 if(!ready||!candidate||ready==candidate||!owner||ready->owner!=owner||candidate->owner!=owner||candidate->generation!=generation||!generation||candidate->ready||candidate->pinned||ready->pinned||ready->revision==UINT64_MAX||candidate->count>candidate->capacity||!ready->packets||!candidate->packets||ready->capacity>SIZE_MAX/sizeof(hone_alpha_packet)||candidate->capacity>SIZE_MAX/sizeof(hone_alpha_packet))return 0;

 if(!hone_alpha_disjoint(ready,sizeof *ready,candidate,sizeof *candidate)||!hone_alpha_disjoint(ready,sizeof *ready,ready->packets,ready->capacity*sizeof(hone_alpha_packet))||!hone_alpha_disjoint(candidate,sizeof *candidate,candidate->packets,candidate->capacity*sizeof(hone_alpha_packet))||!hone_alpha_disjoint(ready,sizeof *ready,candidate->packets,candidate->capacity*sizeof(hone_alpha_packet))||!hone_alpha_disjoint(candidate,sizeof *candidate,ready->packets,ready->capacity*sizeof(hone_alpha_packet)))return 0;

 if(!hone_alpha_disjoint(ready->packets,ready->capacity*sizeof(hone_alpha_packet),candidate->packets,candidate->capacity*sizeof(hone_alpha_packet)))return 0;

 hone_alpha_snapshot old=*ready;
 *ready=*candidate;
 ready->revision=old.revision+1;
 ready->ready=1;
 *candidate=old;
 candidate->ready=0;
 candidate->count=0;
 candidate->generation=generation;
 return 1;

}
#endif
