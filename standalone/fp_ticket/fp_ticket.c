#include "fp_ticket.h"
#include <stddef.h>
#include <string.h>

_Static_assert(FP_TICKET_SLOTS==16 && FP_TICKET_CORES==2, "mask layout");

static uint32_t load_of(const fp_ticket_request_t *r) {
    // Widen and round upward so even tiny jobs reserve capacity.
    return (uint32_t)(((uint64_t)r->runtime_us*FP_TICKET_FULL_LOAD+
                       r->period_us-1)/r->period_us);
}

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
       (r->allowed_cores & ~FP_TICKET_ANY_CORE) ||
       (r->flags & ~FP_TICKET_USES_FP)) return FP_TICKET_INVALID;
    if(!m->next_id) return FP_TICKET_ID_EXHAUSTED;
    unsigned slot=FP_TICKET_SLOTS;
    for(unsigned i=0;i<FP_TICKET_SLOTS;i++) {
        if(!(m->live_mask & (1u<<i))) { slot=i; break; }
    }
    if(slot==FP_TICKET_SLOTS) return FP_TICKET_NO_SLOT;
    uint32_t load=load_of(r);
    fp_ticket_snapshot_t state;
    fp_ticket_snapshot(m,&state);
    unsigned eligible=r->allowed_cores;
    for(unsigned core=0;core<FP_TICKET_CORES;core++)
        if(!m->capacity_ppm[core]) eligible &= ~(1u<<core);
    if(!eligible) return FP_TICKET_NO_CAPACITY;
    if(r->flags & FP_TICKET_USES_FP) {
        eligible &= ~state.fp_cores;
        if(!eligible) return FP_TICKET_FP_BUSY;
    }
    unsigned best=FP_TICKET_CORES;
    uint32_t best_used=0;
    for(unsigned core=0;core<FP_TICKET_CORES;core++) {
        uint32_t cap=m->capacity_ppm[core];
        if(!(eligible & (1u<<core)) ||
           state.used_ppm[core]>cap || load>cap-state.used_ppm[core]) continue;
        uint32_t projected=state.used_ppm[core]+load;
        if(best==FP_TICKET_CORES ||
           (uint64_t)projected*m->capacity_ppm[best] < (uint64_t)best_used*cap ||
           ((uint64_t)projected*m->capacity_ppm[best] == (uint64_t)best_used*cap &&
            !(r->flags & FP_TICKET_USES_FP) &&
            state.non_fp_reservations[core]<state.non_fp_reservations[best])) {
            best=core; best_used=projected;
        }
    }
    if(best==FP_TICKET_CORES) return FP_TICKET_NO_CAPACITY;
    fp_ticket_slot_t *s=&m->slots[slot];
    s->request=r;
    s->id=m->next_id++;
    uint16_t bit=(uint16_t)(1u<<slot);
    m->live_mask |= bit;
    if(best==1) m->core1_mask |= bit;
    if(r->flags & FP_TICKET_USES_FP) m->fp_mask |= bit;
    *ticket=(fp_ticket_t){.owner=m,.id=s->id};
    return FP_TICKET_OK;
}

static const fp_ticket_slot_t *find(const fp_ticket_manager_t *m, fp_ticket_t t) {
    if(!m || t.owner!=m || !t.id) return NULL;
    for(unsigned i=0;i<FP_TICKET_SLOTS;i++)
        if((m->live_mask & (1u<<i)) && m->slots[i].id==t.id)
            return &m->slots[i];
    return NULL;
}

fp_ticket_result_t fp_ticket_get(const fp_ticket_manager_t *m,
                                fp_ticket_t t, fp_ticket_info_t *info) {
    if(!m || !info) return FP_TICKET_INVALID;
    const fp_ticket_slot_t *s=find(m,t);
    if(!s) return FP_TICKET_STALE;
    unsigned i=(unsigned)(s-m->slots);
    *info=(fp_ticket_info_t){.core=(m->core1_mask & (1u<<i)) ? 1 : 0,
                            .load_ppm=load_of(s->request),.request=*s->request};
    return FP_TICKET_OK;
}

fp_ticket_result_t fp_ticket_release(fp_ticket_manager_t *m, fp_ticket_t t) {
    if(!m) return FP_TICKET_INVALID;
    const fp_ticket_slot_t *s=find(m,t);
    if(!s) return FP_TICKET_STALE;
    unsigned i=(unsigned)(s-m->slots);
    uint16_t keep=(uint16_t)~(1u<<i);
    m->live_mask &= keep;
    m->core1_mask &= keep;
    m->fp_mask &= keep;
    memset(&m->slots[i],0,sizeof(m->slots[i]));
    return FP_TICKET_OK;
}

fp_ticket_result_t fp_ticket_snapshot(const fp_ticket_manager_t *m,
                                     fp_ticket_snapshot_t *out) {
    if(!m || !out) return FP_TICKET_INVALID;
    fp_ticket_snapshot_t result={0};
    for(unsigned core=0;core<FP_TICKET_CORES;core++) {
        result.capacity_ppm[core]=m->capacity_ppm[core];
    }
    for(unsigned i=0;i<FP_TICKET_SLOTS;i++) {
        unsigned bit=1u<<i;
        if(!(m->live_mask & bit)) continue;
        unsigned core=(m->core1_mask & bit) ? 1 : 0;
        result.used_ppm[core]+=load_of(m->slots[i].request);
        result.reservations[core]++;
        if(m->fp_mask & bit) result.fp_cores |= (uint8_t)(1u<<core);
        else result.non_fp_reservations[core]++;
    }
    *out=result;
    return FP_TICKET_OK;
}
