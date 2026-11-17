#ifndef AY_STREAM_GRAPH_OCCLUSION_BACKEND_H
#define AY_STREAM_GRAPH_OCCLUSION_BACKEND_H
#include "pocket/pocket_stream_candidate.h"
#include "../../hone/native_geometry/alpha_graph_occlusion_band_candidate.h"
/* Trusted opt-in hooks for existing actor registration. work, row/depth/scratch
 * remain exclusively owned until successful actor unbind. Payload is an immutable
 * hone_graph_band_payload (or first member of ay_material_pending), frozen with
 * geometry through physical drain/ACK and READY redraw. No automatic registration. */
static inline bool ay_stream_graph_occlusion_preflight(void*context,const ksn_mesh_snapshot*s,const void*payload){
 return hone_graph_occlusion_band_preflight(s,payload,context)!=0;
}
static inline ksn_result ay_stream_graph_occlusion_band(void*context,const ksn_mesh_snapshot*s,const void*payload,
 unsigned y,unsigned rows,uint16_t*pixels,float*depth){
 (void)depth;return hone_graph_occlusion_band_render(s,payload,context,y,rows,pixels)?KSN_OK:KSN_INVALID;
}
static inline pocket_stream_material_hooks ay_stream_graph_occlusion_hooks(hone_graph_band_work*work){
 return (pocket_stream_material_hooks){work,ay_stream_graph_occlusion_preflight,ay_stream_graph_occlusion_band};
}
#endif
