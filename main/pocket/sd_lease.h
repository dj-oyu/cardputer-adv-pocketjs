// Card-free part of the SD read lease protocol. Only the owner task mutates
// the registry. A worker may read revoked and set fault/ack on its own token.
#ifndef SD_LEASE_H
#define SD_LEASE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

typedef struct sd_lease {
    struct sd_lease *next;       // owner task only
    uint32_t key, generation;   // immutable while attached
    atomic_bool revoked;        // owner -> worker cancellation
    atomic_bool fault;          // worker -> owner removal observation
    atomic_bool ack;            // worker -> owner teardown acknowledgement
} sd_lease_t;

typedef struct {
    sd_lease_t *head;           // owner task only
    uint32_t generation;
    bool mounted, pending_unmount;
} sd_lease_registry_t;

void sd_lease_registry_reset(sd_lease_registry_t *r);
bool sd_lease_registry_mount(sd_lease_registry_t *r, uint32_t generation);
bool sd_lease_attach(sd_lease_registry_t *r, sd_lease_t *l,
                     uint32_t key, uint32_t generation);
void sd_lease_cancel_all(sd_lease_registry_t *r);
bool sd_lease_busy(const sd_lease_registry_t *r, uint32_t key);
bool sd_lease_take_fault(sd_lease_registry_t *r);
bool sd_lease_ready_to_unmount(const sd_lease_registry_t *r);
void sd_lease_unmounted(sd_lease_registry_t *r);

// ACK means the worker has stopped touching its file, ring and session. An
// unstarted worker may ACK on the owner task. A timed-out worker cannot be
// detached: keep the entire owning session allocated until an ACK arrives.
void sd_lease_ack(sd_lease_t *l);
bool sd_lease_detach(sd_lease_registry_t *r, sd_lease_t *l);

#endif
