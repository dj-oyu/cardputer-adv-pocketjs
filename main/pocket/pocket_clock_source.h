#ifndef POCKET_CLOCK_SOURCE_H
#define POCKET_CLOCK_SOURCE_H
#include "ui/kasane/ksn_source.h"

/* Clock-owned storage. Snapshots borrow face/tag only until release. */
typedef struct {
    uint64_t key,revision;
    uint32_t generation;
    bool initialized;
    char face[6],tag[8];
    ksn_schema_value fields[2];
} pocket_clock_source_state;

ksn_result pocket_clock_source_open(void *storage,ksn_source_provider *out);
void pocket_clock_source_registered(void *storage,ksn_source_handle handle);
#endif
