#ifndef KSN_DEPTH_LINES_H
#define KSN_DEPTH_LINES_H

#include "ksn_ports.h"
#include <stdbool.h>
#include <stdint.h>

#define KSN_DEPTH_LINES_MAX 256u
#define KSN_DEPTH_RASTER_MAX 8192u

typedef struct {
    int16_t x0,y0,x1,y1;
    uint16_t z0,z1,color;
} ksn_depth_line;
typedef struct {
    ksn_depth_line lines[KSN_DEPTH_LINES_MAX];
    uint16_t count,raster_steps;
} ksn_depth_frame;
typedef struct {
    const ksn_depth_frame *frame;
    uint16_t width,height,cached_y;
    bool cached;
    uint16_t color[240],depth[240];
    uint8_t alpha[240];
} ksn_depth_image;

void ksn_depth_frame_reset(ksn_depth_frame *frame);
/* Smaller z is nearer; 65535 is reserved for the empty depth row. */
bool ksn_depth_add(ksn_depth_frame *frame,ksn_depth_line line);
/* The frame remains owner-owned and immutable until the image is rebound or
 * the Kasane resource has finished every pending/repair presentation. */
bool ksn_depth_image_init(ksn_depth_image *image,const ksn_depth_frame *frame,
                          uint16_t width,uint16_t height);
void ksn_depth_image_bind(ksn_depth_image *image,const ksn_depth_frame *frame);
void ksn_depth_image_port(ksn_depth_image *image,ksn_image_port *out);

#endif
