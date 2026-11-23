#include "sd_lease.h"
#include <stddef.h>

void sd_lease_registry_reset(sd_lease_registry_t *r) {
    r->head=NULL;
    r->generation=0;
    r->mounted=false;
    r->pending_unmount=false;
}

bool sd_lease_registry_mount(sd_lease_registry_t *r, uint32_t generation) {
    if(r->pending_unmount||r->head||!generation) return false;
    r->generation=generation;
    r->mounted=true;
    return true;
}

bool sd_lease_attach(sd_lease_registry_t *r, sd_lease_t *l,
                     uint32_t key, uint32_t generation) {
    if(!l||!r->mounted||r->pending_unmount||!generation||
       generation!=r->generation) return false;
    for(sd_lease_t *at=r->head;at;at=at->next)
        if(at==l) return false;
    l->key=key;
    l->generation=generation;
    atomic_init(&l->revoked,false);
    atomic_init(&l->fault,false);
    atomic_init(&l->ack,false);
    l->next=r->head;
    r->head=l;
    return true;
}

void sd_lease_cancel_all(sd_lease_registry_t *r) {
    r->mounted=false;
    r->pending_unmount=true;
    for(sd_lease_t *at=r->head;at;at=at->next)
        atomic_store(&at->revoked,true);
}

bool sd_lease_busy(const sd_lease_registry_t *r, uint32_t key) {
    for(const sd_lease_t *at=r->head;at;at=at->next)
        if(at->key==key) return true;
    return false;
}

bool sd_lease_take_fault(sd_lease_registry_t *r) {
    bool fault=false;
    for(sd_lease_t *at=r->head;at;at=at->next)
        if(atomic_exchange(&at->fault,false)) fault=true;
    return fault;
}

bool sd_lease_ready_to_unmount(const sd_lease_registry_t *r) {
    return r->pending_unmount&&!r->head;
}

void sd_lease_unmounted(sd_lease_registry_t *r) {
    if(sd_lease_ready_to_unmount(r)) r->pending_unmount=false;
}

void sd_lease_ack(sd_lease_t *l) { atomic_store(&l->ack,true); }

bool sd_lease_detach(sd_lease_registry_t *r, sd_lease_t *l) {
    if(!l||!atomic_load(&l->ack)) return false;
    for(sd_lease_t **at=&r->head;*at;at=&(*at)->next) {
        if(*at==l) {
            *at=l->next;
            l->next=NULL;
            return true;
        }
    }
    return false;
}
