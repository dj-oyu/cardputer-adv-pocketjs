#ifndef HONE_TRACK_MODEL_H
#define HONE_TRACK_MODEL_H
#include <stdint.h>

/* Exported constant data. Channel 0 denotes a constant coordinate or color.
 * For a numeric channel at a key, coordinate = base + scale * signed delta.
 * x/y are screen pixels in Q4, z is signed integer depth (larger is nearer).
 * A vertex may be offscreen within the admitted Q4 range [-4096,8191].
 * Group visibility is packed LSB first. Triangles have two visible sides and
 * use the source order to resolve equal depths. Color tracks store RGB565. */
typedef struct { int16_t base[3]; uint16_t channel[3]; } hone_track_vertex;
typedef struct { uint16_t vertex[3], rgb565; uint8_t group; uint16_t color_channel; } hone_track_triangle;
typedef struct { uint16_t first, second; uint8_t blend; } hone_track_pose;
#endif
