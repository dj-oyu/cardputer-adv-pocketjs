#include "sys_clock.h"
#include <stdatomic.h>
#include <stddef.h>
#include <sys/time.h>
#ifdef ESP_PLATFORM
#include "vm_wake.h"
#endif

enum { TRUST_UNKNOWN=0, TRUST_YES=1, TRUST_RTC=2, TRUST_NO=-1 };
static atomic_int trust;
static atomic_bool update_requested=true;

void sys_clock_set_synchronized(bool value) {
    atomic_store_explicit(&trust,value?TRUST_YES:TRUST_NO,memory_order_release);
    bool pending=atomic_exchange_explicit(&update_requested,true,memory_order_acq_rel);
#ifdef ESP_PLATFORM
    /* Publish first. The existing counted wake covers arrival before wait. */
    if(!pending)vm_wake_post();
#else
    (void)pending;
#endif
}
bool sys_clock_update_pending(void){return atomic_load_explicit(&update_requested,memory_order_acquire);}
bool sys_clock_take_update(void){
    return atomic_exchange_explicit(&update_requested,false,memory_order_acq_rel);
}

sys_clock_sample sys_clock_read(void) {
    int state=atomic_load_explicit(&trust,memory_order_acquire);
    sys_clock_sample sample={.trusted=state>0,.synchronized=state==TRUST_YES};
    if(state==TRUST_NO)return sample;
    struct timeval now;
    if(gettimeofday(&now,NULL)!=0||now.tv_usec<0||now.tv_usec>=1000000)
        return sample;
    sample.seconds=(int64_t)now.tv_sec;
    sample.microseconds=(int32_t)now.tv_usec;
    sample.available=true;
    /* The RTC survives resets, unlike this flag. Reject its unset epoch, but
     * do not impose solar's ephemeris range on every clock consumer. CAS keeps
     * a concurrent explicit revocation from being overwritten by discovery. */
    if(state==TRUST_UNKNOWN&&sample.seconds>=INT64_C(946684800)) {
        int expected=TRUST_UNKNOWN;
        atomic_compare_exchange_strong_explicit(&trust,&expected,TRUST_RTC,
            memory_order_acq_rel,memory_order_acquire);
        sample.trusted=expected!=TRUST_NO;
        sample.synchronized=expected==TRUST_YES;
        if(expected==TRUST_NO)sample.available=false;
    }
    return sample;
}
