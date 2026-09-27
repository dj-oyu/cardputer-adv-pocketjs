/* Exercise the real arena implementation with deterministic allocator and
 * FreeRTOS queue shims. Timing and cross-core scheduling still need device
 * coverage; this test checks ownership and every bounded fallback branch. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>

#define ESP_PLATFORM 1
#include "../../main/pocket/pocket_mutex_arena.c"

static atomic_bool fail_arena_allocation,fail_static_create;
static atomic_bool race_allocate,hold_static_create,allow_static_create;
static atomic_uint race_allocating;
static atomic_bool static_create_entered;
static atomic_uint arena_allocations,arena_frees;
static unsigned dynamic_creates,dynamic_deletes;
static void *dynamic_queues[32];
static unsigned dynamic_live;

void *heap_caps_calloc(size_t count,size_t size,unsigned caps){
    assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    if(atomic_exchange(&fail_arena_allocation,false))return NULL;
    void *result=calloc(count,size);
    if(result)arena_allocations++;
    if(result&&atomic_load(&race_allocate)){
        atomic_fetch_add(&race_allocating,1);
        while(atomic_load(&race_allocating)<2)sched_yield();
    }
    return result;
}
void heap_caps_free(void *ptr){
    assert(ptr);
    arena_frees++;
    free(ptr);
}
QueueHandle_t xQueueCreateMutexStatic(uint8_t type,StaticQueue_t *storage){
    (void)type;
    if(atomic_load(&hold_static_create)){
        atomic_store(&static_create_entered,true);
        while(!atomic_load(&allow_static_create))sched_yield();
    }
    if(atomic_exchange(&fail_static_create,false))return NULL;
    return storage;
}
QueueHandle_t __real_xQueueCreateMutex(const uint8_t type){
    (void)type;
    assert(dynamic_live<sizeof(dynamic_queues)/sizeof(dynamic_queues[0]));
    void *queue=malloc(sizeof(StaticQueue_t));
    assert(queue);
    dynamic_queues[dynamic_live++]=queue;
    dynamic_creates++;
    return queue;
}
void __real_vQueueDelete(QueueHandle_t queue){
    for(unsigned i=0;i<dynamic_live;i++)if(dynamic_queues[i]==queue){
        dynamic_queues[i]=dynamic_queues[--dynamic_live];
        dynamic_deletes++;
        free(queue);
        return;
    }
    StaticQueue_t *storage=atomic_load(&arena);
    assert(storage);
    uintptr_t first=(uintptr_t)storage,value=(uintptr_t)queue;
    assert(value>=first&&value-first<POCKET_MUTEX_ARENA_SLOTS*sizeof(*storage));
}
static void expect_empty(void){
    assert(!atomic_load(&arena));
    assert(!atomic_load(&active));
    assert(active_users==0);
    assert(arena_allocations==arena_frees);
    assert(dynamic_creates==dynamic_deletes);
    assert(dynamic_live==0);
    for(unsigned i=0;i<POCKET_MUTEX_ARENA_SLOTS;i++)assert(!taken[i]);
}
static atomic_int worker_start,worker_ready,worker_go;
static void *parallel_producer(void *arg){
    (void)arg;
    while(!atomic_load(&worker_start))sched_yield();
    assert(pocket_mutex_arena_activate());
    atomic_fetch_add(&worker_ready,1);
    while(!atomic_load(&worker_go))sched_yield();
    StaticQueue_t *storage=atomic_load(&arena);
    QueueHandle_t queue=__wrap_xQueueCreateMutex(0);
    assert(queue&&storage&&(uintptr_t)queue>=(uintptr_t)storage&&
           (uintptr_t)queue-(uintptr_t)storage<POCKET_MUTEX_ARENA_SLOTS*sizeof(*storage));
    pocket_mutex_arena_deactivate();
    __wrap_vQueueDelete(queue);
    return NULL;
}
static atomic_bool race_start,race_release;
static atomic_uint race_ready;
static void *simultaneous_activate(void *arg){
    (void)arg;
    while(!atomic_load(&race_start))sched_yield();
    assert(pocket_mutex_arena_activate());
    atomic_fetch_add(&race_ready,1);
    while(!atomic_load(&race_release))sched_yield();
    pocket_mutex_arena_deactivate();
    return NULL;
}
static void *create_while_deactivating(void *arg){
    QueueHandle_t *queue=arg;
    *queue=__wrap_xQueueCreateMutex(0);
    return NULL;
}
int main(void){
    QueueHandle_t dormant=__wrap_xQueueCreateMutex(0);
    assert(dormant&&dynamic_creates==1&&arena_allocations==0);
    __wrap_vQueueDelete(dormant);
    expect_empty();

    fail_arena_allocation=true;
    assert(!pocket_mutex_arena_activate());
    QueueHandle_t failed=__wrap_xQueueCreateMutex(0);
    assert(failed&&dynamic_creates==2&&arena_allocations==0);
    __wrap_vQueueDelete(failed);
    expect_empty();

    assert(pocket_mutex_arena_activate());
    assert(pocket_mutex_arena_activate());
    assert(active_users==2);
    QueueHandle_t shared=__wrap_xQueueCreateMutex(0);
    StaticQueue_t *shared_storage=atomic_load(&arena);
    assert(shared==&shared_storage[0]);
    pocket_mutex_arena_deactivate();
    assert(active_users==1&&atomic_load(&active));
    QueueHandle_t second_scope=__wrap_xQueueCreateMutex(0);
    assert(second_scope==&shared_storage[1]);
    pocket_mutex_arena_deactivate();
    assert(!atomic_load(&active)&&atomic_load(&arena)==shared_storage);
    __wrap_vQueueDelete(shared);
    __wrap_vQueueDelete(second_scope);
    expect_empty();

    assert(pocket_mutex_arena_activate());
    StaticQueue_t *storage=atomic_load(&arena);
    assert(storage&&arena_allocations==2);
    QueueHandle_t queues[POCKET_MUTEX_ARENA_SLOTS];
    for(unsigned i=0;i<POCKET_MUTEX_ARENA_SLOTS;i++){
        queues[i]=__wrap_xQueueCreateMutex(0);
        assert(queues[i]==&storage[i]);
    }
    QueueHandle_t overflow=__wrap_xQueueCreateMutex(0);
    assert(overflow&&dynamic_creates==3);
    pocket_mutex_arena_deactivate();
    assert(atomic_load(&arena)==storage); /* Live queues pin backing. */
    __wrap_vQueueDelete(overflow);
    for(unsigned i=0;i<POCKET_MUTEX_ARENA_SLOTS-1;i++)
        __wrap_vQueueDelete(queues[i]);
    assert(atomic_load(&arena)==storage);

    /* Reactivation reuses backing even with one queue still alive. */
    assert(pocket_mutex_arena_activate());
    assert(atomic_load(&arena)==storage&&arena_allocations==2);
    QueueHandle_t reused=__wrap_xQueueCreateMutex(0);
    assert(reused==&storage[0]);
    pocket_mutex_arena_deactivate();
    __wrap_vQueueDelete(reused);
    assert(atomic_load(&arena)==storage);
    __wrap_vQueueDelete(queues[POCKET_MUTEX_ARENA_SLOTS-1]);
    expect_empty();

    assert(pocket_mutex_arena_activate());
    fail_static_create=true;
    QueueHandle_t static_failed=__wrap_xQueueCreateMutex(0);
    assert(static_failed&&dynamic_creates==4&&!taken[0]);
    pocket_mutex_arena_deactivate();
    assert(!atomic_load(&arena));
    __wrap_vQueueDelete(static_failed);
    expect_empty();

    pthread_t worker;
    assert(pthread_create(&worker,NULL,parallel_producer,NULL)==0);
    atomic_store(&worker_start,1);
    assert(pocket_mutex_arena_activate());
    atomic_fetch_add(&worker_ready,1);
    while(atomic_load(&worker_ready)<2)sched_yield();
    assert(active_users==2&&atomic_load(&active));
    pocket_mutex_arena_deactivate();
    assert(active_users==1&&atomic_load(&active));
    atomic_store(&worker_go,1);
    assert(pthread_join(worker,NULL)==0);
    expect_empty();

    /* Force two callers to allocate before either can publish the arena.
     * The losing allocation must be freed, while both users share one arena. */
    unsigned allocated_before=atomic_load(&arena_allocations);
    unsigned freed_before=atomic_load(&arena_frees);
    pthread_t contenders[2];
    atomic_store(&race_allocate,true);
    for(unsigned i=0;i<2;i++)
        assert(pthread_create(&contenders[i],NULL,simultaneous_activate,NULL)==0);
    atomic_store(&race_start,true);
    while(atomic_load(&race_ready)<2)sched_yield();
    assert(active_users==2&&atomic_load(&active)&&atomic_load(&arena));
    assert(atomic_load(&arena_allocations)==allocated_before+2);
    assert(atomic_load(&arena_frees)==freed_before+1);
    atomic_store(&race_release,true);
    for(unsigned i=0;i<2;i++)assert(pthread_join(contenders[i],NULL)==0);
    atomic_store(&race_allocate,false);
    expect_empty();

    /* Reservation pins backing even after the last producer deactivates.
     * Freeing it while xQueueCreateMutexStatic is in flight would be a UAF. */
    assert(pocket_mutex_arena_activate());
    StaticQueue_t *reserved_storage=atomic_load(&arena);
    QueueHandle_t reserved_queue=NULL;
    atomic_store(&hold_static_create,true);
    assert(pthread_create(&worker,NULL,create_while_deactivating,&reserved_queue)==0);
    while(!atomic_load(&static_create_entered))sched_yield();
    assert(taken[0]&&atomic_load(&arena)==reserved_storage);
    pocket_mutex_arena_deactivate();
    assert(!atomic_load(&active)&&atomic_load(&arena)==reserved_storage);
    atomic_store(&allow_static_create,true);
    assert(pthread_join(worker,NULL)==0);
    atomic_store(&hold_static_create,false);
    assert(reserved_queue==&reserved_storage[0]);
    __wrap_vQueueDelete(reserved_queue);
    expect_empty();

    puts("PASS mutex arena failure, overflow, concurrent activation, reserved create and reclaim");
    return 0;
}
