#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <limits.h>
#include "ayatsuri/ayatsuri_model_skin.h"
#include "ayatsuri/ayatsuri_model_provider.h"
#include "ui/kasane/ksn_mesh_depth.h"
#define COUNT 23
#define CAP 12288
static size_t live,peak,calls;
static struct {void*p;size_t n;} ledger[64];
void *__real_malloc(size_t);void *__real_calloc(size_t,size_t);void __real_free(void*);
static void record(void*p,size_t n){assert(p);for(unsigned j=0;j<64;j++)if(!ledger[j].p){ledger[j].p=p;ledger[j].n=n;live+=n;if(live>peak)peak=live;return;}assert(0);}
void *__wrap_malloc(size_t n){calls++;void*p=__real_malloc(n);if(p)record(p,n);return p;}
void *__wrap_calloc(size_t n,size_t s){calls++;void*p=__real_calloc(n,s);if(p)record(p,n*s);return p;}
void __wrap_free(void*p){if(!p)return;for(unsigned j=0;j<64;j++)if(ledger[j].p==p){live-=ledger[j].n;ledger[j].p=NULL;__real_free(p);return;}assert(0);}
static const float cam[9]={0,0,400,0,0,0,1,120,67},light[6]={0,0,1,1,1,1};
/* Images belong to host oracle only, not adapter runtime. */
static uint16_t pixels[240*135],expected[240*135];
static float depthwork[240*8];
struct Adapter {ayatsuri_model_instance candidate;ksn_mesh_snapshot ready,pending;bool busy,ready_pinned,pending_pinned;};
struct Work {ayatsuri_skin_result one[32];};
static void geometry(unsigned frame,unsigned node,ayatsuri_skin_result*out){
 for(unsigned v=0;v<32;v++){
  double a=v*2*3.141592653589793/32;out[v]=(ayatsuri_skin_result){{(float)(8*cos(a)+3*sin(frame*.2)),(float)(8*sin(a)),(float)(2*sin(frame*.1+node))},{0,0,1}};
  /* Test-only reflected geometry exercises empty face selection. */
  if(frame==UINT_MAX)out[v].position[0]=-out[v].position[0];
 }
}
struct Provider {unsigned frame,calls;int fail_node;ayatsuri_model_instance *candidate;const void *owner;};
static bool provide(void*opaque,unsigned node,unsigned count,ayatsuri_skin_result*slot){
 struct Provider*p=opaque;++p->calls;
 assert(ayatsuri_model_instance_evaluate(p->candidate,p->owner,1,cam,light)==AYATSURI_MODEL_BUSY);
 assert(ayatsuri_model_instance_dispose(p->candidate,p->owner,1)==AYATSURI_MODEL_BUSY);
 if(node<1||node>COUNT||count!=32||(int)node==p->fail_node)return false;
 geometry(p->frame,node,slot);
 if(-(int)node==p->fail_node)slot[31].normal[2]=0;
 return true;
}
/* Snapshot-only publication. Candidate actor remains private. Only complete frame snapshots publish;
 * partial actor packets after a failure are disposable. */
static int transaction(struct Adapter*a,struct Work*w,const void*owner,unsigned gen,unsigned frame,int fail_node,int bad_projection,size_t capacity){
 if(a->busy||a->pending_pinned||capacity>CAP)return 0;
 a->busy=true;
 a->pending.packets.size=0;a->pending.packets.packets=0;a->pending.work=0;a->pending.packets.capacity=capacity;
 float camera[9];memcpy(camera,cam,sizeof camera);if(bad_projection)camera[7]=4096;
 struct Provider provider={frame,0,fail_node,&a->candidate,owner};
 if(ayatsuri_model_candidate_evaluate_provider(&a->candidate,owner,gen,camera,light,provide,&provider,w->one)!=AYATSURI_MODEL_OK){a->busy=false;return 0;}
 assert(provider.calls==COUNT);
 uint32_t cursor=0;ayatsuri_model_packet packet;ayatsuri_model_status status;
 while((status=ayatsuri_model_instance_next(&a->candidate,owner,gen,&cursor,&packet))==AYATSURI_MODEL_OK){
  ayatsuri_model_mesh_view view;
  if(packet.kind!=4||ayatsuri_model_instance_mesh_view(&a->candidate,owner,gen,packet.node_id,&view)!=AYATSURI_MODEL_OK||!ksn_mesh_append(&a->pending,view.patch,view.depth,packet.node_id)){a->busy=false;return 0;}
 }
 if(status!=AYATSURI_MODEL_DONE){a->busy=false;return 0;}
 ksn_mesh_snapshot swap=a->ready;a->ready=a->pending;a->pending=swap;a->pending_pinned=a->ready_pinned;a->ready_pinned=false;a->busy=false;return 1;
}
static uint64_t digest(const ksn_mesh_snapshot*s){uint64_t h=1469598103934665603ULL;for(size_t j=0;j<s->packets.size;j++)h=(h^s->packets.data[j])*1099511628211ULL;return h;}
static void render(const ksn_mesh_snapshot*s,uint16_t*out,unsigned band,unsigned rows){
 for(unsigned j=0;j<240*135;j++)out[j]=17;
 for(unsigned y=0;y<135;y+=band)assert(ksn_mesh_render(s,out+y*240,y,135-y<band?135-y:band,depthwork,rows));
}
static void entry_rejections(struct Adapter*a,struct Work*w,const void*owner){
 assert(transaction(a,w,owner,1,0,0,0,CAP));
 ayatsuri_model_instance before=a->candidate;
 void*saved=malloc(before.arena_bytes);assert(saved);memcpy(saved,before.arena,before.arena_bytes);
 struct Provider provider={0,0,0,&a->candidate,owner};
#define REJECT_SLOT(ptr,fn) do { \
 assert(ayatsuri_model_candidate_evaluate_provider(&a->candidate,owner,1,cam,light,fn,&provider,ptr)==AYATSURI_MODEL_INVALID); \
 assert(!provider.calls&&!memcmp(&before,&a->candidate,sizeof before)&&!memcmp(saved,before.arena,before.arena_bytes)); \
} while(0)
 REJECT_SLOT(NULL,provide);
 REJECT_SLOT((ayatsuri_skin_result*)((unsigned char*)w->one+1),provide);
 REJECT_SLOT((ayatsuri_skin_result*)before.arena,provide);
 REJECT_SLOT((ayatsuri_skin_result*)&a->candidate,provide);
 REJECT_SLOT((ayatsuri_skin_result*)before.model,provide);
 REJECT_SLOT((ayatsuri_skin_result*)before.model->data,provide);
 REJECT_SLOT((ayatsuri_skin_result*)cam,provide);
 REJECT_SLOT((ayatsuri_skin_result*)light,provide);
 REJECT_SLOT(w->one,NULL);
#undef REJECT_SLOT
 free(saved);
}
static void unsupported_metadata(const char*path,const char*variant){
 char name[1024];const char*slash=strrchr(path,'/');assert(slash);
 int written=snprintf(name,sizeof name,"%.*s/skin-%s.aynm",(int)(slash-path),path,variant);assert(written>0&&(size_t)written<sizeof name);
 FILE*f=fopen(name,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long size=ftell(f);assert(size>0);rewind(f);
 unsigned char*blob=malloc((size_t)size);assert(fread(blob,1,(size_t)size,f)==(size_t)size);fclose(f);
 ayatsuri_model m;assert(ayatsuri_model_open(blob,(size_t)size,&m)==AYATSURI_MODEL_OK);
 size_t bytes=ayatsuri_model_instance_bytes(&m);void*arena=malloc(bytes),*saved=malloc(bytes);unsigned owner=1;ayatsuri_model_instance i;
 assert(ayatsuri_model_instance_init(&m,arena,bytes,&owner,1,&i)==AYATSURI_MODEL_OK);
 assert(ayatsuri_model_instance_evaluate(&i,&owner,1,cam,light)==AYATSURI_MODEL_OK);
 ayatsuri_model_instance before=i;memcpy(saved,arena,bytes);ayatsuri_skin_result slot[32];struct Provider provider={0,0,0,&i,&owner};
 assert(ayatsuri_model_candidate_evaluate_provider(&i,&owner,1,cam,light,provide,&provider,slot)==AYATSURI_MODEL_INVALID);
 assert(!provider.calls&&!memcmp(&i,&before,sizeof i)&&!memcmp(arena,saved,bytes));
 assert(ayatsuri_model_instance_dispose(&i,&owner,1)==AYATSURI_MODEL_OK);free(saved);free(arena);free(blob);
}
static void *run(void*path){
 unsupported_metadata(path,"holes");unsupported_metadata(path,"sided");
 FILE*f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);size_t bytes=(size_t)ftell(f);rewind(f);uint8_t*blob=malloc(bytes);assert(fread(blob,1,bytes,f)==bytes);fclose(f);
 ayatsuri_model model;assert(ayatsuri_model_open(blob,bytes,&model)==AYATSURI_MODEL_OK);size_t n=ayatsuri_model_instance_bytes(&model);
 struct Adapter*a=calloc(1,sizeof *a);struct Work*w=malloc(sizeof *w);unsigned owner=1;
 void*arena=malloc(n);assert(ayatsuri_model_instance_init(&model,arena,n,&owner,1,&a->candidate)==AYATSURI_MODEL_OK);
 a->ready=(ksn_mesh_snapshot){.packets={malloc(CAP),CAP,0,0},.work_limit=1000000,.enabled=true};
 a->pending=(ksn_mesh_snapshot){.packets={malloc(CAP),CAP,0,0},.work_limit=1000000,.enabled=true};
 size_t adapter_live=live;
 /* Reference is intentionally all-at-once and excluded from adapter accounting. */
 ayatsuri_model_instance ref;void*refarena=malloc(n);assert(ayatsuri_model_instance_init(&model,refarena,n,&owner,1,&ref)==AYATSURI_MODEL_OK);
 ayatsuri_skin_result*all=malloc(COUNT*32*sizeof *all);ayatsuri_model_mesh_geometry desc[COUNT];
 ksn_mesh_snapshot oracle={.packets={malloc(CAP),CAP,0,0},.work_limit=1000000,.enabled=true};
 entry_rejections(a,w,&owner);
 size_t warm=calls,maxpacket=0;unsigned fails=0;
 for(unsigned frame=0;frame<24;frame++){
  assert(transaction(a,w,&owner,1,frame,0,0,CAP));
  for(unsigned id=1;id<=COUNT;id++){geometry(frame,id,all+(id-1)*32);desc[id-1]=(ayatsuri_model_mesh_geometry){id,32,all+(id-1)*32};}
  assert(ayatsuri_model_instance_evaluate_meshes(&ref,&owner,1,cam,light,desc,COUNT)==AYATSURI_MODEL_OK);
  oracle.packets.size=0;oracle.packets.packets=0;oracle.work=0;
  for(unsigned id=1;id<=COUNT;id++){ayatsuri_model_mesh_view v;assert(ayatsuri_model_instance_mesh_view(&ref,&owner,1,id,&v)==AYATSURI_MODEL_OK);assert(ksn_mesh_append(&oracle,v.patch,v.depth,id));}
  assert(a->ready.packets.size==oracle.packets.size&&!memcmp(a->ready.packets.data,oracle.packets.data,oracle.packets.size));
  render(&oracle,expected,8,8);
  for(unsigned band=4;band<=8;band+=4)for(unsigned rows=1;rows<=8;rows+=7){render(&a->ready,pixels,band,rows);assert(!memcmp(pixels,expected,sizeof pixels));}
  if(a->ready.packets.size>maxpacket)maxpacket=a->ready.packets.size;
  uint64_t old=digest(&a->ready);size_t used=a->ready.packets.size;ksn_mesh_snapshot before=a->ready;
  for(int id=1;id<=COUNT;id++){assert(!transaction(a,w,&owner,1,frame+1,id,0,CAP));assert(!memcmp(&before,&a->ready,sizeof before)&&!memcmp(a->ready.packets.data,oracle.packets.data,used)&&digest(&a->ready)==old&&a->ready.packets.size==used);fails++;}
  for(int id=1;id<=COUNT;id++){assert(!transaction(a,w,&owner,1,frame+1,-id,0,CAP));assert(!memcmp(&before,&a->ready,sizeof before)&&!memcmp(a->ready.packets.data,oracle.packets.data,used));fails++;}
  assert(!transaction(a,w,&owner,1,frame+1,0,1,CAP));assert(!memcmp(&before,&a->ready,sizeof before)&&!memcmp(a->ready.packets.data,oracle.packets.data,used)&&digest(&a->ready)==old);fails++;
  assert(!transaction(a,w,&owner,1,frame+1,0,0,used-1));assert(!memcmp(&before,&a->ready,sizeof before)&&!memcmp(a->ready.packets.data,oracle.packets.data,used)&&digest(&a->ready)==old);fails++;
  assert(!transaction(a,w,&owner,2,frame+1,0,0,CAP));assert(!memcmp(&before,&a->ready,sizeof before)&&!memcmp(a->ready.packets.data,oracle.packets.data,used)&&digest(&a->ready)==old);fails++;
  unsigned stranger=0;assert(!transaction(a,w,&stranger,1,frame+1,0,0,CAP));assert(!memcmp(&before,&a->ready,sizeof before)&&!memcmp(a->ready.packets.data,oracle.packets.data,used)&&digest(&a->ready)==old);fails++;
  assert(!transaction(a,w,&owner,1,frame+1,0,0,CAP+1));assert(!memcmp(&before,&a->ready,sizeof before)&&!memcmp(a->ready.packets.data,oracle.packets.data,used)&&digest(&a->ready)==old);fails++;
  a->busy=true;assert(!transaction(a,w,&owner,1,frame+1,0,0,CAP));a->busy=false;
  assert(transaction(a,w,&owner,1,frame,0,0,CAP));assert(digest(&a->ready)==old);
 }
 /* A transport-owned old frame cannot be reused as the next candidate. */
 a->ready_pinned=true;ksn_mesh_snapshot held=a->ready;uint64_t held_hash=digest(&held);
 assert(transaction(a,w,&owner,1,24,0,0,CAP));assert(a->pending_pinned);
 assert(!transaction(a,w,&owner,1,25,0,0,CAP));assert(digest(&held)==held_hash);
 render(&held,pixels,8,8);assert(!memcmp(pixels,expected,sizeof pixels));
 a->pending_pinned=false;assert(transaction(a,w,&owner,1,23,0,0,CAP));
 assert(ayatsuri_model_instance_evaluate(&a->candidate,&owner,1,cam,light)==AYATSURI_MODEL_OK&&!a->candidate.mesh_overridden);
 assert(transaction(a,w,&owner,1,23,0,0,CAP));
 assert(transaction(a,w,&owner,1,UINT_MAX,0,0,CAP));assert(a->ready.packets.size==0);
 render(&a->ready,pixels,4,1);for(unsigned j=0;j<240*135;j++)assert(pixels[j]==17);
 uint8_t dirty[COUNT+1]={0};float poses[(COUNT+1)*9]={0};uint32_t colors[COUNT+1]={0};double opacity[COUNT+1]={0};
 ayatsuri_model_targets targets={dirty,poses,colors,opacity,COUNT+1};dirty[0]=AYATSURI_TARGET_HIDDEN;
 assert(ayatsuri_model_instance_targets(&a->candidate,&owner,1,&targets)==AYATSURI_MODEL_OK);
 assert(transaction(a,w,&owner,1,23,0,0,CAP));assert(a->ready.packets.size==0);
 dirty[0]=AYATSURI_TARGET_VISIBLE;assert(ayatsuri_model_instance_targets(&a->candidate,&owner,1,&targets)==AYATSURI_MODEL_OK);
 assert(transaction(a,w,&owner,1,23,0,0,CAP));
 assert(calls==warm);
 /* Frame copy outlives model instance and borrowed cold bytes. */
 assert(ayatsuri_model_instance_dispose(&a->candidate,&owner,1)==AYATSURI_MODEL_OK);free(arena);
 assert(ayatsuri_model_instance_dispose(&ref,&owner,1)==AYATSURI_MODEL_OK);free(refarena);free(blob);
 memset(w,0,sizeof *w);free(w);free(all);
 render(&a->ready,pixels,4,1);assert(!memcmp(pixels,expected,sizeof pixels));
 printf("PASS frames=24 conditions=4 rollback_failures=%u snapshot_bytes=%zu actor_arena=%zu old_plus_candidate_snapshots=%u producer=%zu adapter_state=%zu cold_blob=%zu adapter_heap_peak=%zu host_total_heap_peak=%zu warm_allocations=%zu\n",fails,maxpacket,n,2*CAP,sizeof(struct Work),sizeof(struct Adapter),bytes,adapter_live,peak,calls-warm);
 free(a->ready.packets.data);free(a->pending.packets.data);free(oracle.packets.data);free(a);assert(!live);return NULL;
}
int main(int argc,char**argv){assert(argc==2);size_t stackbytes=1024*1024;void*stack=NULL;assert(!posix_memalign(&stack,4096,stackbytes));memset(stack,0xa5,stackbytes);pthread_attr_t attr;assert(!pthread_attr_init(&attr));assert(!pthread_attr_setstack(&attr,stack,stackbytes));pthread_t t;assert(!pthread_create(&t,&attr,run,argv[1]));assert(!pthread_join(t,NULL));size_t first=0;while(first<stackbytes&&((unsigned char*)stack)[first]==0xa5)first++;printf("worker_stack_painted_highwater=%zu host_heap_plus_stack_highwater_upper_envelope=%zu test_static_images_depth=%zu\n",stackbytes-first,peak+stackbytes-first,sizeof pixels+sizeof expected+sizeof depthwork);pthread_attr_destroy(&attr);__real_free(stack);}
