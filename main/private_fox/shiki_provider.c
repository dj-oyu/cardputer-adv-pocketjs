#include "shiki_provider.h"
#ifdef FOX_SHIKI_PROVIDER
#include <assert.h>
#include <string.h>
typedef struct {
 lease_pool *pool; lease_ticket ticket; const fox_eye_snapshot *snapshot; bool live;
} fox_shiki_token;
static fox_shiki_token staged,pending,committed;
static render_scratch *workspace;
static lease_ticket cached;
static bool cache_valid,busy;
static void drop(fox_shiki_token*t){
 if(t->live){bool ok=lease_complete(t->pool,t->ticket);assert(ok);(void)ok;}
 memset(t,0,sizeof *t);
}
static bool cache_is(const fox_shiki_token*t){
 return cache_valid&&cached.pool==t->ticket.pool&&cached.serial==t->ticket.serial&&
 cached.key.slot==t->ticket.key.slot&&cached.key.generation==t->ticket.key.generation;
}
static bool prepare(const fox_shiki_token*t){
 if(cache_is(t))return true;
 cache_valid=false;
 if(!fox_render_prepare_snapshot(t->snapshot,workspace))return false;
 cached=t->ticket;cache_valid=true;return true;
}
bool fox_shiki_offer(lease_pool*p,lease_key key,render_scratch*w){
 if(!p||!w||busy||staged.live||pending.live||(workspace&&workspace!=w))return false;
 fox_shiki_token t={.pool=p};const void*payload;
 if(!lease_pin(p,key,&t.ticket,&payload))return false;
 t.snapshot=payload;t.live=true;workspace=w;busy=true;
 bool ok=prepare(&t);busy=false;
 if(!ok){drop(&t);if(!committed.live)workspace=NULL;return false;}
 staged=t;return true;
}
void fox_shiki_accept(void){
 assert(!busy);if(!staged.live||pending.live)return;
 pending=staged;memset(&staged,0,sizeof staged);
}
void fox_shiki_reject(void){
 assert(!busy);drop(&staged);cache_valid=false;
 if(!pending.live&&!committed.live)workspace=NULL;
}
bool fox_shiki_rows(uint16_t*dst,unsigned y,unsigned rows){
 if(!dst||!rows||rows>8||y>=135||rows>135-y||busy)return false;
 const fox_shiki_token*t=pending.live?&pending:&committed;
 if(!t->live)return false;
 busy=true;bool ok=prepare(t);
 for(unsigned done=0;ok&&done<rows;){
  unsigned n=rows-done;if(n>4)n=4;
  ok=fox_render_snapshot_rows(t->snapshot,workspace,dst+done*240,y+done,n);done+=n;
 }
 if(!ok)cache_valid=false;
 busy=false;return ok;
}
void fox_shiki_ack(bool success){
 assert(!busy);if(!pending.live||!success)return;
 drop(&committed);committed=pending;memset(&pending,0,sizeof pending);
}
void fox_shiki_close(void){
 assert(!busy);drop(&staged);drop(&pending);drop(&committed);
 workspace=NULL;cache_valid=false;memset(&cached,0,sizeof cached);
}
bool fox_shiki_active(void){return pending.live||committed.live;}
bool fox_shiki_pending(void){return pending.live;}
#endif
