#ifndef POCKET_STREAM_CANDIDATE_H
#define POCKET_STREAM_CANDIDATE_H
#include "ayatsuri/ayatsuri_stream_frame_owner_candidate.h"
#include "ui/kasane/ksn_runtime.h"

/* Explicit native diagnostic bridge, not an installed persistent APP source.
 * Zero initialize. Keep frame/depth[240] alive through pending drain recovery.
 * Same synchronous UI owner as pocket; no reentry or other presentation while
 * held. Physical port present must copy its input before returning (Kasane ABI).
 * Caller supplies dedicated depth scratch disjoint from all model/frame/port
 * buffers. No private model or material evaluator is installed by this bridge. */
typedef struct {
    ayatsuri_stream_frame_owner *frame;
    ayatsuri_stream_frame_token token;
    float *depth;
    bool held,busy,ready_borrow;
    const ksn_mesh_snapshot *snapshot;
} pocket_stream_candidate;

ksn_result pocket_stream_candidate_present(pocket_stream_candidate*,
    ayatsuri_stream_frame_owner*,const void*,uint32_t,float depth[240],
    ayatsuri_stream_frame_preflight,void*,const ksn_display_port*,
    const ksn_display_completion*,ksn_render_stats*);
/* Retry ONLY the authoritative display drain; never replay or ACK the failed
 * candidate. On successful drain roll back its pin and invalidate for repair. */
ksn_result pocket_stream_candidate_recover(pocket_stream_candidate*,const ksn_display_completion*);
/* Redraw committed packets without generating or promoting a new frame. An
 * unsuccessful physical drain retains the committed borrow and blocks produce. */
ksn_result pocket_stream_candidate_redraw(pocket_stream_candidate*,
    ayatsuri_stream_frame_owner*,const void*,uint32_t,float depth[240],
    ayatsuri_stream_frame_preflight,void*,const ksn_display_port*,
    const ksn_display_completion*,ksn_render_stats*);
#endif
