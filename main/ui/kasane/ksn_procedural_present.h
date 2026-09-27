#ifndef KSN_PROCEDURAL_PRESENT_H
#define KSN_PROCEDURAL_PRESENT_H
#include "ksn_procedural_layers.h"
#include "ksn_render.h"

/* Host integration boundary. If a core submission and procedural candidate
 * coexist, the core submission is presented over committed procedural frames
 * first. The candidate stays pending for the next call. The caller owns the
 * core, display, surfaces and backdrop.
 * An attempted procedural transfer that fails discards its candidate; the next
 * call repairs the panel from committed frames and core commands. A failure
 * before transfer leaves the candidate retryable. */
ksn_result ksn_proc_layers_present(ksn_proc_layers *layers,ksn_core *core,
                                   const ksn_display_port *display,
                                   ksn_render_stats *stats);
#endif
