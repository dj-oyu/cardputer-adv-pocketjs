#include "ksn_source.h"
#include <stdatomic.h>
#include <string.h>

/* Generations must also survive a registry object's teardown/reuse at the
 * same address. Registration is cold-path; leases remain allocation-free. */
static _Atomic uint32_t next_generation;
static uint32_t allocate_generation(void){
    uint32_t current=atomic_load_explicit(&next_generation,memory_order_relaxed);
    do{
        if(current==UINT32_MAX)return 0;
    }while(!atomic_compare_exchange_weak_explicit(&next_generation,&current,
        current+1u,memory_order_relaxed,memory_order_relaxed));
    return current+1u;
}

static uint32_t field_bits(unsigned count){
    return count==32u?UINT32_MAX:((uint32_t)1u<<count)-1u;
}
static ksn_source_entry *entry(ksn_source_registry *registry,
                               ksn_source_handle handle){
    if(!registry||handle.index>=KSN_SOURCE_MAX_REGISTERED||!handle.generation)
        return NULL;
    ksn_source_entry *e=&registry->entries[handle.index];
    return e->provider&&e->generation==handle.generation?e:NULL;
}
void ksn_source_registry_init(ksn_source_registry *registry){
    if(registry)memset(registry,0,sizeof(*registry));
}
ksn_result ksn_source_register(ksn_source_registry *registry,
                               const ksn_source_provider *provider,
                               ksn_source_handle *out){
    if(!registry||!provider||!out||provider->size<sizeof(*provider)||
       provider->version!=KSN_SOURCE_ABI_VERSION||!provider->field_count||
       provider->field_count>KSN_SOURCE_MAX_FIELDS||!provider->field_types||
       !provider->acquire||!provider->release||!provider->allow)return KSN_INVALID;
    for(unsigned field=0;field<provider->field_count;field++)
        if((unsigned)provider->field_types[field]>KSN_SLOT_RESOURCE)return KSN_INVALID;
    for(unsigned i=0;i<KSN_SOURCE_MAX_REGISTERED;i++){
        ksn_source_entry *e=&registry->entries[i];
        if(e->provider)continue;
        uint32_t generation=allocate_generation();
        if(!generation)return KSN_LIMIT;
        e->provider=provider;e->generation=generation;
        *out=(ksn_source_handle){(uint8_t)i,e->generation};
        return KSN_OK;
    }
    return KSN_LIMIT;
}
ksn_result ksn_source_unregister(ksn_source_registry *registry,
                                 ksn_source_handle handle){
    ksn_source_entry *e=entry(registry,handle);
    if(!e)return KSN_STALE;
    if(e->pins)return KSN_BUSY;
    e->provider=NULL;
    return KSN_OK;
}
ksn_result ksn_source_subscribe(ksn_source_registry *registry,
                                ksn_source_handle handle,uint32_t consumer,
                                const ksn_schema *schema,
                                const ksn_source_binding *bindings,uint8_t count,
                                ksn_source_subscription *out){
    ksn_source_entry *e=entry(registry,handle);
    if(!e)return KSN_STALE;
    const ksn_source_provider *p=e->provider;
    if(!consumer||!p->allow(p->context,consumer))return KSN_UNSUPPORTED;
    if(!out||!bindings||!count||count>KSN_SOURCE_MAX_FIELDS||
       ksn_schema_validate(schema)!=KSN_OK)return KSN_INVALID;
    uint32_t used_slots=0;
    for(unsigned i=0;i<count;i++){
        unsigned slot=bindings[i].slot,field=bindings[i].field;
        if(slot>=schema->slot_count||field>=p->field_count||
           (used_slots&((uint32_t)1u<<slot))||
           schema->slots[slot].type!=p->field_types[field])return KSN_INVALID;
        used_slots|=(uint32_t)1u<<slot;
    }
    *out=(ksn_source_subscription){.handle=handle,.consumer=consumer,
        .bound_slots=used_slots,.binding_count=count};
    memcpy(out->bindings,bindings,count*sizeof(*bindings));
    return KSN_OK;
}
ksn_result ksn_source_acquire(ksn_source_registry *registry,
                              ksn_source_subscription *subscription,
                              const ksn_schema *schema,
                              const ksn_schema_value *base,uint64_t now_us,
                              ksn_schema_value effective[KSN_SCHEMA_MAX_SLOTS],
                              ksn_source_lease *lease){
    if(!subscription||!effective||base==effective||!lease||lease->active||
       ksn_schema_validate(schema)!=KSN_OK||
       (schema->slot_count&&!base))return KSN_INVALID;
    ksn_source_entry *e=entry(registry,subscription->handle);
    if(!e)return KSN_STALE;
    const ksn_source_provider *p=e->provider;
    if(!subscription->binding_count||
       subscription->binding_count>KSN_SOURCE_MAX_FIELDS||e->pins==UINT32_MAX)
        return KSN_INVALID;
    for(unsigned i=0;i<subscription->binding_count;i++)
        if(subscription->bindings[i].slot>=schema->slot_count||
           subscription->bindings[i].field>=p->field_count||
           schema->slots[subscription->bindings[i].slot].type!=
               p->field_types[subscription->bindings[i].field])return KSN_INVALID;
    if(!p->allow(p->context,subscription->consumer))return KSN_UNSUPPORTED;
    ksn_source_snapshot snapshot={0};
    e->pins++;
    ksn_result r=p->acquire(p->context,
                           subscription->has_validated?subscription->validated_revision:0,
                           now_us,&snapshot);
    if(r!=KSN_OK){e->pins--;return r;}
    uint32_t mask=field_bits(p->field_count);
    if(snapshot.size<sizeof(snapshot)||snapshot.version!=KSN_SOURCE_ABI_VERSION||
       snapshot.field_count!=p->field_count||
       snapshot.generation!=subscription->handle.generation||
       !snapshot.revision||!snapshot.fields||
       (subscription->has_validated&&
        snapshot.revision<subscription->validated_revision)||
       (snapshot.valid_fields&~mask)||(snapshot.changed_fields&~mask)){
        p->release(p->context,&snapshot);e->pins--;return KSN_INVALID;
    }
    memcpy(effective,base,schema->slot_count*sizeof(*effective));
    uint32_t valid_slots=0,dirty_slots=0;
    bool expired=snapshot.expires_at_us&&now_us>=snapshot.expires_at_us;
    bool changed=!subscription->has_validated||
                 snapshot.revision!=subscription->validated_revision;
    bool gap=!subscription->has_validated||
             subscription->validated_revision==UINT64_MAX||
             snapshot.revision!=subscription->validated_revision+1u;
    for(unsigned i=0;i<subscription->binding_count;i++){
        unsigned slot=subscription->bindings[i].slot;
        unsigned field=subscription->bindings[i].field;
        uint32_t slot_bit=(uint32_t)1u<<slot,field_bit=(uint32_t)1u<<field;
        if(!expired&&(snapshot.valid_fields&field_bit)){
            effective[slot]=snapshot.fields[field];
            valid_slots|=slot_bit;
        }
        if(changed&&(gap||(snapshot.changed_fields&field_bit)))dirty_slots|=slot_bit;
    }
    dirty_slots|=valid_slots^subscription->valid_slots;
    if(ksn_schema_values_validate(schema,effective)!=KSN_OK){
        /* A malformed snapshot must not leave some native fields installed in
         * the caller's candidate. Failure is cold-path; successful acquire
         * keeps its single metadata copy and borrowed payload pointers. */
        memcpy(effective,base,schema->slot_count*sizeof(*effective));
        p->release(p->context,&snapshot);e->pins--;return KSN_INVALID;
    }
    *lease=(ksn_source_lease){.registry=registry,.subscription=subscription,
        .provider=p,.snapshot=snapshot,.valid_slots=valid_slots,
        .dirty_slots=dirty_slots,.active=true};
    return KSN_OK;
}
ksn_result ksn_source_commit(ksn_source_lease *lease){
    if(!lease||!lease->active||lease->committed)return KSN_INVALID;
    ksn_source_subscription *sub=lease->subscription;
    sub->validated_revision=lease->snapshot.revision;
    sub->valid_slots=lease->valid_slots;
    sub->has_validated=true;
    lease->committed=true;
    return KSN_OK;
}
void ksn_source_release(ksn_source_lease *lease){
    if(!lease||!lease->active)return;
    lease->provider->release(lease->provider->context,&lease->snapshot);
    lease->registry->entries[lease->subscription->handle.index].pins--;
    *lease=(ksn_source_lease){0};
}
ksn_result ksn_source_presented(ksn_source_subscription *subscription,
                                uint64_t revision){
    if(!subscription||!subscription->has_validated||!revision||
       revision<subscription->displayed_revision||
       revision>subscription->validated_revision)return KSN_INVALID;
    subscription->displayed_revision=revision;
    return KSN_OK;
}
