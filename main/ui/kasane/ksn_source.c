#include "ksn_source.h"
#include <stdatomic.h>
#include <string.h>
#ifdef KASANE_P0_PROBE
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "esp_log.h"
static uint64_t borrow_pin_us,borrow_overlay_us;
static uint32_t borrow_calls,borrow_pin_max,borrow_overlay_max;
void ksn_source_borrow_probe_report(void){
    ESP_LOGI("KSN_SOURCE_BORROW","calls=%lu pin_mean_us=%lu pin_max_us=%lu overlay_mean_us=%lu overlay_max_us=%lu",
        (unsigned long)borrow_calls,
        (unsigned long)(borrow_calls?borrow_pin_us/borrow_calls:0),
        (unsigned long)borrow_pin_max,
        (unsigned long)(borrow_calls?borrow_overlay_us/borrow_calls:0),
        (unsigned long)borrow_overlay_max);
    borrow_pin_us=borrow_overlay_us=0;
    borrow_calls=borrow_pin_max=borrow_overlay_max=0;
}
#else
void ksn_source_borrow_probe_report(void){}
#endif
#endif
_Static_assert(sizeof(ksn_source_bundle)<=512u,"source bundle stack budget");
_Static_assert(sizeof(ksn_source_binding)==2u,"identity binding byte layout");

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
static const ksn_source_binding identity_bindings[KSN_SOURCE_MAX_FIELDS]={
    {0,0},{1,1},{2,2},{3,3},{4,4},{5,5},{6,6},{7,7},
    {8,8},{9,9},{10,10},{11,11},{12,12},{13,13},{14,14},{15,15},
    {16,16},{17,17},{18,18},{19,19},{20,20},{21,21},{22,22},{23,23}
};
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
        if((unsigned)provider->field_types[field]>KSN_SLOT_U32)return KSN_INVALID;
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
        .bound_slots=used_slots,.binding_count=count,
        .identity_bindings=used_slots==field_bits(count)&&
            memcmp(bindings,identity_bindings,count*sizeof(*bindings))==0};
    memcpy(out->bindings,bindings,count*sizeof(*bindings));
    return KSN_OK;
}
static ksn_result source_pin(ksn_source_registry *registry,
                             ksn_source_subscription *subscription,
                             const ksn_schema *schema,uint64_t now_us,
                             ksn_source_lease *lease,bool schema_checked,
                             bool validate_bindings){
    if(!subscription||!lease||lease->active||
       (!schema_checked&&ksn_schema_validate(schema)!=KSN_OK))return KSN_INVALID;
    ksn_source_entry *e=entry(registry,subscription->handle);
    if(!e)return KSN_STALE;
    const ksn_source_provider *p=e->provider;
    if(!subscription->binding_count||
       subscription->binding_count>KSN_SOURCE_MAX_FIELDS||e->pins==UINT32_MAX)
        return KSN_INVALID;
    if(validate_bindings){
        uint32_t bound=0;
        for(unsigned i=0;i<subscription->binding_count;i++){
            unsigned slot=subscription->bindings[i].slot;
            unsigned field=subscription->bindings[i].field;
            if(slot>=KSN_SCHEMA_MAX_SLOTS||field>=p->field_count||
               (bound&((uint32_t)1u<<slot))||
               (schema&&(slot>=schema->slot_count||
                 schema->slots[slot].type!=p->field_types[field])))return KSN_INVALID;
            bound|=(uint32_t)1u<<slot;
        }
        if(bound!=subscription->bound_slots)return KSN_INVALID;
    }
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
    *lease=(ksn_source_lease){.registry=registry,.subscription=subscription,
        .provider=p,.snapshot=snapshot,.active=true};
    return KSN_OK;
}
static ksn_result source_overlay(ksn_source_lease *lease,uint64_t now_us,
                                 ksn_schema_value *effective,
                                 bool validate_bindings){
    const ksn_source_subscription *subscription=lease->subscription;
    const ksn_source_snapshot *snapshot=&lease->snapshot;
    uint32_t valid_slots=0,dirty_slots=0,bound=0;
    bool expired=snapshot->expires_at_us&&now_us>=snapshot->expires_at_us;
    bool changed=!subscription->has_validated||
                 snapshot->revision!=subscription->validated_revision;
    bool gap=!subscription->has_validated||
             subscription->validated_revision==UINT64_MAX||
             snapshot->revision!=subscription->validated_revision+1u;
    if(validate_bindings&&subscription->identity_bindings){
        uint32_t mask=field_bits(subscription->binding_count);
        if(subscription->binding_count>snapshot->field_count||
           subscription->bound_slots!=mask||
           memcmp(subscription->bindings,identity_bindings,
                  subscription->binding_count*sizeof(ksn_source_binding))!=0)
            return KSN_INVALID;
        valid_slots=expired?0u:snapshot->valid_fields&mask;
        dirty_slots=changed?(gap?mask:snapshot->changed_fields&mask):0u;
        dirty_slots|=valid_slots^subscription->valid_slots;
        lease->valid_slots=valid_slots;lease->dirty_slots=dirty_slots;
        return KSN_OK;
    }
    for(unsigned i=0;i<subscription->binding_count;i++){
        unsigned slot=subscription->bindings[i].slot;
        unsigned field=subscription->bindings[i].field;
        if(validate_bindings&&(slot>=KSN_SCHEMA_MAX_SLOTS||
            field>=snapshot->field_count||
            (bound&((uint32_t)1u<<slot))))return KSN_INVALID;
        uint32_t slot_bit=(uint32_t)1u<<slot,field_bit=(uint32_t)1u<<field;
        if(validate_bindings)bound|=slot_bit;
        if(!expired&&(snapshot->valid_fields&field_bit)){
            if(effective)effective[slot]=snapshot->fields[field];
            valid_slots|=slot_bit;
        }
        if(changed&&(gap||(snapshot->changed_fields&field_bit)))dirty_slots|=slot_bit;
    }
    if(validate_bindings&&bound!=subscription->bound_slots)return KSN_INVALID;
    dirty_slots|=valid_slots^subscription->valid_slots;
    lease->valid_slots=valid_slots;lease->dirty_slots=dirty_slots;
    return KSN_OK;
}
ksn_result ksn_source_acquire(ksn_source_registry *registry,
                              ksn_source_subscription *subscription,
                              const ksn_schema *schema,
                              const ksn_schema_value *base,uint64_t now_us,
                              ksn_schema_value effective[KSN_SCHEMA_MAX_SLOTS],
                              ksn_source_lease *lease){
    if(!schema||!effective||base==effective||
       (schema->slot_count&&!base))return KSN_INVALID;
    ksn_result r=source_pin(registry,subscription,schema,now_us,lease,false,true);
    if(r!=KSN_OK)return r;
    memcpy(effective,base,schema->slot_count*sizeof(*effective));
    (void)source_overlay(lease,now_us,effective,false);
    if(ksn_schema_values_validate(schema,effective)!=KSN_OK){
        /* Failure is cold-path. Keep the candidate atomic without adding a
         * second metadata or payload copy to a successful owner turn. */
        memcpy(effective,base,schema->slot_count*sizeof(*effective));
        ksn_source_release(lease);
        return KSN_INVALID;
    }
    return KSN_OK;
}
ksn_result ksn_source_borrow(ksn_source_registry *registry,
                             ksn_source_subscription *subscription,
                             uint64_t now_us,ksn_source_lease *lease){
#if defined(KASANE_P0_PROBE) && defined(ESP_PLATFORM)
    int64_t begun=esp_timer_get_time();
#endif
    ksn_result r=source_pin(registry,subscription,NULL,now_us,lease,true,false);
    if(r==KSN_OK){
#if defined(KASANE_P0_PROBE) && defined(ESP_PLATFORM)
        int64_t pinned=esp_timer_get_time();
#endif
        r=source_overlay(lease,now_us,NULL,true);
        if(r!=KSN_OK)ksn_source_release(lease);
#if defined(KASANE_P0_PROBE) && defined(ESP_PLATFORM)
        uint32_t pin_us=(uint32_t)(pinned-begun);
        uint32_t overlay_us=(uint32_t)(esp_timer_get_time()-pinned);
        borrow_calls++;borrow_pin_us+=pin_us;borrow_overlay_us+=overlay_us;
        if(pin_us>borrow_pin_max)borrow_pin_max=pin_us;
        if(overlay_us>borrow_overlay_max)borrow_overlay_max=overlay_us;
#endif
    }
    return r;
}
ksn_result ksn_source_borrow_identity(ksn_source_registry *registry,
                                      ksn_source_subscription *subscription,
                                      uint64_t now_us,ksn_source_lease *lease){
    if(!subscription||!lease||lease->active||!subscription->identity_bindings)
        return KSN_INVALID;
    ksn_source_entry *e=entry(registry,subscription->handle);
    if(!e)return KSN_STALE;
    const ksn_source_provider *p=e->provider;
    unsigned count=subscription->binding_count;
    if(!count||count>p->field_count||e->pins==UINT32_MAX||
       subscription->bound_slots!=field_bits(count))return KSN_INVALID;
    if(!p->allow(p->context,subscription->consumer))return KSN_UNSUPPORTED;
    ksn_source_snapshot snapshot={0};
    e->pins++;
    ksn_result r=p->acquire(p->context,
        subscription->has_validated?subscription->validated_revision:0,
        now_us,&snapshot);
    if(r!=KSN_OK){e->pins--;return r;}
    uint32_t provider_mask=field_bits(p->field_count);
    if(snapshot.size<sizeof(snapshot)||snapshot.version!=KSN_SOURCE_ABI_VERSION||
       snapshot.field_count!=p->field_count||
       snapshot.generation!=subscription->handle.generation||
       !snapshot.revision||!snapshot.fields||
       (subscription->has_validated&&
        snapshot.revision<subscription->validated_revision)||
       (snapshot.valid_fields&~provider_mask)||
       (snapshot.changed_fields&~provider_mask)){
        p->release(p->context,&snapshot);e->pins--;return KSN_INVALID;
    }
    uint32_t mask=subscription->bound_slots;
    bool expired=snapshot.expires_at_us&&now_us>=snapshot.expires_at_us;
    bool changed=!subscription->has_validated||
                 snapshot.revision!=subscription->validated_revision;
    bool gap=!subscription->has_validated||
             subscription->validated_revision==UINT64_MAX||
             snapshot.revision!=subscription->validated_revision+1u;
    uint32_t valid_slots=expired?0u:snapshot.valid_fields&mask;
    uint32_t dirty_slots=changed?(gap?mask:snapshot.changed_fields&mask):0u;
    dirty_slots|=valid_slots^subscription->valid_slots;
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
ksn_result ksn_source_bundle_acquire(const ksn_source_member *members,uint8_t count,
    const ksn_schema *schema,const ksn_schema_value *base,uint64_t now_us,
    ksn_schema_value effective[KSN_SCHEMA_MAX_SLOTS],ksn_source_bundle *bundle){
    if(!members||!count||count>KSN_SOURCE_MAX_REGISTERED||!bundle||
       bundle->active||bundle->count||!effective||base==effective||
       ksn_schema_validate(schema)!=KSN_OK||(schema->slot_count&&!base))
        return KSN_INVALID;
    uint32_t used_slots=0;
    for(unsigned i=0;i<count;i++){
        const ksn_source_subscription *sub=members[i].subscription;
        if(!members[i].registry||!sub||!sub->binding_count||
           sub->binding_count>KSN_SOURCE_MAX_FIELDS)return KSN_INVALID;
        uint32_t bound=0;
        for(unsigned j=0;j<sub->binding_count;j++){
            unsigned slot=sub->bindings[j].slot;
            if(slot>=schema->slot_count||
               (bound&((uint32_t)1u<<slot)))return KSN_INVALID;
            bound|=(uint32_t)1u<<slot;
        }
        if(bound!=sub->bound_slots||(used_slots&bound))return KSN_INVALID;
        used_slots|=bound;
    }
    memcpy(effective,base,schema->slot_count*sizeof(*effective));
    bundle->dirty_slots=0;
    for(unsigned i=0;i<count;i++){
        ksn_source_lease *lease=&bundle->leases[i];
        ksn_result r=source_pin(members[i].registry,members[i].subscription,
                                 schema,now_us,lease,true,true);
        if(r!=KSN_OK){
            ksn_source_bundle_release(bundle);
            memcpy(effective,base,schema->slot_count*sizeof(*effective));
            return r;
        }
        bundle->count++;
        (void)source_overlay(lease,now_us,effective,false);
        bundle->dirty_slots|=lease->dirty_slots;
    }
    if(ksn_schema_values_validate(schema,effective)!=KSN_OK){
        ksn_source_bundle_release(bundle);
        memcpy(effective,base,schema->slot_count*sizeof(*effective));
        return KSN_INVALID;
    }
    bundle->active=true;
    return KSN_OK;
}
ksn_result ksn_source_bundle_commit(ksn_source_bundle *bundle){
    if(!bundle||!bundle->active||!bundle->count||
       bundle->count>KSN_SOURCE_MAX_REGISTERED)return KSN_INVALID;
    for(unsigned i=0;i<bundle->count;i++)
        if(!bundle->leases[i].active||bundle->leases[i].committed)return KSN_INVALID;
    for(unsigned i=0;i<bundle->count;i++)
        (void)ksn_source_commit(&bundle->leases[i]);
    return KSN_OK;
}
void ksn_source_bundle_release(ksn_source_bundle *bundle){
    if(!bundle)return;
    while(bundle->count)ksn_source_release(&bundle->leases[--bundle->count]);
    *bundle=(ksn_source_bundle){0};
}
ksn_result ksn_source_presented(ksn_source_subscription *subscription,
                                uint64_t revision){
    if(!subscription||!subscription->has_validated||!revision||
       revision<subscription->displayed_revision||
       revision>subscription->validated_revision)return KSN_INVALID;
    subscription->displayed_revision=revision;
    return KSN_OK;
}
