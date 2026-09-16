#pragma once
#include "ksn_ports.h"
// Built-in Latin + mapped Japanese faces; immutable, no glyph cache or heap.
extern const ksn_text_port ksn_font_port;
/* Same-binary A/B arm for the span kernel: 1 (shipped) gives a glyph cell only
 * the chunk columns it owns, 0 is the pre-change walk over the whole chunk.
 * Both arms write the same pixels; see main/text/ksn_font.c. */
extern int g_ksn_span_narrow;
