#include "ksn_source_pool.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

typedef struct {uint32_t value,inverse,mix;} payload;
static payload slots[KSN_SOURCE_POOL_SLOTS];
static ksn_source_pool pool;
static atomic_bool done;
static atomic_uint errors;

static void put(ksn_source_write *write,uint32_t value){
    payload *p=write->data;
    *p=(payload){value,~value,value^UINT32_C(0xa5a55a5a)};
}
static bool sound(const ksn_source_read *read){
    const payload *p=read->data;
    return p->inverse==~p->value&&p->mix==(p->value^UINT32_C(0xa5a55a5a));
}
static void *producer(void *unused){
    (void)unused;
    for(uint32_t i=0;i<50000;i++){
        ksn_source_write write={0};
        if(ksn_source_pool_begin(&pool,&write)!=KSN_OK){sched_yield();continue;}
        put(&write,i);
        if(ksn_source_pool_publish(&write,NULL)!=KSN_OK)atomic_fetch_add(&errors,1);
    }
    return NULL;
}
static void *consumer(void *unused){
    (void)unused;
    uint32_t last=0;
    while(!atomic_load(&done)){
        ksn_source_read read={0};
        if(ksn_source_pool_acquire(&pool,&read)!=KSN_OK){sched_yield();continue;}
        if(!sound(&read)||read.revision<last)atomic_fetch_add(&errors,1);
        last=read.revision;
        sched_yield(); /* keep old generations pinned while producer publishes */
        if(!sound(&read)||ksn_source_pool_release(&read)!=KSN_OK)
            atomic_fetch_add(&errors,1);
    }
    return NULL;
}
int main(void){
    assert(ksn_source_pool_init(&pool,slots,sizeof(slots)-1,sizeof(slots[0]))==KSN_INVALID);
    assert(ksn_source_pool_init(&pool,slots,sizeof(slots),sizeof(slots[0]))==KSN_OK);
    ksn_source_read a={0},b={0},c={0},latest={0};
    assert(ksn_source_pool_acquire(&pool,&a)==KSN_STALE);
    ksn_source_write write={0},busy={0};
    assert(ksn_source_pool_begin(&pool,&write)==KSN_OK);
    assert(ksn_source_pool_begin(&pool,&busy)==KSN_BUSY);
    assert(ksn_source_pool_cancel(&write)==KSN_OK);
    assert(ksn_source_pool_cancel(&write)==KSN_INVALID);
    assert(ksn_source_pool_begin(&pool,&write)==KSN_OK);
    put(&write,11);uint32_t revision=0;
    assert(ksn_source_pool_publish(&write,&revision)==KSN_OK&&revision==1);
    assert(ksn_source_pool_acquire(&pool,&a)==KSN_OK&&sound(&a));
    const void *old_a=a.data;
    assert(ksn_source_pool_begin(&pool,&write)==KSN_OK);
    put(&write,22);
    assert(ksn_source_pool_publish(&write,&revision)==KSN_OK&&revision==2);
    assert(ksn_source_pool_acquire(&pool,&b)==KSN_OK&&sound(&b));
    assert(ksn_source_pool_begin(&pool,&write)==KSN_OK);
    put(&write,33);
    assert(ksn_source_pool_publish(&write,&revision)==KSN_OK&&revision==3);
    assert(ksn_source_pool_acquire(&pool,&c)==KSN_OK&&sound(&c));
    assert(ksn_source_pool_begin(&pool,&write)==KSN_BUSY);
    assert(ksn_source_pool_skipped(&pool)==1&&
           a.data==old_a&&((const payload *)a.data)->value==11&&
           ((const payload *)b.data)->value==22&&
           ((const payload *)c.data)->value==33);
    assert(ksn_source_pool_release(&a)==KSN_OK);
    assert(ksn_source_pool_begin(&pool,&write)==KSN_OK);
    put(&write,44);
    assert(ksn_source_pool_publish(&write,&revision)==KSN_OK&&revision==4);
    assert(sound(&b)&&sound(&c)&&
           ((const payload *)b.data)->value==22&&
           ((const payload *)c.data)->value==33);
    assert(ksn_source_pool_acquire(&pool,&latest)==KSN_OK&&
           latest.revision==4&&((const payload *)latest.data)->value==44);
    assert(ksn_source_pool_release(&b)==KSN_OK);
    assert(ksn_source_pool_release(&c)==KSN_OK);
    assert(ksn_source_pool_release(&latest)==KSN_OK);
    assert(ksn_source_pool_release(&latest)==KSN_INVALID);

    /* Many concurrent reader leases must never permit reuse of their slot. */
    ksn_source_read pins[255]={0};
    for(unsigned i=0;i<255;i++)assert(ksn_source_pool_acquire(&pool,&pins[i])==KSN_OK);
    assert(ksn_source_pool_acquire(&pool,&latest)==KSN_BUSY);
    for(unsigned i=0;i<255;i++)assert(ksn_source_pool_release(&pins[i])==KSN_OK);
    assert(ksn_source_pool_acquire(&pool,&latest)==KSN_OK);
    assert(ksn_source_pool_release(&latest)==KSN_OK);

    /* Two producers contend for the single writer lease while readers pin
     * older immutable generations. BUSY is allowed; torn payloads are not. */
    pthread_t writers[2],readers[2];
    for(unsigned i=0;i<2;i++)
        assert(pthread_create(&writers[i],NULL,producer,NULL)==0);
    for(unsigned i=0;i<2;i++)assert(pthread_create(&readers[i],NULL,consumer,NULL)==0);
    for(unsigned i=0;i<2;i++)assert(pthread_join(writers[i],NULL)==0);
    atomic_store(&done,true);
    for(unsigned i=0;i<2;i++)assert(pthread_join(readers[i],NULL)==0);
    assert(atomic_load(&errors)==0&&ksn_source_pool_latest(&pool)>=4);

    /* Revision exhaustion must not wrap a stale handle back into use. A
     * rejected publish keeps the writer lease until explicit cancellation. */
    uint32_t before_limit=ksn_source_pool_latest(&pool);
    atomic_store(&pool.revision,UINT32_MAX>>2);
    assert(ksn_source_pool_begin(&pool,&write)==KSN_OK);
    put(&write,99);
    assert(ksn_source_pool_publish(&write,&revision)==KSN_LIMIT&&write.active);
    assert(ksn_source_pool_latest(&pool)==before_limit);
    assert(ksn_source_pool_begin(&pool,&busy)==KSN_BUSY);
    assert(ksn_source_pool_cancel(&write)==KSN_OK);
    assert(ksn_source_pool_acquire(&pool,&latest)==KSN_OK&&sound(&latest));
    assert(ksn_source_pool_release(&latest)==KSN_OK);
    puts("source pool: PASS (zero-copy leases, exhaustion, immutability, concurrent producers/readers, revision limit)");
    return 0;
}
