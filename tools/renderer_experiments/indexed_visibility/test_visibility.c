/* White-box include enables retain-overflow testing without public mutation API. */
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
static size_t allocations=0, attempts=0, fail_at=0;
static void *test_malloc(size_t n){ attempts++; if(fail_at && attempts==fail_at)return NULL; void *p=malloc(n); if(p)allocations++; return p;}
static void *test_calloc(size_t n,size_t z){ attempts++; if(fail_at && attempts==fail_at)return NULL; void *p=calloc(n,z); if(p)allocations++; return p;}
static void test_free(void *p){if(p){assert(allocations);allocations--;}free(p);}
#define malloc test_malloc
#define calloc test_calloc
#define free test_free
#include "visibility.c"
#include <assert.h>
#include <stdio.h>
static vis_budget ample(void){vis_budget b={{1000,100000,100000},{0,0,0}};return b;}
int main(void){
 double v[]={-1,-1,0, 1,-1,0, 0,1,0};uint32_t ix[]={0,1,2};
 vis_generation g={1,1,1},g2={2,1,1};vis_limits lim={2048,4095,1000000};
 vis_scene *s=NULL;vis_binding b={0};int hit=71;vis_budget budget=ample();
 assert(vis_scene_build(&s,v,3,ix,1,g,lim)==VIS_OK);
 assert(vis_scene_overlaps(s,s,sizeof(*s)));
 assert(vis_scene_overlaps(s,s->tri,sizeof(triangle)));
 assert(vis_scene_overlaps(s,s->tree,sizeof(node)));
 assert(!vis_scene_overlaps(s,v,sizeof(v)));
 assert(!vis_scene_overlaps(s,s,0));
 assert(vis_scene_bytes(s)==vis_scene_required_bytes_v3(3,1));assert(vis_scene_triangle_count(s)==1);assert(vis_scene_node_count(s)==1);
 assert(vis_binding_set(&b,s,g)==VIS_OK);assert(s->refs==2);
 /* New-scene alias is rejected before binding contents are dereferenced. */
 {
  unsigned char saved[sizeof(node)],after[sizeof(node)];
  memcpy(saved,s->tree,sizeof(saved));
  vis_binding fake={0};memcpy(s->tree,&fake,sizeof(fake));
  memcpy(after,s->tree,sizeof(after));size_t refs=s->refs;
  assert(vis_binding_set((vis_binding *)s->tree,s,g)==VIS_INVALID);
  assert(!memcmp(after,s->tree,sizeof(after)) && refs==s->refs);
  memcpy(s->tree,&s,sizeof(s));memcpy(after,s->tree,sizeof(after));
  assert(vis_scene_build((vis_scene **)s->tree,v,3,ix,1,g2,lim)==VIS_INVALID);
  assert(!memcmp(after,s->tree,sizeof(after)) && refs==s->refs);
  memcpy(s->tree,saved,sizeof(saved));
 }

 assert(vis_binding_set(&b,s,g)==VIS_OK);assert(s->refs==2);
 double a[]={0,0,-1},z[]={0,0,1};
 assert(vis_segment_occluded(&b,g,a,z,&budget,(int *)s->tree)==VIS_INVALID);
 assert(vis_segment_occluded(&b,g,a,z,(vis_budget *)s->tree,&hit)==VIS_INVALID);
 assert(vis_segment_occluded(&b,g,a,z,&budget,(int *)&budget)==VIS_INVALID);
 assert(vis_segment_occluded(&b,g,a,z,&budget,&hit)==VIS_OK && hit==1);
 assert(vis_segment_occluded(&b,g,z,a,&budget,&hit)==VIS_OK && hit==1);
 double miss0[]={2,2,-1},miss1[]={2,2,1};
 assert(vis_segment_occluded(&b,g,miss0,miss1,&budget,&hit)==VIS_OK && hit==0);
 double edge0[]={0,-1,-1},edge1[]={0,-1,1};
 assert(vis_segment_occluded(&b,g,edge0,edge1,&budget,&hit)==VIS_OK && hit==1);
 double near0[]={0,0,-0.5e-5};
 assert(vis_segment_occluded(&b,g,near0,z,&budget,&hit)==VIS_OK && hit==0);
 assert(vis_segment_occluded(&b,g,z,near0,&budget,&hit)==VIS_OK && hit==0);
 double endpoint[]={0,0,0};
 assert(vis_segment_occluded(&b,g,endpoint,z,&budget,&hit)==VIS_OK && hit==0);
 double parallel0[]={-0.5,0,0},parallel1[]={0.5,0,0};
 assert(vis_segment_occluded(&b,g,parallel0,parallel1,&budget,&hit)==VIS_OK && hit==0);
 hit=71;assert(vis_segment_occluded(&b,g2,a,z,&budget,&hit)==VIS_GENERATION && hit==71);
 vis_generation pose_mismatch={1,2,1},light_mismatch={1,1,2};
 assert(vis_segment_occluded(&b,pose_mismatch,a,z,&budget,&hit)==VIS_GENERATION && hit==71);
 assert(vis_segment_occluded(&b,light_mismatch,a,z,&budget,&hit)==VIS_GENERATION && hit==71);
 assert(vis_binding_set(&b,s,g2)==VIS_GENERATION);
 assert(vis_binding_set(&b,s,pose_mismatch)==VIS_GENERATION);
 assert(vis_binding_set(&b,s,light_mismatch)==VIS_GENERATION);
 assert(vis_segment_occluded(&b,g,a,a,&budget,&hit)==VIS_INVALID && hit==71);
 double bad[]={NAN,0,0};assert(vis_segment_occluded(&b,g,bad,z,&budget,&hit)==VIS_INVALID && hit==71);
 vis_scene *old=s;lim.max_bytes=1;assert(vis_scene_build(&s,v,3,ix,1,g2,lim)==VIS_CAPACITY && s==old);lim.max_bytes=1000000;
 lim.max_nodes=0;assert(vis_scene_build(&s,v,3,ix,1,g2,lim)==VIS_CAPACITY && s==old);lim.max_nodes=4095;
 assert(vis_scene_build(&s,v,3,ix,2049,g2,lim)==VIS_CAPACITY && s==old);
 assert(vis_scene_build(&s,v,4097,ix,1,g2,lim)==VIS_INVALID && s==old);
 ix[2]=3;assert(vis_scene_build(&s,v,3,ix,1,g2,lim)==VIS_INVALID && s==old);ix[2]=2;
 v[0]=NAN;assert(vis_scene_build(&s,v,3,ix,1,g2,lim)==VIS_INVALID && s==old);v[0]=-1;
 assert(vis_scene_build(&s,v,3,ix,1,g,lim)==VIS_GENERATION && s==old);
 uint32_t degenerate[]={0,0,0};assert(vis_scene_build(&s,v,3,degenerate,1,g2,lim)==VIS_INVALID && s==old);
 for(size_t f=1;f<=5;f++){size_t before=allocations;attempts=0;fail_at=f;
  assert(vis_scene_build(&s,v,3,ix,1,g2,lim)==VIS_ALLOC && s==old && allocations==before);
 }fail_at=0;
 size_t saved=s->refs;s->refs=SIZE_MAX;
 assert(vis_scene_retain(s)==VIS_CAPACITY && s->refs==SIZE_MAX);
 assert(vis_binding_set(&b,s,g)==VIS_CAPACITY && b.scene==s && s->refs==SIZE_MAX);s->refs=saved;
 assert(vis_scene_build(&s,v,3,ix,1,g2,lim)==VIS_OK && s!=old && old->refs==1);
 /* Binding in old-scene storage must not be mutated or released on rebind. */
 {
  unsigned char saved[sizeof(node)],after[sizeof(node)];
  memcpy(saved,old->tree,sizeof(saved));vis_binding fake={old,g};
  memcpy(old->tree,&fake,sizeof(fake));memcpy(after,old->tree,sizeof(after));
  size_t oldrefs=old->refs,newrefs=s->refs;
  assert(vis_binding_set((vis_binding *)old->tree,s,g2)==VIS_INVALID);
  vis_binding_clear((vis_binding *)old->tree);
  assert(!memcmp(after,old->tree,sizeof(after)) && oldrefs==old->refs && newrefs==s->refs);
  memcpy(old->tree,saved,sizeof(saved));
 }
 memset(v,0xff,sizeof(v));memset(ix,0xff,sizeof(ix)); /* source poison */
 assert(vis_segment_occluded(&b,g,a,z,&budget,&hit)==VIS_OK && hit==1);
 assert(vis_binding_set(&b,s,g)==VIS_GENERATION && b.scene==old);
 assert(vis_binding_set(&b,s,g2)==VIS_OK);
 vis_scene_release(s);s=NULL; /* binding alone owns generation 2 */
 assert(vis_segment_occluded(&b,g2,a,z,&budget,&hit)==VIS_OK && hit==1);
 for(int k=0;k<3;k++){vis_budget exhausted=ample();hit=71;
  if(k==0)exhausted.limit.rays=0;else if(k==1)exhausted.limit.node_visits=0;else exhausted.limit.triangle_tests=0;
  assert(vis_segment_occluded(&b,g2,a,z,&exhausted,&hit)==VIS_BUDGET && hit==71);
  assert(exhausted.used.rays<=exhausted.limit.rays && exhausted.used.node_visits<=exhausted.limit.node_visits && exhausted.used.triangle_tests<=exhausted.limit.triangle_tests);
 }
 vis_budget one=ample();one.limit.rays=1;
 assert(vis_segment_occluded(&b,g2,a,z,&one,&hit)==VIS_OK);hit=71;
 assert(vis_segment_occluded(&b,g2,a,z,&one,&hit)==VIS_BUDGET && hit==71 && one.used.rays==1);
 vis_binding_clear(&b);vis_binding_clear(&b);assert(!b.scene);
 /* Full capacity and actual BVH recursion/traversal. */
 double vv[]={-1,-1,0,1,-1,0,0,1,0};uint32_t *ii=malloc(2048*3*sizeof(*ii));assert(ii);
 for(size_t j=0;j<2048*3;j++)ii[j]=(uint32_t)(j%3);
 lim.max_bytes=524288;
 /* Storage contract v3 intentionally admits exact storage under this ceiling. */
 assert(vis_scene_build(&s,vv,3,ii,2048,g,lim)==VIS_OK && s);
 vis_scene_release(s);s=NULL;
 lim.max_bytes=1000000;
 assert(vis_scene_build(&s,vv,3,ii,2048,g,lim)==VIS_OK);free(ii);
 assert(vis_scene_node_count(s)==1023);assert(vis_binding_set(&b,s,g)==VIS_OK);
 budget=ample();assert(vis_segment_occluded(&b,g,a,z,&budget,&hit)==VIS_OK && hit==1 && budget.used.node_visits>1);
 printf("PASS: ownership, generations, poison, intersections, endpoint/grazing, capacities, overflow and cumulative budgets\n");
 printf("Requested heap bytes: scene=%zu triangle=%zu node=%zu; 1024 triangles=%zu; 2048 triangles=%zu; maximum exact nodes=1023\n",sizeof(vis_scene),sizeof(triangle),sizeof(node),vis_scene_required_bytes_v3(3,1024),vis_scene_required_bytes_v3(3,2048));
 /* Light-only replacement preserves immutable old-generation bindings. */
 vis_scene *light_old=s;vis_generation light_next={1,1,2};uint32_t light_indices[]={0,1,2};
 assert(vis_scene_build(&s,vv,3,light_indices,1,light_next,lim)==VIS_OK);
 assert(s!=light_old && light_old->refs==1 && b.scene==light_old);
 budget=ample();assert(vis_segment_occluded(&b,g,a,z,&budget,&hit)==VIS_OK && hit==1);
 hit=71;assert(vis_segment_occluded(&b,light_next,a,z,&budget,&hit)==VIS_GENERATION && hit==71);
 assert(vis_binding_set(&b,s,light_next)==VIS_OK);
 assert(vis_segment_occluded(&b,light_next,a,z,&budget,&hit)==VIS_OK && hit==1);
 vis_binding_clear(&b);vis_scene_release(s);assert(allocations==0);puts("Allocation-failure rollback and zero live allocations: PASS");return 0;
}
