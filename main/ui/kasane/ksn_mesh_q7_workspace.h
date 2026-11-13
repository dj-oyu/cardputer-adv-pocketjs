#ifndef KSN_MESH_Q7_WORKSPACE_H
#define KSN_MESH_Q7_WORKSPACE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Retain and free the original allocation, never this interior pointer. */
static inline bool ksn_mesh_q7_workspace_allocation(size_t bytes,size_t alignment,size_t *total){
    if(!total||!bytes||!alignment||bytes>SIZE_MAX-(alignment-1))return false;
    *total=bytes+alignment-1;return true;
}
static inline void *ksn_mesh_q7_workspace_align(void *raw,size_t total,size_t bytes,size_t alignment){
    if(!raw||!bytes||!alignment)return NULL;
    uintptr_t address=(uintptr_t)raw;
    size_t padding=(alignment-address%alignment)%alignment;
    if(padding>total||bytes>total-padding||address>UINTPTR_MAX-padding)return NULL;
    return (void *)(address+padding);
}
#endif
