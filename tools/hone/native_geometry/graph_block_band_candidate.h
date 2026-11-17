#ifndef HONE_GRAPH_BLOCK_BAND_CANDIDATE_H
#define HONE_GRAPH_BLOCK_BAND_CANDIDATE_H
#include "graph_index_band_candidate.h"

/* Nearest first. Input IDs are in original packet order, so equal z and
 * equal order keep the earlier packet nearer. A fifth survivor only matters
 * if none of the first four fully occludes it. */
typedef struct {
    hone_graph_ordered near[4];
    unsigned count;
} hone_graph_block_pixel;

static inline void hone_graph_block_insert(hone_graph_block_pixel *pixel,
                                           hone_graph_fragment f, uint32_t order) {
    unsigned n = pixel->count < 4 ? pixel->count : 4;
    unsigned pos = 0;
    while (pos < n) {
        const hone_graph_ordered *old = &pixel->near[pos];
        if (f.z > old->fragment.z || (f.z == old->fragment.z && order < old->order)) break;
        pos++;
    }
    if (pos < 4) {
        unsigned last = n < 4 ? n : 3;
        while (last > pos) {
            pixel->near[last] = pixel->near[last-1];
            last--;
        }
        pixel->near[pos].fragment = f;
        pixel->near[pos].order = order;
    }
    if (pixel->count < 5) pixel->count++;
}

static inline int hone_graph_block_compose(const hone_graph_block_pixel *pixel, uint16_t *color) {
    unsigned n = pixel->count;
    for (unsigned j = 0; j < n && j < 4; j++) {
        if (pixel->near[j].fragment.alpha == 1) {
            n = j + 1;
            break;
        }
    }
    if (n > 4) return 0;
    float rgb[3] = {(*color >> 11)/31.f, ((*color >> 5)&63)/63.f, (*color&31)/31.f};
    while (n) {
        const hone_graph_fragment *f = &pixel->near[--n].fragment;
        for (unsigned k = 0; k < 3; k++) rgb[k] = f->rgb[k]*f->alpha + rgb[k]*(1-f->alpha);
    }
    *color = (uint16_t)(((unsigned)floorf(rgb[0]*31+.5f)<<11) |
                       ((unsigned)floorf(rgb[1]*63+.5f)<<5) | (unsigned)floorf(rgb[2]*31+.5f));
    return 1;
}

/* Internal row kernel. Public render/preflight below validate all pointers,
 * graph revision and mutable buffers. Stack workspace is 16 bounded pixels. */
static inline int hone_graph_block_row(const ksn_mesh_snapshot *opaque, const hone_graph_band_payload *p,
                                      const hone_graph_band_index *index, hone_graph_band_work *w, unsigned y) {
    memset(w->row, 0, 240*sizeof *w->row);
    if (!ksn_mesh_render(opaque,w->row,y,1,w->depth,1)) return 0;
    size_t begin = index->offsets[y/8], end = index->offsets[y/8+1];
    if (end < begin || end > index->used || index->used > index->ids_capacity) return 0;
    unsigned left = 240, right = 0;
    hone_graph_block_pixel block[16];
    for (unsigned start = 0; start < 240; start += 16) {
        for (unsigned j = 0; j < 16; j++) {
            float z = w->depth[start+j];
            if (isnan(z) || z == INFINITY) return 0;
            block[j].count = 0;
        }
        for (size_t at = begin; at < end; at++) {
            unsigned id = index->ids[at];
            if (id >= p->graph.count) return 0;
            hone_graph_bounds b = index->bounds[id];
            if (y < b.y0 || y > b.y1) continue;
            unsigned first = b.x0 > start ? b.x0 : start;
            unsigned last = b.x1 < start+15 ? b.x1 : start+15;
            const hone_graph_packet *packet = &p->graph.packets[id];
            for (unsigned x = first; x <= last; x++) {
                hone_graph_fragment f;
                int status = hone_graph_fragment_at(&packet->triangle,x,y,w->depth[x],p->graph.drivers,
                             p->graph.driver_count,w->scratch,w->scratch_count,&f);
                if (status < 0) return 0;
                if (status) hone_graph_block_insert(&block[x-start],f,packet->order);
            }
        }
        for (unsigned j = 0; j < 16; j++) {
            if (!block[j].count) continue;
            unsigned x = start+j;
            if (!hone_graph_block_compose(&block[j], &w->row[x])) return 0;
            if (x < left) left = x;
            right = x+1;
        }
    }
    return !right || right-left <= p->span_limit;
}

static inline int hone_graph_block_band_preflight(const ksn_mesh_snapshot *opaque,const hone_graph_band_payload *p,
        const hone_graph_band_index *index,uint64_t revision,hone_graph_band_work *w) {
    if (!hone_graph_index_band_valid(opaque,p,index,revision,w,NULL,0)) return 0;
    for (unsigned y = 0; y < 135; y++) if (!hone_graph_block_row(opaque,p,index,w,y)) return 0;
    return 1;
}

static inline int hone_graph_block_band_render(const ksn_mesh_snapshot *opaque,const hone_graph_band_payload *p,
        const hone_graph_band_index *index,uint64_t revision,hone_graph_band_work *w,unsigned y,unsigned rows,uint16_t *pixels) {
    if (y > 135 || rows > 8 || rows > 135-y ||
        !hone_graph_index_band_valid(opaque,p,index,revision,w,pixels,(size_t)rows*240)) return 0;
    for (unsigned j = 0; j < rows; j++) {
        if (!hone_graph_block_row(opaque,p,index,w,y+j)) return 0;
        memcpy(pixels+(size_t)j*240,w->row,240*sizeof *pixels);
    }
    return 1;
}
#endif
