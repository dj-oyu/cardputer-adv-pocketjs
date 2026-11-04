#ifndef KSN_PROCEDURAL_H
#define KSN_PROCEDURAL_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
/* Shiki frame with copied native analytic/Gouraud packets; no float VM. */
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
#define KSN_PROC_EXT_ELLIPSOID 8192
#define KSN_PROC_EXT_BLEND_ELLIPSOIDS 12288
#define KSN_PROC_EXT_CLIPPED_ELLIPSOID 16384
#define KSN_PROC_EXT_BLEND_REGION 20480
#define KSN_PROC_EXT_GOURAUD_PATCH 24576
#define KSN_PROC_EXT_MIN 2048
#define KSN_PROC_PATTERN_BITS 24
#define KSN_PROC_PATTERN_BLOCK 6

/* Inline, immutable analytic patch. A candidate owns this numeric snapshot,
 * not a JS buffer or a framebuffer. Padding is explicit for stable hashes. */
typedef struct {
    float screen[8],normal[9],light[9],material[8],texture[9];
    uint16_t surface[5],reserved;
} ksn_proc_ellipsoid;
#define KSN_PROC_ELLIPSOID_WORDS ((sizeof(ksn_proc_ellipsoid)+sizeof(ksn_proc_segment)-1)/sizeof(ksn_proc_segment))
#define KSN_PROC_ELLIPSOID_ENTRIES (1+KSN_PROC_ELLIPSOID_WORDS)
_Static_assert(sizeof(ksn_proc_ellipsoid)==184,"analytic packet layout");
/* Bounds are clipped pixel centres. False means invalid; an offscreen patch
 * has right<left or bottom<top and no raster cost. */
bool ksn_proc_ellipsoid_bounds(const ksn_proc_ellipsoid *p,int *left,int *top,int *right,int *bottom);
/* Screen-space inclusion, not volume intersection or depth occlusion. Empty
 * count means draw nothing; 3..8 vertices form a strictly convex polygon in
 * either winding. Finite float32 coordinates are in -4096..4096 screen pixels;
 * inclusion/row intersections use binary64 intermediates, closed pixel edges.
 * Unused vertices/reserved bytes are canonical zero. */
#define KSN_PROC_CLIP_VERTICES 8
typedef struct {uint8_t count,reserved[3];float xy[16];} ksn_proc_ellipsoid_clip;
typedef struct {ksn_proc_ellipsoid patch;ksn_proc_ellipsoid_clip clip;} ksn_proc_clipped_ellipsoid;
_Static_assert(sizeof(ksn_proc_clipped_ellipsoid)==252,"clipped packet layout");
#define KSN_PROC_CLIPPED_WORDS ((sizeof(ksn_proc_clipped_ellipsoid)+sizeof(ksn_proc_segment)-1)/sizeof(ksn_proc_segment))
#define KSN_PROC_CLIPPED_ENTRIES (1+KSN_PROC_CLIPPED_WORDS)
bool ksn_proc_clipped_ellipsoid_read(const ksn_proc_segment *data,unsigned words,ksn_proc_clipped_ellipsoid *out);
bool ksn_proc_clipped_ellipsoid_bounds(const ksn_proc_clipped_ellipsoid *p,int *left,int *top,int *right,int *bottom);
/* A shared surface over bounded analytic lobes. The serialized packet holds
 * only count lobes; memcpy is required at segment boundaries (10-byte stride). */
#define KSN_PROC_BLEND_LOBES 8
typedef struct { float screen[8],normal[9],depth[4]; } ksn_proc_blend_lobe;
typedef struct {
    float light[9],material[11],controls[2];
    uint16_t surface[3];
    uint8_t count,reserved;
} ksn_proc_blend_header;
typedef struct {
    ksn_proc_blend_header header;
    ksn_proc_blend_lobe lobes[KSN_PROC_BLEND_LOBES];
} ksn_proc_blend_ellipsoids;
_Static_assert(sizeof(ksn_proc_blend_header)==96,"blend header layout");
_Static_assert(sizeof(ksn_proc_blend_lobe)==84,"blend lobe layout");
#define KSN_PROC_BLEND_BYTES(n) (sizeof(ksn_proc_blend_header)+(n)*sizeof(ksn_proc_blend_lobe))
#define KSN_PROC_BLEND_WORDS(n) ((KSN_PROC_BLEND_BYTES(n)+sizeof(ksn_proc_segment)-1)/sizeof(ksn_proc_segment))
bool ksn_proc_blend_bounds(const ksn_proc_blend_ellipsoids *p,int *left,int *top,int *right,int *bottom);
bool ksn_proc_blend_read(const ksn_proc_segment *data,unsigned words,ksn_proc_blend_ellipsoids *p,unsigned *used);
/* A common model-local plane transformed to f=A*x+B*y+C*depth+D.
 * Serialized after the raw legacy blend bytes, before segment padding. */
typedef struct {float plane[4],feather;uint16_t color,reserved;} ksn_proc_blend_region_mask;
_Static_assert(sizeof(ksn_proc_blend_region_mask)==24,"blend region mask layout");
#define KSN_PROC_BLEND_REGION_BYTES(n) (KSN_PROC_BLEND_BYTES(n)+sizeof(ksn_proc_blend_region_mask))
#define KSN_PROC_BLEND_REGION_WORDS(n) ((KSN_PROC_BLEND_REGION_BYTES(n)+sizeof(ksn_proc_segment)-1)/sizeof(ksn_proc_segment))
bool ksn_proc_blend_region_read(const ksn_proc_segment *data,unsigned words,
    ksn_proc_blend_ellipsoids *p,ksn_proc_blend_region_mask *mask,unsigned *used);
bool ksn_proc_blend_region_bounds(const ksn_proc_blend_ellipsoids *p,const ksn_proc_blend_region_mask *mask,
    int *left,int *top,int *right,int *bottom);
/* Screen-linear, already-lit vertex colours; mask selects B at >=0. No
 * lighting, depth buffer or alpha is implied. Serialized arrays use only the
 * declared counts; fixed-array unused storage and segment padding are zero. */
#define KSN_PROC_GOURAUD_VERTICES 32
#define KSN_PROC_GOURAUD_TRIANGLES 64
#define KSN_PROC_GOURAUD_HOLES 2
typedef struct {float x,y,mask;uint16_t color_a,color_b;} ksn_proc_gouraud_vertex;
typedef struct {uint8_t vertices,triangles,holes,reserved;uint32_t reserved2;} ksn_proc_gouraud_header;
typedef struct {
    ksn_proc_gouraud_header header;
    ksn_proc_gouraud_vertex vertices[KSN_PROC_GOURAUD_VERTICES];
    uint16_t indices[3*KSN_PROC_GOURAUD_TRIANGLES];
    ksn_proc_ellipsoid_clip clips[KSN_PROC_GOURAUD_HOLES];
} ksn_proc_gouraud_patch;
_Static_assert(sizeof(ksn_proc_gouraud_vertex)==16&&sizeof(ksn_proc_gouraud_header)==8&&
               sizeof(ksn_proc_ellipsoid_clip)==68&&sizeof(ksn_proc_gouraud_patch)==1040,"Gouraud packet layout");
#define KSN_PROC_GOURAUD_BYTES(v,t,h) (8u+16u*(v)+6u*(t)+68u*(h))
#define KSN_PROC_GOURAUD_WORDS(v,t,h) ((KSN_PROC_GOURAUD_BYTES(v,t,h)+sizeof(ksn_proc_segment)-1)/sizeof(ksn_proc_segment))
bool ksn_proc_gouraud_read(const ksn_proc_segment *data,unsigned words,ksn_proc_gouraud_patch *p,unsigned *used);
/* Cost is the sum of clipped integer triangle bounding-box areas, before
 * holes/top-left coverage; the submitting owner applies its frame budget. */
bool ksn_proc_gouraud_bounds(const ksn_proc_gouraud_patch *p,int *left,int *top,int *right,int *bottom,unsigned *cost);
typedef struct {
    uint16_t count;
    uint16_t raster_steps;
    bool ready;
    uint8_t ext;
    ksn_proc_segment segments[KSN_PROC_SEGMENTS];
} ksn_proc_frame;
_Static_assert(sizeof(ksn_proc_frame)==6+KSN_PROC_SEGMENTS*sizeof(ksn_proc_segment),
               "ext took the padding byte: the frame did not grow");
/* Out of line so legacy walks do not reserve the new packet's stack storage. */
bool ksn_proc_gouraud_frame_next(const ksn_proc_frame *frame,unsigned *i,ksn_proc_segment *geometry,unsigned *cost);
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
        if(geometry->x0>=KSN_PROC_EXT_GOURAUD_PATCH-480)
            return ksn_proc_gouraud_frame_next(frame,i,geometry,cost);
        if(geometry->x0>=KSN_PROC_EXT_BLEND_REGION-480){
            ksn_proc_blend_ellipsoids p;ksn_proc_blend_region_mask mask;unsigned used;
            if(!ksn_proc_blend_region_read(frame->segments+*i,frame->count-*i,&p,&mask,&used)){
                *i=frame->count+1u;return false;
            }
            int left,top,right,bottom;
            const bool valid=ksn_proc_blend_region_bounds(&p,&mask,&left,&top,&right,&bottom);
            geometry->x0=(int16_t)(geometry->x0-KSN_PROC_EXT_BLEND_REGION);
            *i+=used;
            if(!valid||left!=geometry->x0||top!=geometry->y0||right!=geometry->x1||bottom!=geometry->y1||left>right||top>bottom){
                *i=frame->count+1u;return false;
            }
            *cost=(unsigned)(right-left+1)*(unsigned)(bottom-top+1)*p.header.count;return true;
        }
        if(geometry->x0>=KSN_PROC_EXT_CLIPPED_ELLIPSOID-480){
            ksn_proc_clipped_ellipsoid p;
            if(!ksn_proc_clipped_ellipsoid_read(frame->segments+*i,frame->count-*i,&p)){
                *i=frame->count+1u;return false;
            }
            *i+=(unsigned)KSN_PROC_CLIPPED_WORDS;
            int left,top,right,bottom;
            const bool valid=ksn_proc_clipped_ellipsoid_bounds(&p,&left,&top,&right,&bottom);
            geometry->x0=(int16_t)(geometry->x0-KSN_PROC_EXT_CLIPPED_ELLIPSOID);
            if(!valid||left!=geometry->x0||top!=geometry->y0||right!=geometry->x1||bottom!=geometry->y1||left>right||top>bottom){
                *i=frame->count+1u;return false;
            }
            *cost=(unsigned)(right-left+1)*(unsigned)(bottom-top+1);return true;
        }
        if(geometry->x0>=KSN_PROC_EXT_BLEND_ELLIPSOIDS-480){
            ksn_proc_blend_ellipsoids p;unsigned used;
            if(!ksn_proc_blend_read(frame->segments+*i,frame->count-*i,&p,&used)){
                *i=frame->count+1u;return false;
            }
            *i+=used;
            int left,top,right,bottom;
            const bool valid=ksn_proc_blend_bounds(&p,&left,&top,&right,&bottom);
            geometry->x0=(int16_t)(geometry->x0-KSN_PROC_EXT_BLEND_ELLIPSOIDS);
            if(!valid||left!=geometry->x0||top!=geometry->y0||right!=geometry->x1||bottom!=geometry->y1||left>right||top>bottom){
                *i=frame->count+1u;return false;
            }
            *cost=(unsigned)(right-left+1)*(unsigned)(bottom-top+1)*p.header.count;
            return true;
        }
        if(geometry->x0>=KSN_PROC_EXT_ELLIPSOID-480){
            if(KSN_PROC_ELLIPSOID_WORDS>frame->count-*i){*i=frame->count+1u;return false;}
            ksn_proc_ellipsoid p;memcpy(&p,frame->segments+*i,sizeof p);
            int left,top,right,bottom;
            const bool valid=ksn_proc_ellipsoid_bounds(&p,&left,&top,&right,&bottom);
            *i+=(unsigned)KSN_PROC_ELLIPSOID_WORDS;
            geometry->x0=(int16_t)(geometry->x0-KSN_PROC_EXT_ELLIPSOID);
            if(!valid||left!=geometry->x0||top!=geometry->y0||right!=geometry->x1||bottom!=geometry->y1||left>right||top>bottom){
                *i=frame->count+1u;return false;
            }
            *cost=(unsigned)(geometry->x1-geometry->x0+1)*(unsigned)(geometry->y1-geometry->y0+1);
            return true;
        }
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
bool ksn_proc_render_band(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height);
#endif
