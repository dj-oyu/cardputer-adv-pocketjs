#include "fp_ticket.h"
#include <stddef.h>
#include <string.h>

fp_ticket_result_t fp_ticket_init(fp_ticket_manager_t *m,
                                 uint32_t core0_ppm, uint32_t core1_ppm) {
    if(!m || core0_ppm>FP_TICKET_FULL_LOAD || core1_ppm>FP_TICKET_FULL_LOAD ||
       (!core0_ppm && !core1_ppm)) return FP_TICKET_INVALID;
    memset(m,0,sizeof(*m));
    m->capacity_ppm[0]=core0_ppm;
    m->capacity_ppm[1]=core1_ppm;
    m->next_id=1;
    return FP_TICKET_OK;
}

fp_ticket_result_t fp_ticket_acquire(fp_ticket_manager_t *m,
                                    const fp_ticket_request_t *r,
                                    fp_ticket_t *ticket) {
    if(!m || !r || !ticket || !r->runtime_us || !r->period_us ||
       r->runtime_us>r->period_us || !r->allowed_cores ||
       (r->allowed_cores & ~FP_TICKET_ANY_CORE)) return FP_TICKET_INVALID;
    if(!m->next_id) return FP_TICKET_ID_EXHAUSTED;
    unsigned slot=FP_TICKET_SLOTS;
    for(unsigned i=0;i<FP_TICKET_SLOTS;i++) {
        if(!m->slots[i].id) { slot=i; break; }
    }
    if(slot==FP_TICKET_SLOTS) return FP_TICKET_NO_SLOT;
    // Widen before multiplying. Rounding upward prevents tiny jobs from
    // becoming free reservations, even for UINT32_MAX-length periods.
    uint32_t load=(uint32_t)(((uint64_t)r->runtime_us*FP_TICKET_FULL_LOAD+
                             r->period_us-1)/r->period_us);
    unsigned best=FP_TICKET_CORES;
    uint32_t best_used=0;
    for(unsigned core=0;core<FP_TICKET_CORES;core++) {
        uint32_t cap=m->capacity_ppm[core];
        if(!(r->allowed_cores & (1u<<core)) || !cap ||
           m->used_ppm[core]>cap || load>cap-m->used_ppm[core]) continue;
        uint32_t projected=m->used_ppm[core]+load;
        if(best==FP_TICKET_CORES ||
           (uint64_t)projected*m->capacity_ppm[best] < (uint64_t)best_used*cap) {
            best=core; best_used=projected;
        }
    }
    if(best==FP_TICKET_CORES) return FP_TICKET_NO_CAPACITY;
    fp_ticket_slot_t *s=&m->slots[slot];
    s->info=(fp_ticket_info_t){.core=(uint8_t)best,.load_ppm=load,.request=*r};
    s->id=m->next_id++;
    m->used_ppm[best]=best_used;
    *ticket=(fp_ticket_t){.owner=m,.id=s->id};
    return FP_TICKET_OK;
}

static const fp_ticket_slot_t *find(const fp_ticket_manager_t *m, fp_ticket_t t) {
    if(!m || t.owner!=m || !t.id) return NULL;
    for(unsigned i=0;i<FP_TICKET_SLOTS;i++)
        if(m->slots[i].id==t.id) return &m->slots[i];
    return NULL;
}

fp_ticket_result_t fp_ticket_get(const fp_ticket_manager_t *m,
                                fp_ticket_t t, fp_ticket_info_t *info) {
    if(!m || !info) return FP_TICKET_INVALID;
    const fp_ticket_slot_t *s=find(m,t);
    if(!s) return FP_TICKET_STALE;
    *info=s->info;
    return FP_TICKET_OK;
}

fp_ticket_result_t fp_ticket_release(fp_ticket_manager_t *m, fp_ticket_t t) {
    if(!m) return FP_TICKET_INVALID;
    const fp_ticket_slot_t *s=find(m,t);
    if(!s) return FP_TICKET_STALE;
    unsigned i=(unsigned)(s-m->slots);
    m->used_ppm[s->info.core]-=s->info.load_ppm;
    memset(&m->slots[i],0,sizeof(m->slots[i]));
    return FP_TICKET_OK;
}

fp_ticket_result_t fp_ticket_snapshot(const fp_ticket_manager_t *m,
                                     fp_ticket_snapshot_t *out) {
    if(!m || !out) return FP_TICKET_INVALID;
    fp_ticket_snapshot_t result={0};
    for(unsigned core=0;core<FP_TICKET_CORES;core++) {
        result.capacity_ppm[core]=m->capacity_ppm[core];
        result.used_ppm[core]=m->used_ppm[core];
    }
    for(unsigned i=0;i<FP_TICKET_SLOTS;i++) {
        if(m->slots[i].id) result.reservations[m->slots[i].info.core]++;
    }
    *out=result;
    return FP_TICKET_OK;
}
