#include "diagnostic_control.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
static const uint64_t boot=UINT64_C(0x123456789abcdef0);
static diagnostic_control c;
static diagnostic_session session=DIAGNOSTIC_SESSION_INIT;
static uint8_t id[16]={1},other[16]={2};
static struct {unsigned acquire,drain,work,restore,release,fail,cancel_at;bool borrow;} f;
static const uint8_t *query_id=id;
static diagnostic_control_snapshot get(void){return diagnostic_control_status(&c,boot,query_id);}
static void cancel_at(unsigned phase){
 if((f.cancel_at==6&&phase==1)||(f.cancel_at==7&&phase==3)){
  diagnostic_control_local_cancel(&c);(void)diagnostic_command_cancel(&session.command);
 }
 if(f.cancel_at==phase){diagnostic_control_snapshot s=diagnostic_control_cancel(&c,boot,id);assert(s.state==DC_CANCEL_REQUESTED);}
}
static void *acquire(void *ctx){(void)ctx;f.acquire++;cancel_at(1);return f.fail==1?NULL:&f;}
static bool drain(void *ctx){(void)ctx;f.drain++;assert(get().flags==DC_NOT_STARTED);cancel_at(2);return f.fail!=2;}
static bool work(void *ctx,void *owner){
 (void)ctx;assert(owner==&f);f.work++;f.borrow=true;
 assert(get().state==DC_RUNNING&&get().flags==0);cancel_at(3);
 if(f.fail==3)return false;
 f.borrow=false;return true;
}
static bool restore(void *ctx){
 (void)ctx;f.restore++;assert(!f.borrow);assert(!(get().flags&(DC_DRAINED|DC_HOME|DC_OWNER_RELEASED)));
 cancel_at(4);return f.fail!=4;
}
static void release(void *ctx,void *owner){
 (void)ctx;assert(owner==&f&&!f.borrow);assert(!(get().flags&DC_OWNER_RELEASED));
 assert((get().flags&(DC_DRAINED|DC_HOME|DC_BORROWS_RELEASED))==(DC_DRAINED|DC_HOME|DC_BORROWS_RELEASED));
 cancel_at(5);f.release++;
}
static const diagnostic_lifecycle_ops ops={NULL,acquire,drain,work,restore,release};
static void reset(void){
 session=(diagnostic_session)DIAGNOSTIC_SESSION_INIT;memset(&f,0,sizeof f);query_id=id;
 assert(diagnostic_control_init(&c,boot,true,&session));
}
static void expect(diagnostic_control_snapshot s,diagnostic_control_state st,unsigned flags,bool known){
 assert(s.state==st&&s.flags==flags&&s.known==known);
 assert(!((s.flags&DC_WORK_DONE)&&(s.flags&DC_NOT_STARTED)));
}
/* Force an input-task CANCEL and snapshots while UI is inside work. */
static pthread_barrier_t rendezvous;
static void barrier(void){int n=pthread_barrier_wait(&rendezvous);assert(n==0||n==PTHREAD_BARRIER_SERIAL_THREAD);}
static bool concurrent_work(void *ctx,void *owner){
 (void)ctx;assert(owner==&f);f.work++;barrier();barrier();
 expect(get(),DC_CANCEL_REQUESTED,0,true);return true;
}
static void *producer(void *arg){
 (void)arg;barrier();expect(diagnostic_control_status(&c,boot,other),DC_REFUSED,0,false);
 expect(diagnostic_control_cancel(&c,boot,other),DC_REFUSED,0,false);
 expect(diagnostic_control_cancel(&c,boot,id),DC_CANCEL_REQUESTED,0,true);
 for(unsigned i=0;i<50000;i++)expect(get(),DC_CANCEL_REQUESTED,0,true);
 barrier();return NULL;
}
int main(void){
 reset();uint8_t zero[16]={0};
 expect(diagnostic_control_request(&c,0,id),DC_REFUSED,0,false);
 expect(diagnostic_control_request(&c,boot,zero),DC_REFUSED,0,false);
 expect(diagnostic_control_status(&c,boot,id),DC_REFUSED,0,false);
 assert(!atomic_load(&c.used));
 for(unsigned cancel=0;cancel<=7;cancel++){
  reset();f.cancel_at=cancel;
  expect(diagnostic_control_request(&c,boot,id),DC_PENDING,DC_NOT_STARTED,true);
  expect(diagnostic_control_request(&c,boot,id),DC_PENDING,DC_NOT_STARTED,true);
  assert(diagnostic_control_pump(&c,&ops,true)==DC_OWNER_SAFE_END);
  unsigned flags=cancel&&(cancel<=2||cancel==6)?0x3e:0x1f;
  expect(get(),cancel?DC_CANCELLED:DC_COMPLETED,flags,true);
  assert(f.acquire==1&&f.drain==1&&f.restore==1&&f.release==1);
  assert(f.work==(cancel&&(cancel<=2||cancel==6)?0u:1u));
  expect(diagnostic_control_cancel(&c,boot,id),cancel?DC_CANCELLED:DC_COMPLETED,flags,true);
  expect(diagnostic_control_request(&c,boot,id),cancel?DC_CANCELLED:DC_COMPLETED,flags,true);
  assert(diagnostic_control_pump(&c,&ops,true)==DC_OWNER_NONE);
 }
 reset();(void)diagnostic_control_request(&c,boot,id);
 expect(diagnostic_control_cancel(&c,boot,id),DC_CANCEL_REQUESTED,DC_NOT_STARTED,true);
 assert(diagnostic_control_pump(&c,&ops,false)==DC_OWNER_REFUSED);
 expect(get(),DC_REFUSED,DC_NOT_STARTED,true);assert(!f.acquire&&!f.work&&!f.restore);
 reset();(void)diagnostic_control_request(&c,boot,id);
 (void)diagnostic_control_cancel(&c,boot,id);
 assert(diagnostic_control_pump(&c,&ops,true)==DC_OWNER_SAFE_END);
 expect(get(),DC_CANCELLED,0x3e,true);assert(!f.work&&f.release==1);
 for(unsigned failure=1;failure<=4;failure++){
  reset();f.fail=failure;(void)diagnostic_control_request(&c,boot,id);
  diagnostic_control_owner_result result=diagnostic_control_pump(&c,&ops,true);
  if(failure==1){assert(result==DC_OWNER_REFUSED);expect(get(),DC_REFUSED,DC_NOT_STARTED,true);}
  else{
   assert(result==DC_OWNER_TERMINAL&&session.lifecycle.owner==&f&&session.lifecycle.quarantined);
   assert(get().state==DC_QUARANTINED&&!(get().flags&DC_OWNER_RELEASED)&&!f.release);
   unsigned before=f.acquire;
   assert(diagnostic_control_pump(&c,&ops,true)==DC_OWNER_TERMINAL&&f.acquire==before);
  }
 }
 reset();
 for(unsigned n=0;n<DC_SLOTS;n++){
  uint8_t key[16]={(uint8_t)(n+1)};
  expect(diagnostic_control_request(&c,boot,key),DC_PENDING,DC_NOT_STARTED,true);
  assert(diagnostic_control_pump(&c,&ops,false)==DC_OWNER_REFUSED);
 }
 uint8_t ninth[16]={9};expect(diagnostic_control_request(&c,boot,ninth),DC_REFUSED,DC_NOT_STARTED,false);
 expect(diagnostic_control_status(&c,boot,ninth),DC_REFUSED,0,false);
 assert(atomic_load(&c.used)==DC_SLOTS&&!f.work);
 for(unsigned n=0;n<DC_SLOTS;n++){
  uint8_t key[16]={(uint8_t)(n+1)};expect(diagnostic_control_request(&c,boot,key),DC_REFUSED,DC_NOT_STARTED,true);
 }
 assert(diagnostic_control_pump(&c,&ops,true)==DC_OWNER_NONE&&!f.work);
 reset();assert(diagnostic_control_init(&c,boot,false,NULL));
 expect(diagnostic_control_request(&c,boot,id),DC_REFUSED,DC_NOT_STARTED,false);
 assert(!atomic_load(&c.used)&&diagnostic_control_pump(&c,NULL,true)==DC_OWNER_NONE);
 assert(diagnostic_control_init(&c,boot+1,true,&session));
 expect(diagnostic_control_request(&c,boot,id),DC_REFUSED,0,false);assert(!atomic_load(&c.used));
 reset();(void)diagnostic_command_request(&session.command);
 expect(diagnostic_control_request(&c,boot,id),DC_REFUSED,DC_NOT_STARTED,true);
 assert(atomic_load(&session.command.state)==DIAG_CMD_PENDING);
 /* Delayed cancellation of an old completed ID cannot affect a new run. */
 reset();(void)diagnostic_control_request(&c,boot,id);assert(diagnostic_control_pump(&c,&ops,true)==DC_OWNER_SAFE_END);
 (void)diagnostic_control_request(&c,boot,other);expect(diagnostic_control_cancel(&c,boot,id),DC_COMPLETED,0x1f,true);
 query_id=other;assert(diagnostic_control_pump(&c,&ops,true)==DC_OWNER_SAFE_END&&f.work==2);
 reset();(void)diagnostic_control_request(&c,boot,id);
 assert(!pthread_barrier_init(&rendezvous,NULL,2));pthread_t thread;assert(!pthread_create(&thread,NULL,producer,NULL));
 diagnostic_lifecycle_ops concurrent=ops;concurrent.work=concurrent_work;
 assert(diagnostic_control_pump(&c,&concurrent,true)==DC_OWNER_SAFE_END);
 assert(!pthread_join(thread,NULL));assert(!pthread_barrier_destroy(&rendezvous));
 expect(get(),DC_CANCELLED,0x1f,true);assert(f.work==1&&f.release==1);
 printf("diagnostic controller PASS slots=8 retained no replay coherent cancellation controller_bytes=%zu\n",sizeof c);
}
