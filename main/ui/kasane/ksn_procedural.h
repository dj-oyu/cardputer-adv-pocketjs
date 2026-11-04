#ifndef KSN_PROCEDURAL_H
#define KSN_PROCEDURAL_H
#include <stdbool.h>
#include <stdint.h>

/* The frame Shiki draws into (ksn_shiki.c) and the band renderer that turns
 * it into pixels: an ordered list of line segments in panel coordinates. The
 * float VM that first wrote these frames went in Shiki v2.0. A pattern line
 * (Shiki's LINE_PATTERN, docs/kasane/shiki-spec.md) takes two entries: a
 * pixel's colour comes from a periodic bit pattern along the line. */
#define KSN_PROC_SEGMENTS 1024
#define KSN_PROC_RASTER_STEPS 8192
#define KSN_PROC_W 240
#define KSN_PROC_H 135

typedef struct { int16_t x0,y0,x1,y1; uint16_t color; } ksn_proc_segment;
/* A pattern line (LINE_PATTERN) takes two consecutive entries, so the entry
 * keeps its size and a frame without one is byte for byte what it was
 * (docs/kasane/crowd-primitives-design.md):
 *   geometry    x0 + KSN_PROC_EXT_PATTERN (no coordinate, -480..720, reaches
 *               KSN_PROC_EXT_MIN), y0, x1, y1, color = colour A
 *   parameters  x0 = pattern bits 0..15, y0 = 0x8000 | bits 16..23 |
 *               period << 8 | has-B << 13, x1 = u0, y1 = 0x8000 | du (8.8
 *               cells, each under period * 256), color = colour B
 * The parameter entry's y0 and y1 are negative, so a renderer's vertical
 * range test passes over it like an entry above the panel: only an entry
 * that reaches a band pays the test for the bias. frame.ext (what was
 * padding) says whether a frame holds any; a frame without takes the loop it
 * always took. */
#define KSN_PROC_EXT_PATTERN 4096
#define KSN_PROC_EXT_MIN 2048
#define KSN_PROC_PATTERN_BITS 24
#define KSN_PROC_PATTERN_BLOCK 6
typedef struct {
    uint16_t count;
    uint16_t raster_steps;
    bool ready;
    uint8_t ext;
    ksn_proc_segment segments[KSN_PROC_SEGMENTS];
} ksn_proc_frame;
_Static_assert(sizeof(ksn_proc_frame)==6+KSN_PROC_SEGMENTS*sizeof(ksn_proc_segment),
               "ext took the padding byte: the frame did not grow");
/* For code that walks a frame's geometry (damage, validation): the entry at
 * *i (a pattern line's with the bias removed), whether it is a pattern line,
 * and its raster cost; *i moves past a pattern line's parameters. False at
 * the end, or for a pattern line cut off by count or whose parameter entry
 * is not one. */
static inline bool ksn_proc_frame_next(const ksn_proc_frame *frame,unsigned *i,
                                       ksn_proc_segment *geometry,bool *pattern,
                                       unsigned *cost){
    if(*i>=frame->count)return false;
    *geometry=frame->segments[(*i)++];
    *pattern=frame->ext&&geometry->x0>=KSN_PROC_EXT_MIN;
    if(*pattern){
        if(*i>=frame->count)return false;
        const ksn_proc_segment *q=&frame->segments[(*i)++];
        const unsigned period=(uint16_t)q->y0>>8&31u;
        if(q->y0>=0||q->y1>=0||period<1||period>KSN_PROC_PATTERN_BITS||
           (uint16_t)q->x1>=period*256u||((uint16_t)q->y1&0x7fffu)>=period*256u)return false;
        geometry->x0=(int16_t)(geometry->x0-KSN_PROC_EXT_PATTERN);
    }
    unsigned dx=(unsigned)(geometry->x1>geometry->x0?geometry->x1-geometry->x0:geometry->x0-geometry->x1);
    unsigned dy=(unsigned)(geometry->y1>geometry->y0?geometry->y1-geometry->y0:geometry->y0-geometry->y1);
    *cost=(dx>dy?dx:dy)+1;
    return true;
}
/* Replays only the requested rows over an existing RGB565 backdrop. */
bool ksn_proc_render_band(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height);
/* The same, looking only at entries [first, end): the caller knows no entry
 * outside them touches rows y..y+height-1 (pocket_proc's per-band ranges,
 * docs/platform/lcd-dma-driver.md 9.5). first must start an entry. The
 * pixels are those of ksn_proc_render_band. */
bool ksn_proc_render_band_range(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height,
                                unsigned first,unsigned end);
/* 1 (default): a horizontal line (y0 == y1) is clipped to the panel once and
 * filled two pixels a store, instead of walked a pixel at a time. 0 is the
 * Bresenham walk for every line, the reference for a same-binary A/B. The
 * pixels are the same: a horizontal Bresenham line is exactly x0..x1 on its
 * row. */
extern int g_ksn_proc_hspan;

#endif
