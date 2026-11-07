#ifndef AYATSURI_COMPACT_OPAQUE_H
#define AYATSURI_COMPACT_OPAQUE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Experimental owned opaque packet representation, NOT a legacy segment packet.
 * No allocation, hidden capacity policy, quantization, renderer or JS API changes.
 * Caller owns storage/lifetime/accounting and must copy before borrowed inputs mutate.
 * Holes/alpha/clips are not represented: caller must reject those before append. */
enum { CO_MAX_VERTICES=32, CO_MAX_TRIANGLES=64, CO_MAX_PACKETS=256 };
typedef struct { float x,y,mask,depth; uint16_t color_a,color_b; } co_vertex;
typedef struct {
    const co_vertex *vertices;
    const uint16_t *indices;
    uint16_t node;
    uint8_t vertex_count,triangle_count;
} co_input;
/* Explicit mode2 retains front/back A/B colors plus selected face-side bits.
 * The unused high bits in the last side byte must be zero. Sources are
 * synchronous, disjoint borrows, with2*vertex_count back colors and ceil(T/8)
 * side bytes. No native/source pointer is retained in a record. */
typedef struct { co_input front; const uint16_t *back_colors; const uint8_t *back_faces; } co_sided_input;
typedef struct { uint8_t *data; size_t capacity,size; unsigned packets; } co_buffer;
typedef struct {
    const uint8_t *data; size_t size;
    uint16_t node,color;
    uint8_t vertex_count,triangle_count,mode;
} co_view;
/* Rejection leaves buffer bytes, size and packet count unchanged. Distinct,
 * non-overlapping source and destination spans are required. */
bool co_append(co_buffer *buffer,const co_input *input);
bool co_append_sided(co_buffer *buffer,const co_sided_input *input);
/* Validate one complete record at the start of a span. view.size is its length.
 * Reject malformed fields, nonfinite values, bad indices and nonzero padding. */
bool co_read(const uint8_t *data,size_t available,co_view *view);
bool co_vertex_at(const co_view *view,unsigned index,co_vertex *vertex);
bool co_vertex_at_side(const co_view *view,unsigned index,bool back,co_vertex *vertex);
bool co_triangle_side(const co_view *view,unsigned index,bool *back);
bool co_triangle_at(const co_view *view,unsigned index,uint8_t indices[3]);
#endif
