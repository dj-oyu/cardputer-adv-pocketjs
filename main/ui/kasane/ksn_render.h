#ifndef KSN_RENDER_H
#define KSN_RENDER_H
#include "ksn_core.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint32_t bands,transferred_bytes; } ksn_render_stats;
/* Boundary 2a switch: 1 (default) decodes each frame command once per frame
 * into a renderer-owned view (KSN_COMMANDS views + a KSN_TEXT_BYTES frame text
 * pool, static); 0 reads the command again for every band, which is the pre-2a
 * reference path. Both paths live in one binary so a same-binary A/B can
 * compare read counts and frame time without a rebuild. Rendering is
 * pixel-neutral either way; only the number of ksn_core_read calls changes. */
extern int g_ksn_decode_once;
/* Bounded production subset: rect, round rect, 1/2 px stroke, two-color
 * horizontal/vertical gradient, font-port TEXT coverage, command alpha and
 * isolated group opacity. The borrowed text port and its immutable resources
 * must remain replayable for pending and committed frames. Coverage scratch is
 * 64 bytes (plus the existing 256-byte group tile), never a text surface.
 * Unsupported commands are rejected before the first transfer. The caller
 * schedules retry/discard; callbacks may request invalidation, but must not
 * otherwise mutate the core or reenter JS/presentation. prepare_frame consumes
 * preexisting invalidation; later invalidation survives the acknowledgement. */
ksn_result ksn_render_rects(ksn_core *core,const ksn_display_port *display,ksn_render_stats *stats);
#ifdef __cplusplus
}
#endif
#endif
