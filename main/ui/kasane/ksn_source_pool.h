#ifndef KSN_SOURCE_POOL_H
#define KSN_SOURCE_POOL_H
#include "ksn_types.h"
#include <stdatomic.h>

/* One producer, many readers. The caller supplies three aligned payload slots
 * and writes a complete value in-place before publish. A published slot is
 * immutable until every reader releases it. Neither side allocates or waits;
 * pool exhaustion returns KSN_BUSY so the producer can coalesce/skip. */
#define KSN_SOURCE_POOL_SLOTS 3u
typedef struct {
    atomic_uint state[KSN_SOURCE_POOL_SLOTS];
    atomic_uint current,revision,skipped;
    atomic_bool writer_busy;
    unsigned char *storage;
    size_t stride;
} ksn_source_pool;
typedef struct {
    ksn_source_pool *pool;
    void *data;
    uint8_t slot;
    bool active;
} ksn_source_write;
typedef struct {
    ksn_source_pool *pool;
    const void *data;
    uint32_t revision;
    uint8_t slot;
    bool active;
} ksn_source_read;

/* Cold init only: never reset/destroy a pool while a lease is alive. Payload
 * alignment is the caller's responsibility; storage_bytes is checked. Both
 * lease objects must be zero-initialized before their first begin/acquire;
 * publish/cancel/release reset them for reuse. */
ksn_result ksn_source_pool_init(ksn_source_pool *pool,void *storage,
                                size_t storage_bytes,size_t stride);
ksn_result ksn_source_pool_begin(ksn_source_pool *pool,ksn_source_write *write);
ksn_result ksn_source_pool_publish(ksn_source_write *write,uint32_t *revision);
ksn_result ksn_source_pool_cancel(ksn_source_write *write);
ksn_result ksn_source_pool_acquire(ksn_source_pool *pool,ksn_source_read *read);
ksn_result ksn_source_pool_release(ksn_source_read *read);
uint32_t ksn_source_pool_latest(const ksn_source_pool *pool);
uint32_t ksn_source_pool_skipped(const ksn_source_pool *pool);
#endif
