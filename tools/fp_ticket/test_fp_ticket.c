#include "fp_ticket.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static fp_ticket_request_t req(uint32_t run, uint32_t period, uint8_t mask) {
    return (fp_ticket_request_t){.runtime_us=run,.period_us=period,
                                .allowed_cores=mask,.priority=6};
}
int main(void) {
    fp_ticket_manager_t m, other;
    fp_ticket_t a={0}, b={0}, c={0};
    fp_ticket_info_t info;
    fp_ticket_snapshot_t snap;
    assert(fp_ticket_init(&m,800000,800000)==FP_TICKET_OK);
    fp_ticket_request_t ui=req(20000,33333,FP_TICKET_CORE_1);
    assert(fp_ticket_acquire(&m,&ui,&a)==FP_TICKET_OK);
    fp_ticket_request_t mp3=req(6000,24000,FP_TICKET_ANY_CORE);
    assert(fp_ticket_acquire(&m,&mp3,&b)==FP_TICKET_OK);
    assert(fp_ticket_get(&m,b,&info)==FP_TICKET_OK);
    assert(info.core==0 && info.load_ppm==250000 && info.request.priority==6);
    assert(fp_ticket_snapshot(&m,&snap)==FP_TICKET_OK);
    assert(snap.used_ppm[1]==600007 && snap.reservations[0]==1);

    fp_ticket_manager_t before=m;
    fp_ticket_request_t large=req(900,1000,FP_TICKET_ANY_CORE);
    c=a;
    assert(fp_ticket_acquire(&m,&large,&c)==FP_TICKET_NO_CAPACITY);
    assert(memcmp(&m,&before,sizeof(m))==0 && c.id==a.id && c.owner==a.owner);
    fp_ticket_request_t bad=req(1,0,FP_TICKET_ANY_CORE);
    assert(fp_ticket_acquire(&m,&bad,&c)==FP_TICKET_INVALID);
    bad=req(1,1,4);
    assert(fp_ticket_acquire(&m,&bad,&c)==FP_TICKET_INVALID);
    assert(memcmp(&m,&before,sizeof(m))==0);

    assert(fp_ticket_init(&other,800000,800000)==FP_TICKET_OK);
    assert(fp_ticket_acquire(&other,&mp3,&c)==FP_TICKET_OK);
    assert(c.id==a.id);
    assert(fp_ticket_release(&m,c)==FP_TICKET_STALE); // foreign manager
    assert(fp_ticket_release(&m,b)==FP_TICKET_OK);
    assert(fp_ticket_acquire(&m,&mp3,&c)==FP_TICKET_OK);
    assert(c.id!=b.id);
    assert(fp_ticket_release(&m,b)==FP_TICKET_STALE); // reused slot
    assert(fp_ticket_release(&m,c)==FP_TICKET_OK);
    assert(fp_ticket_release(&m,a)==FP_TICKET_OK);
    assert(fp_ticket_release(&m,a)==FP_TICKET_STALE);
    assert(fp_ticket_snapshot(&m,&snap)==FP_TICKET_OK);
    assert(!snap.used_ppm[0] && !snap.used_ppm[1]);

    fp_ticket_manager_t asymmetric;
    assert(fp_ticket_init(&asymmetric,400000,800000)==FP_TICKET_OK);
    assert(fp_ticket_acquire(&asymmetric,&mp3,&a)==FP_TICKET_OK);
    assert(fp_ticket_get(&asymmetric,a,&info)==FP_TICKET_OK && info.core==1);

    fp_ticket_manager_t disabled;
    assert(fp_ticket_init(&disabled,0,1000000)==FP_TICKET_OK);
    fp_ticket_request_t tiny=req(1,UINT32_MAX,FP_TICKET_ANY_CORE);
    fp_ticket_t tickets[FP_TICKET_SLOTS];
    for(unsigned i=0;i<FP_TICKET_SLOTS;i++) {
        assert(fp_ticket_acquire(&disabled,&tiny,&tickets[i])==FP_TICKET_OK);
        assert(fp_ticket_get(&disabled,tickets[i],&info)==FP_TICKET_OK);
        assert(info.core==1 && info.load_ppm==1);
    }
    assert(fp_ticket_acquire(&disabled,&tiny,&a)==FP_TICKET_NO_SLOT);
    for(unsigned i=0;i<FP_TICKET_SLOTS;i++)
        assert(fp_ticket_release(&disabled,tickets[i])==FP_TICKET_OK);
    fp_ticket_request_t full=req(UINT32_MAX,UINT32_MAX,FP_TICKET_ANY_CORE);
    assert(fp_ticket_acquire(&disabled,&full,&a)==FP_TICKET_OK);
    assert(fp_ticket_get(&disabled,a,&info)==FP_TICKET_OK && info.load_ppm==1000000);
    assert(fp_ticket_acquire(&disabled,&tiny,&b)==FP_TICKET_NO_CAPACITY);
    assert(fp_ticket_release(&disabled,a)==FP_TICKET_OK);

    disabled.next_id=UINT32_MAX; // test exhaustion without 2^32 acquisitions
    assert(fp_ticket_acquire(&disabled,&tiny,&a)==FP_TICKET_OK);
    assert(fp_ticket_release(&disabled,a)==FP_TICKET_OK);
    assert(fp_ticket_acquire(&disabled,&tiny,&b)==FP_TICKET_ID_EXHAUSTED);
    assert(fp_ticket_init(NULL,1,1)==FP_TICKET_INVALID);
    assert(fp_ticket_get(&m,(fp_ticket_t){0},&info)==FP_TICKET_STALE);
    fp_ticket_manager_t mixed;
    static const fp_ticket_request_t fp={.runtime_us=1,.period_us=10,
        .allowed_cores=FP_TICKET_ANY_CORE,.flags=FP_TICKET_USES_FP};
    static const fp_ticket_request_t nonfp={.runtime_us=1,.period_us=10,
        .allowed_cores=FP_TICKET_ANY_CORE};
    fp_ticket_t f0, f1, n0, n1;
    assert(fp_ticket_init(&mixed,1000000,1000000)==FP_TICKET_OK);
    assert(fp_ticket_acquire(&mixed,&fp,&f0)==FP_TICKET_OK);
    assert(fp_ticket_get(&mixed,f0,&info)==FP_TICKET_OK && info.core==0);
    assert(fp_ticket_acquire(&mixed,&fp,&f1)==FP_TICKET_OK);
    assert(fp_ticket_get(&mixed,f1,&info)==FP_TICKET_OK && info.core==1);
    before=mixed;
    c=f0;
    assert(fp_ticket_acquire(&mixed,&fp,&c)==FP_TICKET_FP_BUSY);
    assert(memcmp(&mixed,&before,sizeof(mixed))==0 && c.id==f0.id);
    assert(fp_ticket_acquire(&mixed,&nonfp,&n0)==FP_TICKET_OK);
    assert(fp_ticket_get(&mixed,n0,&info)==FP_TICKET_OK && info.core==0);
    assert(fp_ticket_acquire(&mixed,&nonfp,&n1)==FP_TICKET_OK);
    assert(fp_ticket_get(&mixed,n1,&info)==FP_TICKET_OK && info.core==1);
    assert(fp_ticket_snapshot(&mixed,&snap)==FP_TICKET_OK);
    assert(snap.fp_cores==3 && snap.non_fp_reservations[0]==1 &&
           snap.non_fp_reservations[1]==1 && snap.used_ppm[0]==200000 &&
           snap.used_ppm[1]==200000);
    assert(fp_ticket_release(&mixed,f0)==FP_TICKET_OK);
    assert(fp_ticket_acquire(&mixed,&fp,&a)==FP_TICKET_OK);
    assert(fp_ticket_get(&mixed,a,&info)==FP_TICKET_OK && info.core==0);
    assert(fp_ticket_release(&mixed,f0)==FP_TICKET_STALE);
    assert(fp_ticket_release(&mixed,a)==FP_TICKET_OK);
    assert(fp_ticket_release(&mixed,f1)==FP_TICKET_OK);
    assert(fp_ticket_release(&mixed,n0)==FP_TICKET_OK);
    assert(fp_ticket_release(&mixed,n1)==FP_TICKET_OK);
    assert(!mixed.live_mask && !mixed.core1_mask && !mixed.fp_mask);

    static const fp_ticket_request_t fixed_fp={.runtime_us=1,.period_us=10,
        .allowed_cores=FP_TICKET_CORE_1,.flags=FP_TICKET_USES_FP};
    assert(fp_ticket_acquire(&mixed,&fixed_fp,&f1)==FP_TICKET_OK);
    assert(fp_ticket_acquire(&mixed,&fixed_fp,&a)==FP_TICKET_FP_BUSY);
    assert(fp_ticket_acquire(&mixed,&nonfp,&n0)==FP_TICKET_OK);
    assert(fp_ticket_get(&mixed,n0,&info)==FP_TICKET_OK && info.core==0);
    assert(fp_ticket_acquire(&mixed,&nonfp,&n1)==FP_TICKET_OK);
    assert(fp_ticket_get(&mixed,n1,&info)==FP_TICKET_OK && info.core==1);
    bad=req(1,10,FP_TICKET_ANY_CORE);
    bad.flags=2;
    assert(fp_ticket_acquire(&mixed,&bad,&a)==FP_TICKET_INVALID);
    puts("fp_ticket: all tests passed");
    return 0;
}
