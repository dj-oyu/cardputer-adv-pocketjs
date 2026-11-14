#include "bridge.h"
#include "render_adapter.h"
#include "shiki_provider.h"
#ifdef FOX_SHIKI_PROVIDER
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "lease_freertos.h"
static lease_mutex mutex;
static TaskHandle_t owner;
static void owner_check(void){if(owner)assert(owner==xTaskGetCurrentTaskHandle());}
static void lock(void*p){lease_mutex_lock(p);}
static void unlock(void*p){lease_mutex_unlock(p);}
#else
/* Host tests are single-owner; no background renderer may use this shim. */
static void owner_check(void){}
static void lock(void*p){(void)p;}
static void unlock(void*p){(void)p;}
#endif
/* Only this native entry may publish; it joins display work before touching
 * provider state. Kasane owns presentation and teardown ordering. */
extern bool pocket_proc_fox_publish(lease_pool*,lease_key,render_scratch*);
static render_scratch *scratch;
static fox_eye_snapshot *snapshots[2];
static lease_pool pool;
static lease_slot slots[2];
static lease_reader readers[4];
static bool opened,initialized,closed=true;
static unsigned allocation_attempts,accepted_sequence,presented_sequence;
static unsigned accepted_hidden,presented_hidden;
static void *allocate(size_t bytes){
 allocation_attempts++;
#ifdef ESP_PLATFORM
 return heap_caps_calloc(1,bytes,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
#else
 return calloc(1,bytes);
#endif
}
static void release(void){
#ifdef FOX_STATIC_WEIGHT_BIN
 if(scratch)fox_scene_static_dispose(&scratch->workspace);
#endif
 free(scratch);scratch=NULL;
 for(unsigned i=0;i<2;i++){free(snapshots[i]);snapshots[i]=NULL;slots[i].payload=NULL;}
}
static bool init(void){
 if(initialized)return true;
 scratch=allocate(sizeof *scratch);if(!scratch)goto failed;
#ifdef FOX_STATIC_WEIGHT_BIN
 if(!fox_scene_static_load_embedded(&scratch->workspace))goto failed;
#endif
 for(unsigned i=0;i<2;i++){
  snapshots[i]=allocate(sizeof *snapshots[i]);if(!snapshots[i])goto failed;
  slots[i].payload=snapshots[i];
 }
#ifdef ESP_PLATFORM
 lease_mutex_init(&mutex);
 lease_init(&pool,slots,2,readers,4,&mutex,lock,unlock);
#else
 lease_init(&pool,slots,2,readers,4,NULL,lock,unlock);
#endif
 initialized=true;return true;
failed:
 release();return false;
}
/* Acknowledged means Kasane accepted completion, including its no-damage ACK.
 * It is not proof that bytes reached an LCD. Reset/close is not an ACK. */
static void refresh(void){
 if(initialized&&accepted_sequence!=presented_sequence&&fox_shiki_active()&&!fox_shiki_pending()){
  presented_sequence=accepted_sequence;presented_hidden=accepted_hidden;
 }
}
bool fox_composed_open(void){
 owner_check();if(opened||initialized||fox_shiki_active())return false;
#ifdef ESP_PLATFORM
 owner=xTaskGetCurrentTaskHandle();
#endif
 opened=true;closed=false;accepted_sequence=presented_sequence=0;
 accepted_hidden=presented_hidden=0;allocation_attempts=0;return true;
}
bool fox_composed_commit(const FoxEyeControls*c,uint32_t*out){
 owner_check();refresh();
 if(!opened||closed||!c||!out||fox_shiki_pending()||accepted_sequence==UINT32_MAX||!init())return false;
 lease_key key;void*payload;if(!lease_acquire(&pool,&key,&payload))return false;
 fox_eye_snapshot*s=payload;
 if(!fox_eye_snapshot_build(s,c)){bool ok=lease_abort(&pool,key);assert(ok);(void)ok;return false;}
 unsigned hidden=0;for(unsigned i=0;i<sizeof s->hidden/sizeof s->hidden[0];i++)hidden+=s->hidden[i]!=0;
 if(!lease_publish(&pool,key)){bool ok=lease_abort(&pool,key);assert(ok);(void)ok;return false;}
 if(!pocket_proc_fox_publish(&pool,key,scratch)){
  bool ok=lease_retire(&pool,key);assert(ok);(void)ok;return false;
 }
 /* Provider has pinned the token; no new readers may acquire this slot. */
 bool retired=lease_retire(&pool,key);assert(retired);(void)retired;
 accepted_hidden=hidden;*out=++accepted_sequence;refresh();return true;
}
void fox_composed_close(void){
 owner_check();refresh();closed=true;
 if(initialized)lease_close(&pool);
 /* Do not close provider here: Kasane must join/unregister its readers first. */
}
bool fox_composed_finish(void){
 owner_check();fox_composed_close();
 if(initialized){
  if(!lease_drained(&pool))return false;
#ifdef ESP_PLATFORM
  vSemaphoreDelete(mutex.handle);memset(&mutex,0,sizeof mutex);
#endif
  release();initialized=false;
 }
 opened=false;
#ifdef ESP_PLATFORM
 owner=NULL;
#endif
 return true;
}
bool fox_composed_present(void){owner_check();refresh();return !fox_shiki_pending();}
bool fox_composed_owned(void){return false;}
bool fox_composed_pending(void){owner_check();refresh();return initialized&&fox_shiki_pending();}
bool fox_composed_shutdown_pending(void){owner_check();return closed&&initialized;}
uint32_t fox_composed_hash(void){owner_check();refresh();return presented_sequence;}
unsigned fox_composed_accepted(void){owner_check();return accepted_sequence;}
unsigned fox_composed_presented(void){owner_check();refresh();return presented_sequence;}
unsigned fox_composed_hidden(void){owner_check();refresh();return presented_hidden;}
unsigned fox_composed_allocation_attempts(void){owner_check();return allocation_attempts;}
unsigned fox_composed_mode(int mode){owner_check();(void)mode;return 0;}
#endif
