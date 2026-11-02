#ifndef KSN_MESH_DEPTH_H
#define KSN_MESH_DEPTH_H
#include "ayatsuri/compact_opaque.h"
#include "ksn_procedural.h"
/* Explicit bounded opaque scope. All packets in a snapshot share depth.
 * Larger evaluated view depth wins; exact ties retain first submitted face.
 * No alpha/holes/clips. No source pointers survive append. */
typedef struct { co_buffer packets; uint32_t work,work_limit; bool enabled; } ksn_mesh_snapshot;
bool ksn_mesh_append(ksn_mesh_snapshot *s,const ksn_proc_gouraud_patch *p,const float *depth,uint16_t node);
/* scratch has width*rows floats, rows is 1 or 8. Caller supplies background
 * pixels. A complete band can be processed in smaller scratch-sized strips. */
bool ksn_mesh_render(const ksn_mesh_snapshot *s,uint16_t *pixels,unsigned y,unsigned height,
                     float *scratch,unsigned rows);
#endif
