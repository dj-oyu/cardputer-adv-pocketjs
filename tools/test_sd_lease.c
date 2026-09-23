// Host: compile this file together with main/pocket/sd_lease.c using C11.
#include <stdio.h>
#include "sd_lease.h"

static int failures;
#define CHECK(x) do { if(!(x)) { \
    fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); failures++; \
} } while(0)

int main(void) {
    sd_lease_registry_t r;
    sd_lease_t first, second, stale;
    sd_lease_registry_reset(&r);
    CHECK(!sd_lease_attach(&r,&first,11,1));
    CHECK(sd_lease_registry_mount(&r,1));
    CHECK(!sd_lease_attach(&r,&stale,11,2));
    CHECK(sd_lease_attach(&r,&first,11,1));
    CHECK(!sd_lease_attach(&r,&first,11,1));
    CHECK(sd_lease_attach(&r,&second,22,1));
    CHECK(sd_lease_busy(&r,11)&&sd_lease_busy(&r,22));
    CHECK(!sd_lease_busy(&r,33));

    // A worker reports a failed card command without mutating media state.
    atomic_store(&first.fault,true);
    CHECK(sd_lease_take_fault(&r));
    CHECK(!sd_lease_take_fault(&r));

    // Removal is observable immediately but cannot physically unmount while
    // either session owns a FILE, even when one worker finishes late.
    sd_lease_cancel_all(&r);
    CHECK(atomic_load(&first.revoked)&&atomic_load(&second.revoked));
    CHECK(!sd_lease_ready_to_unmount(&r));
    CHECK(!sd_lease_registry_mount(&r,2));
    CHECK(!sd_lease_attach(&r,&stale,33,1));
    CHECK(!sd_lease_detach(&r,&first)); // cancellation is not an ACK
    sd_lease_ack(&second);
    CHECK(sd_lease_detach(&r,&second));
    CHECK(!sd_lease_ready_to_unmount(&r));
    CHECK(sd_lease_busy(&r,11));

    // Timeout/quarantine: retain the whole old session. A late completion may
    // ACK later; only then may the owner detach and finish the unmount.
    sd_lease_ack(&first);
    CHECK(sd_lease_detach(&r,&first));
    CHECK(sd_lease_ready_to_unmount(&r));
    sd_lease_unmounted(&r);
    CHECK(!r.pending_unmount);
    CHECK(sd_lease_registry_mount(&r,2));
    CHECK(sd_lease_attach(&r,&stale,11,2));
    CHECK(!atomic_load(&stale.revoked));
    sd_lease_ack(&stale);
    CHECK(sd_lease_detach(&r,&stale));

    printf("sd lease: %s\n",failures?"FAIL":"OK");
    return failures?1:0;
}
