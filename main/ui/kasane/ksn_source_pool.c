#include "ksn_source_pool.h"
#include <stdint.h>

#define PINS 0xffu
#define RETIRED 0x100u
#define PUBLISHED 0x200u
#define WRITING 0x400u
#define MAX_REVISION (UINT32_MAX>>2)

static void *slot_data(const ksn_source_pool *pool,unsigned slot){
    return pool->storage+pool->stride*slot;
}
ksn_result ksn_source_pool_init(ksn_source_pool *pool,void *storage,
                                size_t storage_bytes,size_t stride){
    if(!pool||!storage||!stride||stride>SIZE_MAX/KSN_SOURCE_POOL_SLOTS||
       storage_bytes<stride*KSN_SOURCE_POOL_SLOTS)return KSN_INVALID;
    pool->storage=storage;pool->stride=stride;
    for(unsigned i=0;i<KSN_SOURCE_POOL_SLOTS;i++)atomic_init(&pool->state[i],0);
    atomic_init(&pool->current,0);atomic_init(&pool->revision,0);
    atomic_init(&pool->skipped,0);atomic_init(&pool->writer_busy,false);
    return KSN_OK;
}
ksn_result ksn_source_pool_begin(ksn_source_pool *pool,ksn_source_write *write){
    if(!pool||!pool->storage||!write||write->active)return KSN_INVALID;
    bool expected=false;
    if(!atomic_compare_exchange_strong_explicit(&pool->writer_busy,&expected,true,
        memory_order_acq_rel,memory_order_acquire))return KSN_BUSY;
    unsigned token=atomic_load_explicit(&pool->current,memory_order_acquire);
    unsigned current=token&3u;
    for(unsigned i=0;i<KSN_SOURCE_POOL_SLOTS;i++){
        if(token&&i==current)continue;
        unsigned state=atomic_load_explicit(&pool->state[i],memory_order_acquire);
        if((state!=0&&state!=RETIRED&&state!=(PUBLISHED|RETIRED))||
           !atomic_compare_exchange_strong_explicit(&pool->state[i],&state,WRITING,
               memory_order_acq_rel,memory_order_acquire))continue;
        *write=(ksn_source_write){.pool=pool,.data=slot_data(pool,i),
                                  .slot=(uint8_t)i,.active=true};
        return KSN_OK;
    }
    atomic_fetch_add_explicit(&pool->skipped,1,memory_order_relaxed);
    atomic_store_explicit(&pool->writer_busy,false,memory_order_release);
    return KSN_BUSY;
}
ksn_result ksn_source_pool_publish(ksn_source_write *write,uint32_t *revision){
    if(!write||!write->active||!write->pool||write->slot>=KSN_SOURCE_POOL_SLOTS)
        return KSN_INVALID;
    ksn_source_pool *pool=write->pool;
    if(atomic_load_explicit(&pool->state[write->slot],memory_order_acquire)!=WRITING)
        return KSN_STALE;
    uint32_t next=atomic_load_explicit(&pool->revision,memory_order_relaxed);
    if(next==MAX_REVISION)return KSN_LIMIT;
    next++;
    atomic_store_explicit(&pool->revision,next,memory_order_relaxed);
    atomic_store_explicit(&pool->state[write->slot],PUBLISHED,memory_order_release);
    unsigned token=(next<<2)|write->slot;
    unsigned old=atomic_exchange_explicit(&pool->current,token,memory_order_acq_rel);
    if(old){
        unsigned slot=old&3u;
        /* One RMW retires the old generation without waiting for readers.
         * Its pin count remains intact; no new reader can pin RETIRED. */
        atomic_fetch_or_explicit(&pool->state[slot],RETIRED,memory_order_acq_rel);
    }
    atomic_store_explicit(&pool->writer_busy,false,memory_order_release);
    if(revision)*revision=next;
    *write=(ksn_source_write){0};
    return KSN_OK;
}
ksn_result ksn_source_pool_cancel(ksn_source_write *write){
    if(!write||!write->active||!write->pool||write->slot>=KSN_SOURCE_POOL_SLOTS)
        return KSN_INVALID;
    ksn_source_pool *pool=write->pool;
    unsigned expected=WRITING;
    if(!atomic_compare_exchange_strong_explicit(&pool->state[write->slot],
        &expected,RETIRED,memory_order_acq_rel,memory_order_acquire))return KSN_STALE;
    atomic_store_explicit(&pool->writer_busy,false,memory_order_release);
    *write=(ksn_source_write){0};
    return KSN_OK;
}
ksn_result ksn_source_pool_acquire(ksn_source_pool *pool,ksn_source_read *read){
    if(!pool||!pool->storage||!read||read->active)return KSN_INVALID;
    for(unsigned attempt=0;attempt<8;attempt++){
        unsigned token=atomic_load_explicit(&pool->current,memory_order_acquire);
        if(!token)return KSN_STALE;
        unsigned slot=token&3u;
        if(slot>=KSN_SOURCE_POOL_SLOTS)return KSN_INVALID;
        unsigned state=atomic_load_explicit(&pool->state[slot],memory_order_acquire);
        if((state&~PINS)!=PUBLISHED||(state&PINS)==PINS)continue;
        if(!atomic_compare_exchange_weak_explicit(&pool->state[slot],&state,state+1u,
            memory_order_acq_rel,memory_order_acquire))continue;
        ksn_source_read candidate={.pool=pool,.data=slot_data(pool,slot),
            .revision=token>>2,.slot=(uint8_t)slot,.active=true};
        if(atomic_load_explicit(&pool->current,memory_order_acquire)==token){
            *read=candidate;return KSN_OK;
        }
        (void)ksn_source_pool_release(&candidate);
    }
    return KSN_BUSY;
}
ksn_result ksn_source_pool_release(ksn_source_read *read){
    if(!read||!read->active||!read->pool||read->slot>=KSN_SOURCE_POOL_SLOTS)
        return KSN_INVALID;
    atomic_uint *state=&read->pool->state[read->slot];
    unsigned current=atomic_load_explicit(state,memory_order_acquire);
    while((current&PINS)&&((current&~PINS)==PUBLISHED||
                           (current&~PINS)==(PUBLISHED|RETIRED))){
        if(atomic_compare_exchange_weak_explicit(state,&current,current-1u,
            memory_order_acq_rel,memory_order_acquire)){
            *read=(ksn_source_read){0};return KSN_OK;
        }
    }
    return KSN_STALE;
}
uint32_t ksn_source_pool_latest(const ksn_source_pool *pool){
    return pool?atomic_load_explicit(&pool->current,memory_order_acquire)>>2:0;
}
uint32_t ksn_source_pool_skipped(const ksn_source_pool *pool){
    return pool?atomic_load_explicit(&pool->skipped,memory_order_relaxed):0;
}
